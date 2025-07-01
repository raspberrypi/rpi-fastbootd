#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <sys/stat.h>
#include <string>
#include <random>

#include "rpiparted.h"
#include "idpdevice.h"
#include "idpcrypt.h"
#include "utility.h"


#define ERR(msg) std::cerr << "IDP: ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl
#define MSG(msg) std::cout << "IDP: " << msg << std::endl


#ifdef WITH_FASTBOOT
#include "device/commands.h"
#include "device/fastboot_device.h"

// Fastboot specifics can be added here

#endif


// Opaque struct to manage access to provisioned block devices
struct IDPcookie {
    size_t index = 0;
    uint64_t session_id = 0;
};


void IDPcookieDeleter(IDPcookie* ptr) {
    if (ptr)
       delete ptr;
}


namespace DeviceValidation {

   std::string translateDeviceClass(const std::string& device_class) {
      static const std::map<std::string, std::string> device_translations = {
         {"pi4", "Raspberry Pi 4"},
         {"cm4", "Raspberry Pi Compute Module 4"},
         {"pi5", "Raspberry Pi 5"},
         {"cm5", "Raspberry Pi Compute Module 5"},
         {"zero2w", "Raspberry Pi Zero 2 W"}
      };

      auto it = device_translations.find(device_class);
      if (it != device_translations.end()) {
         return it->second;
      }
      return device_class;
   }

   bool checkDeviceClass(const std::string& expected_class) {
      try {
         std::ifstream model_file("/proc/device-tree/model");
         if (!model_file.is_open()) {
            ERR("Failed to open DT device model");
            return false;
         }

         std::string actual_model;
         std::getline(model_file, actual_model);

         std::string translated_class = translateDeviceClass(expected_class);
         std::string expected_lower = translated_class;
         std::string actual_lower = actual_model;
         std::transform(expected_lower.begin(), expected_lower.end(), expected_lower.begin(), ::tolower);
         std::transform(actual_lower.begin(), actual_lower.end(), actual_lower.begin(), ::tolower);

         if (actual_lower.find(expected_lower) == std::string::npos) {
            ERR("Device class mismatch. Wanted: " << translated_class
               << " (" << expected_class << ")"
               << ". Got: " << actual_model);
            return false;
         }

         MSG("Detected: " << actual_model);
         return true;

      } catch (const std::exception& e) {
         ERR("Error checking device class: " << e.what());
         return false;
      }
   }
}


// Storage Public  Methods


std::optional<uint64_t> IDPstorage::capacityBytes() const
{
   std::string msg;

   if (!deviceExist()) {
      msg = (std::string(typeString()) +
            " storage not found on device");
      ERR(msg);
      return std::nullopt;
   }

   std::string dev = std::string(BlockDev());
   if (dev.empty())
      return std::nullopt;

   std::string block_name;

   // Extract block device name from path, eg mmcblk0 from /dev/mmcblk0
   auto pos = dev.rfind('/');
   if (pos != std::string::npos)
      block_name = dev.substr(pos + 1);
   else
      block_name = dev;

   // sysfs attributes to establish capacity
   std::string size_path = "/sys/block/" + block_name + "/size";
   std::string sector_size_path = "/sys/block/" + block_name + "/queue/logical_block_size";

   // Get number of sectors
   std::ifstream size_file(size_path);
   if (!size_file)
      return std::nullopt;

   uint64_t sectors = 0;
   size_file >> sectors;

   // Get sector size, default to 512
   uint64_t sector_size = 512;
   std::ifstream sector_file(sector_size_path);
   if (sector_file) sector_file >> sector_size;

   if (sector_size != sectorSize()) {
      ERR("sector size mismatch: " << sector_size << "/" << sectorSize());
      return std::nullopt;
   }

   return sectors * sector_size;
}


std::uint64_t IDPstorage::alignUp(std::uint64_t size) const
{
   if (!ptable_align)
      return 0;

   std::uint64_t align64 = static_cast<std::uint64_t>(ptable_align);

   if (size > (UINT64_MAX - align64 + 1)) {
      throw std::overflow_error("Size too large for alignment");
   }

   return ((size + align64 - 1) / align64) * align64;
}



// Device Public Methods


IDPdevice::IDPdevice(std::optional<IDPkeyfile> lukskey)
  : state_(IDPstate::Init),
   writer(this),
   lukskey_(std::move(lukskey))
{
   if (lukskey_) {
      if (!std::filesystem::exists(*lukskey_)) {
         throw std::runtime_error("LUKS key file does not exist: " + lukskey_->string());
      }
   }
   std::random_device rd;
   std::mt19937_64 gen(rd());
   session_id_ = gen();
}


IDPdevice::~IDPdevice() = default;


