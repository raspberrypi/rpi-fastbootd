#ifndef IDP_STORAGE_H
#define IDP_STORAGE_H

#include <string_view>
#include <filesystem>


enum class IDPptable_type {
   DOS,
   GPT,
};


struct IDPstorage {
   enum class storage_type {
        SD,
        EMMC,
        NVME
    };
   storage_type type;

   size_t sector_size;
   size_t ptable_align;
   IDPptable_type ptable_type;

   std::string_view typeString() const {
      switch (type) {
         case storage_type::SD: return "SD";
         case storage_type::EMMC: return "eMMC";
         case storage_type::NVME: return "NVMe";
         default: return "";
      }
   }

   std::string BlockDev(std::optional<unsigned int> partnum = std::nullopt) const {
      bool valid = true;
      std::string base;
      switch (type) {
         case storage_type::SD:
         case storage_type::EMMC:
            base = "/dev/mmcblk0";
            break;
         case storage_type::NVME:
            base = "/dev/nvme0p1";
            break;
         default:
            base = "";
            valid = false;
            break;
      }
      if (valid && partnum && *partnum >= 1)
         base += "p" + std::to_string(*partnum);

      return base;
   }

   bool deviceExist() const {
      std::string dev = std::string(BlockDev());
      if (dev.empty())
         return false;
      else
         return std::filesystem::exists(BlockDev());
   }

   std::optional<uint64_t> capacityBytes() const;
   std::uint64_t alignUp(std::uint64_t size) const;

   size_t sectorSize() const {return sector_size;}
   size_t pAlignment() const {return ptable_align;}
   IDPptable_type pTableType() const {return ptable_type;}
};


struct IDPimage {
   std::string device_class;
   IDPstorage device_storage;
   std::string device_variant;
   std::string version;
   std::string name;
   size_t size;
};


#endif // IDP_STORAGE_H
