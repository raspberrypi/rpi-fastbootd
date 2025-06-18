#ifndef IDP_CRYPT_H
#define IDP_CRYPT_H

#include <string>
#include <cstddef>
#include <optional>
#include <algorithm>
#include <filesystem>


struct IDPluks {
    enum class encap_type {
       Raw,
       Partitioned
    };
    encap_type etype = encap_type::Raw;

    int version = 2;
    std::string hash;
    std::size_t key_size;
    std::string cipher;
    std::size_t sector_size = 512;
    std::string mname;
    std::optional<std::string> label;

    // Optional overrides
    std::optional<std::size_t> payloadOffsetSectors; // LUKS1
    std::optional<std::size_t> headerSizeBytes;      // LUKS2
    std::size_t pAlignmentBytes = 1024 * 1024;

    // Compute size reserved on disk for the header and metadata, aligned
    std::size_t headerSize() const {
        std::size_t rawSize = 0;

        switch (version) {
            case 1:
                rawSize = payloadOffsetSectors.value_or(2048) * sector_size;
                break;
            case 2:
                rawSize = headerSizeBytes.value_or(16 * 1024 * 1024);
                break;
            default:
                rawSize = 0;
        }
        return alignUp(rawSize, pAlignmentBytes);
    }

    // Summary
    std::string summary() const {
        return ver() + " cipher:" + cipher +
               " keysz:" + std::to_string(key_size) +
               " hash:" + hash +
               " " + std::to_string(sector_size) +
               "/" + std::to_string(pAlignmentBytes);
    }

    // Return the associated block device based on configuration
    std::string BlockDev(std::optional<unsigned int> partnum = std::nullopt) const {
       bool valid = true;
       std::string base = "";

       if (mname.empty())
          return base;

       switch (etype) {
          case encap_type::Raw:
          case encap_type::Partitioned:
             base = "/dev/mapper";
             break;
          default:
             valid = false;
             break;
       }

       if (valid) {
          base += "/" + mname;

          // 60-persistent-storage.rules
          if (etype == encap_type::Partitioned && partnum && *partnum >= 1)
             base += std::to_string(*partnum);
       }
       return base;
    }

    // Device Operations
    bool Create(std::string_view blkdev, std::filesystem::path keyfile) const ;
    bool Open(std::string_view blkdev, std::filesystem::path keyfile) const ;
    bool Close(std::optional<std::string_view> blkdev = std::nullopt) const ;

private:
    static std::size_t alignUp(std::size_t value, std::size_t alignment) {
        return ((value + alignment - 1) / alignment) * alignment;
    }

    std::string ver() const {
        return (version == 1) ? "LUKS1" : "LUKS2";
    }

    std::vector<std::string> luks2createcmd(std::string_view blkdev, std::filesystem::path keyfile) const;
    std::vector<std::string> luks2opencmd(std::string_view blkdev, std::filesystem::path keyfile) const;
    std::vector<std::string> luks2closecmd(std::string_view mname) const;

};

#endif // IDP_CRYPT_H