bool IDPdevice::Initialise(const char* jdata, size_t length)
{
   if (getState() == IDPstate::Writing)
      return false;

   if (!jdata || !length)
      return false;

   if (!parser_.loadData(jdata, length)) {
      ERR("Parser data validation failed");
      return false;
   }

   if (!parser_.getImage(image_)) {
      ERR("No image");
      return false;
   }

   partitions_ = parser_.getPartitions();
   if (partitions_.empty()) {
      ERR("No partitions");
      return false;
   }

   setState(IDPstate::Config);
   return true;
}


bool IDPdevice::canProvision()
{
   if (getState() != IDPstate::Config)
      return false;

   if (!validateDeviceIdent()) {
      ERR("Device identification error");
      return false;
   }

   if (!validateDeviceReadiness()) {
      ERR("Device is not ready");
      return false;
   }

   if (!validateDeviceStorage()) {
      ERR("Device storage error");
      return false;
   }

   if (!validateProvisioningIntent()) {
      ERR("Device provisioning intent denied");
      return false;
   }

   std::string msg = ("Initialised on " +
         std::string(image_.device_storage.BlockDev()) +
         " (" +
         std::string(image_.device_storage.typeString()) +
         ") [" +
         std::to_string(isize_) +
         " / " +
         std::to_string(dcap_) +
         "]");

   MSG(msg);
   setState(IDPstate::Ready);
   return true;
}


