#include "rpiparted.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>


#define ERR(msg) std::cerr << "ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl
#define MSG(msg) std::cout << "MSG: " << msg << std::endl

#define ASSERT(cond, msg) do { if (!(cond)) { ERR(msg); std::abort(); } } while(0)


FdiskContextDeleter::FdiskContextDeleter(bool* assigned)
    : assigned_(assigned) {}


void FdiskContextDeleter::operator()(struct fdisk_context* ctx) const {
   if (ctx) {
      if (assigned_ && *assigned_) {
         if (fdisk_device_is_used(ctx)) {
            fdisk_deassign_device(ctx, 1);  // force cleanup
         }
         *assigned_ = false;
      }
      fdisk_unref_context(ctx);
   }
}


RPIparted::RPIparted()
    : context_(fdisk_new_context(), FdiskContextDeleter(&device_assigned_)) {
    if (!context_) {
        throw std::runtime_error("Failed to create context");
    }
    fdisk_init_debug(0);
}


bool RPIparted::openDevice(const std::string& device_path, unsigned long align_kb) {
   closeDevice();
   context_.reset(fdisk_new_context());
   if (!context_) {
      ERR("Failed to create context");
      return false;
   }

   fdisk_disable_dialogs(context_.get(), 1);

   if (align_kb)
      fdisk_save_user_grain(context_.get(), align_kb * 1024);

   if (fdisk_assign_device(context_.get(), device_path.c_str(), 0)) {
      device_assigned_ = false;
      context_.reset();
      ERR("Failed to open device " << device_path);
      return false;
   }
   device_assigned_ = true;;

   grain_ = fdisk_get_grain_size(context_.get());

   sector_size_ = fdisk_get_sector_size(context_.get());
   if (sector_size_ == 0) {
      ERR("Bad sector size");
      return false;
   }

   is_gpt_ = false;

   if (fdisk_get_label(context_.get(), NULL))
      if (fdisk_is_label(context_.get(), GPT))
         is_gpt_ = true;

   return true;
}


void RPIparted::closeDevice() {
   if (context_) {
      if (device_assigned_ && fdisk_device_is_used(context_.get())) {
         fdisk_deassign_device(context_.get(), 0);
         device_assigned_ = false;
      }
      context_.reset(); // Destroys smart pointer's object, triggering deleter
   }
   return;
}


bool RPIparted::createPartitionTable(const std::string& type, const std::optional<std::string>& id) {
   if (!context_) {
      ERR("Bad context");
      return false;
   }

   std::string type_lower = type;
   std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(), ::tolower);
   if (type_lower != "gpt" && type_lower != "dos" && type_lower != "mbr") {
      ERR("Invalid partition table type: need gpt or dos/mbr");
      return false;
   }

   const char* label_type = (type_lower == "mbr") ? "dos" : type_lower.c_str();

   if (fdisk_create_disklabel(context_.get(), label_type) != 0) {
      ERR("Failed to create label " << type);
      return false;
   }

   if (id && !id->empty() && \
         fdisk_set_disklabel_id_from_string(context_.get(), id->c_str())) {
      ERR("Invalid label ID " << id->c_str());
      return false;
   }

   is_gpt_ = (type_lower == "gpt");

   return true;
}


bool RPIparted::appendPartition(const uint64_t size_bytes, const std::string& type) {
   if (!context_) {
      ERR("Bad context");
      return false;
   }

   if (type.empty()) {
      ERR("Bad partition type");
      return false;
   }

   struct fdisk_label *label = fdisk_get_label(context_.get(), NULL);
   if (!label) {
      ERR("Partition table not initialised");
      return false;
   }

   fdisk_partition* new_part = fdisk_new_partition();
   ASSERT(new_part != NULL, "Failed to create new partition object");
   fdisk_reset_partition(new_part);

   // Defaults
   fdisk_partition_start_follow_default(new_part, 1);
   fdisk_partition_end_follow_default(new_part, 1);
   fdisk_partition_partno_follow_default(new_part, 1);

   if (size_bytes) {
      // Minimum number of sectors required to accomodate the requested size
      uint64_t size_sectors = (size_bytes + sector_size_ - 1) / sector_size_;

      // Alignment grain in sectors
      uint64_t grain_sectors = grain_ / sector_size_;

      // Pad by one grain to ensure we always allocate enough space
      uint64_t padded_size = size_sectors + grain_sectors;

      fdisk_partition_set_size(new_part, padded_size);
      fdisk_partition_end_follow_default(new_part, 0);
   }

   ASSERT(fdisk_partition_has_start(new_part) == 0, "No start");
   ASSERT(fdisk_partition_has_end(new_part) == 0, "No end");
   ASSERT(fdisk_partition_has_partno(new_part) == 0, "No partno");

   struct fdisk_parttype* part_type;

   if (is_gpt_) {
      if (!fdisk_is_label(context_.get(), GPT)) {
         ERR("GPT get fail");
         fdisk_unref_partition(new_part);
         return false;
      }
      part_type = fdisk_label_get_parttype_from_string(label, type.c_str());
      if (!part_type) {
         ERR("Bad GUID: " << type);
         fdisk_unref_partition(new_part);
         return false;
      }
   } else {
      if (!fdisk_is_label(context_.get(), DOS)) {
         ERR("DOS get fail");
         fdisk_unref_partition(new_part);
         return false;
      }

      unsigned int code;

      if (1 != sscanf(type.c_str(), "%x", &code) ||
            !(part_type = fdisk_label_get_parttype_from_code(label, code))) {
         ERR("Bad ptype: " << type);
         fdisk_unref_partition(new_part);
         return false;
      }
   }

   fdisk_partition_set_type(new_part, part_type);
   fdisk_unref_parttype(part_type);

   int ret = fdisk_add_partition(context_.get(), new_part, nullptr);
   fdisk_unref_partition(new_part);

   if (ret != 0) {
      ERR("Failed to add partition: " << ret);
      return false;
   }

   return true;
}


bool RPIparted::removePartition(const size_t partnum) {
   if (!context_) {
      ERR("Bad context");
      return false;
   }

   if (!partnum) {
      ERR("Bad partnum");
      return false;
   }

   int ret = fdisk_delete_partition(context_.get(), partnum-1);
   if (ret != 0) {
      ERR("Failed to remove partition " << partnum << ret);
      return false;
   }

   return true;
}


bool RPIparted::commit() {
   if (!context_) {
      ERR("Bad context");
      return false;
   }

   int ret = fdisk_write_disklabel(context_.get());

   if (ret) {
      ERR("Error writing partition table: "
            << errno << " (" << std::strerror(errno) << ")");
   }

   return (ret == 0) ? true : false;
}


bool RPIparted::rereadPartitionTable() {
   if (!context_) {
      ERR("Bad context");
      return false;
   }

   int ret = fdisk_reread_partition_table(context_.get());

   if (ret != 0) {
      ERR("Error re-reading partition table: "
            << errno << " (" << std::strerror(errno) << ")");
   }

   return (ret == 0) ? true : false;
}