bool IDPdevice::startProvision()
{
   bool ret;

   if (getState() != IDPstate::Ready) {
      ERR("Not ready");
      return false;
   }

   MSG("writing!");

   // No going back now
   setState(IDPstate::Writing);

   ret = writer.PrepareStorage();

   if (!ret) {
      ERR("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
      ERR("!! FAILED to prepare device storage !!");
      ERR("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
      setState(IDPstate::Error);
   }

   setState(IDPstate::Partitioned);
   return ret;
}


bool IDPdevice::endProvision()
{
   if (getState() != IDPstate::Partitioned) {
      ERR("Storage provisioning completed with errors");
      // TODO hook up logging or callout to hook / callback?
   } else {
      // Likewise
   }

   if (writer.FinaliseStorage()) {
      setState(IDPstate::Complete);
   } else {
      setState(IDPstate::Error);
   }

   return (getState() == IDPstate::Complete);
}


IDPdevice::CookiePtr IDPdevice::createCookie() const
{
   auto cookie = new IDPcookie();
   cookie->session_id = session_id_;
   return CookiePtr(cookie, IDPcookieDeleter);
}


std::optional<IDPblockDevice> IDPdevice::getNextBlockDevice(IDPcookie& cookie) const
{
   if (cookie.session_id != session_id_) {
      ERR("bad cookie");
      return std::nullopt;
   }

   if (getState() != IDPstate::Partitioned) {
      ERR("Not partitioned");
      return std::nullopt;
   }

   while (cookie.index < partitions_.size()) {
      const auto& part = partitions_[cookie.index++];

      if (part.isCryptContainer()) {
         continue; // We deal in partitionable block devices here.
                   // An encrypted partition is handled as a separate
                   // IDP partition object to a LUKS container.
      }

#ifdef WITH_FASTBOOT
      if (!part.simg) {
         ERR("no simg");
         return std::nullopt;
      }
      if (!part.simg.has_value()) {
         ERR("bad simg");
         return std::nullopt;
      }
      std::string simg = *part.simg;
#endif

      std::string blkdev = part.getBlockDev();
      if (blkdev.empty()) {
         ERR("no blockdev");
         return std::nullopt;
      }

      return IDPblockDevice{blkdev, simg};
   }

   return std::nullopt;
}


void IDPdevice::resetCookie(IDPcookie& cookie) const
{
   if (cookie.session_id != session_id_) {
      ERR("bad cookie");
      return;
   }
}


/// Device Private Methods


bool IDPdevice::validateDeviceIdent() const
{
   return DeviceValidation::checkDeviceClass(image_.device_class);
}


bool IDPdevice::validateDeviceStorage()
{
   std::optional<uint64_t> cap = image_.device_storage.capacityBytes();
   if (!cap) {
      ERR("Couldn't determine device storage capacity");
      return false;
   }
   dcap_ = *cap;

   isize_ = computeImageStorageSize();
   if (!dcap_) {
      ERR("Couldn't determine image storage size");
      return false;
   }

   std::string msg;

   if (isize_ > dcap_) {
      std::string msg = ("Image storage size " +
            std::to_string(isize_) +
            " exceeds that of " +
            std::string(image_.device_storage.BlockDev()) +
            " (" +
            std::to_string(dcap_) +
            ")");
      ERR(msg);
      return false;
   }

   return true;
}


bool IDPdevice::validateProvisioningIntent() const
{
   bool approved = true;

   // Check we're happy to provision the device with the features
   // and configuration described.

   for (const auto& part : partitions_) {
      if (part.getParentIndex() == -1) {
         if (part.isCryptContainer()) {
            if (!lukskey_ || !std::filesystem::exists(*lukskey_)) {
               ERR("Encrypted container requested, but no LUKS key provided");
               approved = false;
               break;
            }

            if (std::filesystem::file_size(*lukskey_) == 0) {
               ERR("LUKS key file is empty: " << lukskey_->string());
               approved = false;
               break;
            }

         }
      }
   }

   return approved;
}


bool IDPdevice::validateDeviceReadiness() const
{
   // Wait 1s max for the storage we're going to provision to be ready
   if (!utils::waitBlockDev(image_.device_storage.BlockDev(), 1)) {
      ERR("Device storage is not available for provisioning: " <<
            image_.device_storage.BlockDev());
      return false;
   }

   // Make sure the kernel doesn't think the storage device we're going to
   // write to during provisioning is in use. We can't proceed safely unless
   // this check passes.
   if (!utils::ReReadPartitionTable(image_.device_storage.BlockDev())) {
      ERR("Device storage for provisioning is in use: " <<
            image_.device_storage.BlockDev());
      return false;
   }

   return true;
}


std::uint64_t IDPdevice::computeImageStorageSize() const
{
   std::uint64_t size = 0;

   // Image size equals all root partitions (aligned) plus partition table.
   for (const auto& part : partitions_) {
      if (part.getParentIndex() == -1)
         size += part.getSize(true, partitions_);
   }

   if (image_.device_storage.pTableType() == IDPptable_type::DOS)
      size += image_.device_storage.pAlignment(); // Single MBR

   if (image_.device_storage.pTableType() == IDPptable_type::GPT)
      size += image_.device_storage.pAlignment() * 2; // Main GPT + backup

   return size;
}


bool IDPdevice::setPartBlockDev(IDPpartition * part, std::string dev)
{
   if (dev.empty())
      return false;

   if (utils::waitBlockDev(dev)) {
      part->setBlockDev(dev);
      return true;
   }
   ERR("Not a block device: " << dev);
   return false;
}


bool IDPdevice::setState (IDPstate s)
{
   // @TODO as needed - handle state transition entry/exit
   // device init can install handlers for transitioning, etc.
   state_ = s;
   return true;
}


// deviceWriter Public Methods


IDPdeviceWriter::IDPdeviceWriter(IDPdevice* device)
    : device_(device)
{
   if (!device_) {
      throw std::invalid_argument("IDPdeviceWriter requires an IDPdevice");
   }
}


IDPdeviceWriter::~IDPdeviceWriter() = default;


bool IDPdeviceWriter::PrepareStorage()
{
   return (InitPhysicalPartitions() &&
           InitCryptPartitions()) ? true : false;
}


bool IDPdeviceWriter::FinaliseStorage()
{
   CloseCryptPartitions();
   return true;
}


// deviceWriter Private


bool IDPdeviceWriter::WritePhysicalPartitions()
{
   bool ret = false;
   RPIparted parted;
   std::string msg;

   std::string type =
      device_->image_.device_storage.ptable_type == IDPptable_type::DOS ?
      "DOS" : "GPT";

   msg = ("Creating new " +
         type +
         " partition table on " +
         device_->image_.device_storage.BlockDev()); MSG(msg);

   // Create new partition table
   ret = parted.openDevice(device_->image_.device_storage.BlockDev(),
         device_->image_.device_storage.ptable_align / 1024);

  if (!ret) {
      ERR("Unable to open device for partitioning");
      return false;
   }

   ret = parted.createPartitionTable(
         device_->image_.device_storage.ptable_type == IDPptable_type::DOS ?
         "DOS" : "GPT", std::nullopt);

   if (!ret) {
      ERR("Failed to create partition table");
      parted.closeDevice();
      return false;
   }

   // Create partitions
   for (auto& part : device_->partitions_) {
      if (part.getParentIndex() == -1) {

         // When computing the size of a partition, ensure everything uses
         // aligned sizes. This is especially important if sizing a LUKS
         // container which can have any number of encapsulated children.
         uint64_t sz = part.getSize(true, device_->partitions_);

         msg = ("Creating p" +
               std::to_string(part.num) +
               " on " +
               device_->image_.device_storage.BlockDev() +
               " size (bytes) " +
               std::to_string(sz)); MSG(msg);

         // API auto aligns
         ret = parted.appendPartition(sz, *part.pcode);

         if (!ret)
            break;
      }
   }

   if (!ret)
      ERR("Errors encountered during partitioning");
   else
      ret = parted.commit(); // Write partition table to disk

   (void)parted.closeDevice();
   return ret;
}


bool IDPdeviceWriter::InitPhysicalPartitions()
{
   bool ret = WritePhysicalPartitions();

   if (!ret) {
      ERR("Failed to create physical partitions");
      return false;
   }

   // Wait for a successful re-read of the partition table. This should
   // guarantee kernel, udev etc have updated device nodes, internal states, etc
   if (!utils::WaitReReadPartitionTable(device_->image_.device_storage.BlockDev())) {
      ERR("Timed out re-reading partition table on storage device: " <<
            device_->image_.device_storage.BlockDev());
      return false;
   }

   for (auto& part : device_->partitions_) {
      if (part.getParentIndex() == -1) {
         // Assign corresponding block device to all root partitions we created.
         // It would be much better if we could retrieve this from the parted API
         // via probe, rather than assigning the device we expect.
         ret = device_->setPartBlockDev(&part, device_->image_.device_storage.BlockDev(part.num));
         if (!ret)
            break;
      }
   }

   if (!ret)
      ERR("Failed to probe physical partitions");

   return ret;
}


bool IDPdeviceWriter::InitCryptPartitions()
{
   bool ret = true;

   // Create LUKS containers, initialise and open all encrypted children
   for (auto& part : device_->partitions_) {
      if (part.isCryptContainer()) {

         ret = part.luks->Create(device_->image_.device_storage.BlockDev(part.num), *device_->lukskey_);
         if (!ret)
            break;

         ret = part.luks->Open(device_->image_.device_storage.BlockDev(part.num), *device_->lukskey_);
         if (!ret)
            break;

         if (part.hasChildren(device_->partitions_)) {
            // Create GPT on the LUKS block device if using a partitioned scheme
            if (part.luks->etype == IDPluks::encap_type::Partitioned) {
               RPIparted parted;
               std::string msg;

               bool ret = parted.openDevice(part.luks->BlockDev(0),
                     device_->image_.device_storage.ptable_align / 1024);

               if (!ret)
                  break;

               msg = ("Creating new GPT partition table on " +
                     part.luks->BlockDev(0)); MSG(msg);

               ret = parted.createPartitionTable("GPT", std::nullopt);

               if (!ret) {
                  parted.closeDevice();
                  break;
               }

               // Add partitions
               for (auto& child : device_->partitions_) {
                  if (child.getParentIndex() == part.getIndex() &&
                        child.isEncrypted(device_->partitions_)) {
                     uint64_t sz = child.getSize(false, device_->partitions_);

                     msg = ("Creating p" +
                           std::to_string(child.num) +
                           " on " +
                           part.luks->BlockDev(0) +
                           " size (bytes) " +
                           std::to_string(sz)); MSG(msg);

                     ret = parted.appendPartition(sz,*child.pcode);
                  }
                  if (!ret)
                     break;
               }

               if (ret)
                  parted.commit();

               parted.closeDevice();

               // Necessary to create the device mapper nodes for the block
               // devices associated to the LUKS partitions. BLKRRPART alone is
               // insufficient.
               if (ret) {
                  std::string bin = "kpartx";
                  std::string device = part.luks->BlockDev(0);
                  char *argv[] = {
                     const_cast<char*>(bin.c_str()),
                     const_cast<char*>("-a"),
                     const_cast<char*>(device.c_str()),
                     nullptr
                  };
                  int retcode;
                  (void)utils::process_spawn_blocking(&retcode, bin, argv, nullptr, nullptr);
               }
            }

            // Probe and attach block devices for encrypted children
            for (auto& child : device_->partitions_) {
               if (child.getParentIndex() == part.getIndex() &&
                     child.isEncrypted(device_->partitions_)) {

                  ret = device_->setPartBlockDev(&child, part.luks->BlockDev(child.num));
                  if (!ret)
                     break;
               }
            }
         }
      }
      if (!ret)
         break;
   }

   if (!ret)
      ERR("Errors encountered during crypt init");

   return ret;
}


void IDPdeviceWriter::CloseCryptPartitions()
{
   for (auto& part : device_->partitions_) {
      if (part.isCryptContainer()) {
         if (part.hasChildren(device_->partitions_)) {
            for (auto& child : device_->partitions_) {
               if (child.getParentIndex() == part.getIndex() &&
                     child.isEncrypted(device_->partitions_)) {
                  part.luks->Close(part.luks->BlockDev(child.num));
                  child.setBlockDev(std::string());
               }
            }
         }
         part.luks->Close();
         part.setBlockDev(std::string());

      }
   }
}
