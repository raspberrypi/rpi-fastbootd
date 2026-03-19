/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utility.h"

#include <cstring>
#include <expected>
#include <iomanip>
#include <sstream>
#include <mutex>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/fs.h>
#include <unistd.h>
#include <spawn.h> // for posix_spawnp
#include <sys/wait.h> // for waitpid

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <rpifwcrypto.h>
#include <openssl/decoder.h>
#include <openssl/encoder.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include "fastboot_device.h"

using namespace std::chrono_literals;
using android::base::unique_fd;

namespace {

bool OpenPhysicalPartition(const std::string& name, PartitionHandle* handle) {
    std::optional<std::string> path = FindPhysicalPartition(name);
    if (!path) {
        return false;
    }
    *handle = PartitionHandle(*path);
    return true;
}

bool OpenLogicalPartition(FastbootDevice* device, const std::string& partition_name,
                          PartitionHandle* handle) {
    // std::string slot_suffix = GetSuperSlotSuffix(device, partition_name);
    // uint32_t slot_number = SlotNumberForSlotSuffix(slot_suffix);
    // auto path = FindPhysicalPartition(fs_mgr_get_super_partition_name(slot_number));
    // if (!path) {
        return false;
    // }

    // CreateLogicalPartitionParams params = {
    //         .block_device = *path,
    //         .metadata_slot = slot_number,
    //         .partition_name = partition_name,
    //         .force_writable = true,
    //         .timeout_ms = 5s,
    // };
    // std::string dm_path;
    // if (!CreateLogicalPartition(params, &dm_path)) {
    //     LOG(ERROR) << "Could not map partition: " << partition_name;
    //     return false;
    // }
    // auto closer = [partition_name]() -> void { DestroyLogicalPartition(partition_name); };
    // *handle = PartitionHandle(dm_path, std::move(closer));
    // return true;
}

}  // namespace

namespace android {
    uint64_t get_block_device_size(int fd) {
        uint64_t size = 0;
        int ret;

        ret = ioctl(fd, BLKGETSIZE64, &size);

        if (ret) return 0;

        return size;
    }

    int wipe_block_device(int fd) {
        uint64_t range[2];
        int ret;
        uint64_t len = get_block_device_size(fd);

        range[0] = 0;
        range[1] = len;

        if (range[1] == 0) return 0;

        ret = ioctl(fd, BLKSECDISCARD, &range);
        if (ret < 0) {
            LOG(ERROR) << "Something went wrong secure discarding block: " << strerror(errno);
            range[0] = 0;
            range[1] = len;
            ret = ioctl(fd, BLKDISCARD, &range);
            if (ret < 0) {
                LOG(ERROR) << " Discard failed: " <<  strerror(errno);
                return -1;
            } else {
                LOG(ERROR) << "Wipe via secure discard failed, used non-secure discard instead";
                return 0;
            }
        }

        return ret;
    }
} // namespace android

namespace rpi {
    // Hardcoded key ID for ARM crypto device private key
    constexpr uint32_t ARM_CRYPTO_DEVICE_PRIVATE_KEY_ID = 1;

    // Static member definitions
    std::expected<bool, RPI_FW_CRYPTO_STATUS> RpiFwCrypto::key_provisioned_status_ = std::unexpected(RPI_FW_CRYPTO_ERROR_UNKNOWN);
    std::once_flag RpiFwCrypto::init_flag_;

    /**
     * Convert DER-encoded key data to PEM format
     * @param der_data The DER-encoded key data as a vector
     * @param is_private_key true for private keys, false for public keys
     * @return std::expected containing PEM string on success, or int error code on failure
     */
    std::expected<std::string, int> RpiFwCrypto::ConvertDerToPem(const std::vector<uint8_t>& der_data, bool is_private_key) {
            // Create decoder context using ternary operator
            int key_selection = is_private_key ? OSSL_KEYMGMT_SELECT_PRIVATE_KEY : OSSL_KEYMGMT_SELECT_PUBLIC_KEY;
            OSSL_DECODER_CTX* decoder_ctx = OSSL_DECODER_CTX_new_for_pkey(nullptr, "DER", nullptr, "EC",
                                                                         key_selection, nullptr, nullptr);

            if (!decoder_ctx) {
                LOG(ERROR) << "Failed to create OSSL decoder context";
                return std::unexpected(-1);
            }

            const unsigned char* der_ptr = der_data.data();
            EVP_PKEY* pkey = nullptr;

            if (is_private_key) {
                pkey = d2i_PrivateKey(EVP_PKEY_EC, nullptr, &der_ptr, der_data.size());
            } else {
                pkey = d2i_PUBKEY(nullptr, &der_ptr, der_data.size());
            }

            OSSL_DECODER_CTX_free(decoder_ctx);

            if (!pkey) {
                LOG(ERROR) << "Failed to decode DER key data";
                return std::unexpected(-2);
            }

            OSSL_ENCODER_CTX* encoder_ctx = OSSL_ENCODER_CTX_new_for_pkey(pkey, key_selection,
                                                                        "PEM", nullptr, nullptr);

            if (!encoder_ctx) {
                LOG(ERROR) << "Failed to create OSSL encoder context";
                EVP_PKEY_free(pkey);
                return std::unexpected(-4);
            }

            // Create BIO for output
            BIO* bio = BIO_new(BIO_s_mem());
            if (!bio) {
                LOG(ERROR) << "Failed to create BIO";
                OSSL_ENCODER_CTX_free(encoder_ctx);
                EVP_PKEY_free(pkey);
                return std::unexpected(-5);
            }

            // Encode to PEM
            if (!OSSL_ENCODER_to_bio(encoder_ctx, bio)) {
                LOG(ERROR) << "Failed to encode key to PEM format";
                BIO_free(bio);
                OSSL_ENCODER_CTX_free(encoder_ctx);
                EVP_PKEY_free(pkey);
                return std::unexpected(-6);
            }

            // Extract PEM string from BIO
            char* pem_data = nullptr;
            long pem_len = BIO_get_mem_data(bio, &pem_data);
            if (pem_len <= 0 || !pem_data) {
                LOG(ERROR) << "Failed to extract PEM data from BIO";
                BIO_free(bio);
                OSSL_ENCODER_CTX_free(encoder_ctx);
                EVP_PKEY_free(pkey);
                return std::unexpected(-7);
            }

            std::string pem_string(pem_data, pem_len);

            // Cleanup
            BIO_free(bio);
            OSSL_ENCODER_CTX_free(encoder_ctx);
            EVP_PKEY_free(pkey);

            return pem_string;
        }

    // Constructor implementation
    RpiFwCrypto::RpiFwCrypto() {
        // Thread-safe initialization of static member on first construction
        std::call_once(init_flag_, []() {
            key_provisioned_status_ = IsKeyProvisioned();
        });
    }

    /**
     * Get the public key for the device private key in PEM format
     * @return std::expected containing the public key as a PEM string on success, or RPI_FW_CRYPTO_STATUS on failure
     */
    std::expected<std::string, RPI_FW_CRYPTO_STATUS> RpiFwCrypto::GetPublicKey() {
        std::vector<uint8_t> pubkey_buffer(RPI_FW_CRYPTO_PUBLIC_KEY_MAX_SIZE);
        size_t pubkey_len = 0;

        int ret = rpi_fw_crypto_get_pubkey(0, ARM_CRYPTO_DEVICE_PRIVATE_KEY_ID,
                                         pubkey_buffer.data(), pubkey_buffer.size(), &pubkey_len);

        if (ret != 0) {
            return std::unexpected(static_cast<RPI_FW_CRYPTO_STATUS>(ret));
        }

        // Resize vector to actual key length
        pubkey_buffer.resize(pubkey_len);

        // Convert DER to PEM format
        auto pem_result = ConvertDerToPem(pubkey_buffer, false);
        if (!pem_result) {
            return std::unexpected(RPI_FW_CRYPTO_OPERATION_FAILED);
        }

        return *pem_result;
    }

    /**
     * Get the private key for the device private key in PEM format
     * @return std::expected containing the private key as a PEM string on success, or RPI_FW_CRYPTO_STATUS on failure
     */
    std::expected<std::string, RPI_FW_CRYPTO_STATUS> RpiFwCrypto::GetPrivateKey() {
        std::vector<uint8_t> private_key_buffer(RPI_FW_CRYPTO_PRIVATE_KEY_MAX_SIZE);
        size_t private_key_len = 0;

        int ret = rpi_fw_crypto_get_private_key(0, ARM_CRYPTO_DEVICE_PRIVATE_KEY_ID,
                                              private_key_buffer.data(), private_key_buffer.size(), &private_key_len);

        if (ret != 0) {
            return std::unexpected(static_cast<RPI_FW_CRYPTO_STATUS>(ret));
        }

        // Resize vector to actual key length
        private_key_buffer.resize(private_key_len);

        // Convert DER to PEM format
        auto pem_result = ConvertDerToPem(private_key_buffer, true);
        if (!pem_result) {
            return std::unexpected(RPI_FW_CRYPTO_OPERATION_FAILED);
        }

        return *pem_result;
    }

    /**
     * Check if a key has already been provisioned in the slot
     * @return std::expected containing true if key is provisioned, false if not provisioned, or RPI_FW_CRYPTO_STATUS on failure
     */
    std::expected<bool, RPI_FW_CRYPTO_STATUS> RpiFwCrypto::IsKeyProvisioned() {
        uint32_t status = 0;
        int ret = rpi_fw_crypto_get_key_status(ARM_CRYPTO_DEVICE_PRIVATE_KEY_ID, &status);
        if (ret != 0) {
            LOG(ERROR) << "Failed to get key status: " << ret;
            return std::unexpected(static_cast<RPI_FW_CRYPTO_STATUS>(ret));
        }

        // Check if the key has been provisioned by checking the device private key flag
        bool is_provisioned = (status & ARM_CRYPTO_KEY_STATUS_TYPE_DEVICE_PRIVATE_KEY) != 0;

        return is_provisioned;
    }

    /**
     * Get the cached key provisioning status from construction
     * @return std::expected containing the cached provisioning status
     */
    std::expected<bool, RPI_FW_CRYPTO_STATUS> RpiFwCrypto::GetCachedProvisioningStatus() {
        return key_provisioned_status_;
    }

    /**
     * Provision a new ECDSA key into the slot
     * @return 0 on success, negative error code on failure
     */
    int RpiFwCrypto::ProvisionKey() {
        return rpi_fw_crypto_gen_ecdsa_key(0, ARM_CRYPTO_DEVICE_PRIVATE_KEY_ID);
    }

    /**
     * Calculate HMAC-SHA256 using the device private key
     * @param message The message to HMAC as a vector
     * @return std::expected containing the HMAC as a lowercase hex string on success, or RPI_FW_CRYPTO_STATUS on failure
     */
    std::expected<std::string, RPI_FW_CRYPTO_STATUS> RpiFwCrypto::CalculateHmac(const std::vector<uint8_t>& message) {
        std::vector<uint8_t> hmac(32); // HMAC-SHA256 is always 32 bytes

        int ret = rpi_fw_crypto_hmac_sha256(0, ARM_CRYPTO_DEVICE_PRIVATE_KEY_ID,
                                          message.data(), message.size(), hmac.data());

        if (ret != 0) {
            return std::unexpected(static_cast<RPI_FW_CRYPTO_STATUS>(ret));
        }

        // Convert to lowercase hex string
        std::ostringstream hex_stream;
        hex_stream << std::hex << std::setfill('0');
        for (uint8_t byte : hmac) {
            hex_stream << std::setw(2) << static_cast<unsigned int>(byte);
        }

        return hex_stream.str();
    }

    /**
     * Get the key status as a human-readable string
     * @return String describing the current key status
     */
    std::string RpiFwCrypto::GetKeyStatusString() {
        uint32_t status = 0;
        int ret = rpi_fw_crypto_get_key_status(ARM_CRYPTO_DEVICE_PRIVATE_KEY_ID, &status);
        if (ret != 0) {
            return "Failed to get key status";
        }
        return std::string(rpi_fw_crypto_key_status_str(status));
    }

    int process_spawn_blocking(int *r, std::string bin, char * const argv[], char * const envp[], posix_spawn_file_actions_t *file_actions) {
        int ret;
        pid_t pid;
        int wstatus;
    
        ret = posix_spawnp(
            &pid,
            bin.c_str(),
            file_actions, // file_actions
            NULL, // spawn_attr
            argv,
            envp
        );
    
        if (ret) return ret;
    
        do {
            ret = waitpid(pid, &wstatus, 0);
            if (ret == -1) break;
        } while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));
    
        if ((ret != -1) && WIFEXITED(wstatus)) {
            ret = 0;
            *r = WEXITSTATUS(wstatus);
        } else {
            ret = -1;
        }
    
        return ret;
    }
}

bool OpenPartition(FastbootDevice* device, const std::string& name, PartitionHandle* handle,
                   int flags) {
    // We prioritize logical partitions over physical ones, and do this
    // consistently for other partition operations (like getvar:partition-size).
    // if (LogicalPartitionExists(device, name)) {
    //     if (!OpenLogicalPartition(device, name, handle)) {
    //         return false;
    //     }
    // } else if (!OpenPhysicalPartition(name, handle)) {
    if (!OpenPhysicalPartition(name, handle)) {
        LOG(ERROR) << "No such partition: " << name;
        return false;
    }

    return true/*handle->Open(flags)*/;
}

std::optional<std::string> FindPhysicalPartition(const std::string& name) {
    // Check for an invalid file name
    if (android::base::StartsWith(name, "../") || name.find("/../") != std::string::npos) {
        LOG(ERROR) << "Attempted path traversal. Aborting.";
        return {};
    }
    std::string path = name;
    // Check for starts with /dev/
    if (!android::base::StartsWith(name, "/dev")) {
        path.insert(0, "/dev/");
    }
    if (access(path.c_str(), W_OK) < 0) {
        LOG(ERROR) << "Cannot write to path: " << path << "with error " << strerror(errno);
        return {};
    }
    return path;
}

// static const LpMetadataPartition* FindLogicalPartition(const LpMetadata& metadata,
//                                                        const std::string& name) {
//     for (const auto& partition : metadata.partitions) {
//         if (GetPartitionName(partition) == name) {
//             return &partition;
//         }
//     }
//     return nullptr;
// }

bool LogicalPartitionExists(FastbootDevice* device, const std::string& name, bool* is_zero_length) {
    // std::string slot_suffix = GetSuperSlotSuffix(device, name);
    // uint32_t slot_number = SlotNumberForSlotSuffix(slot_suffix);
    // auto path = FindPhysicalPartition(fs_mgr_get_super_partition_name(slot_number));
    // if (!path) {
        return false;
    // }

    // std::unique_ptr<LpMetadata> metadata = ReadMetadata(path->c_str(), slot_number);
    // if (!metadata) {
    //     return false;
    // }
    // const LpMetadataPartition* partition = FindLogicalPartition(*metadata.get(), name);
    // if (!partition) {
    //     return false;
    // }
    // if (is_zero_length) {
    //     *is_zero_length = (partition->num_extents == 0);
    // }
    // return true;
}

bool GetSlotNumber(const std::string& slot, int32_t* number) {
    if (slot.size() != 1) {
        return false;
    }
    if (slot[0] < 'a' || slot[0] > 'z') {
        return false;
    }
    *number = slot[0] - 'a';
    return true;
}

std::vector<std::string> ListPartitions(FastbootDevice* device) {
    std::vector<std::string> partitions;

    // First get physical partitions.
    struct dirent* de;
    std::unique_ptr<DIR, decltype(&closedir)> by_name(opendir("/dev"), closedir);
    while ((de = readdir(by_name.get())) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        if (! (
            android::base::StartsWith(de->d_name, "nvme") ||
            android::base::StartsWith(de->d_name, "mmcblk") ||
            android::base::StartsWith(de->d_name, "sd")
        ) ) {
            continue;
        }
        struct stat s;
        std::string path = "/dev/" + std::string(de->d_name);
        if (!stat(path.c_str(), &s) && S_ISBLK(s.st_mode)) {
            partitions.emplace_back(de->d_name);
        }
    }

    // // Find metadata in each super partition (on retrofit devices, there will
    // // be two).
    // std::vector<std::unique_ptr<LpMetadata>> metadata_list;

    // uint32_t current_slot = SlotNumberForSlotSuffix(device->GetCurrentSlot());
    // std::string super_name = fs_mgr_get_super_partition_name(current_slot);
    // if (auto metadata = ReadMetadata(super_name, current_slot)) {
    //     metadata_list.emplace_back(std::move(metadata));
    // }

    // uint32_t other_slot = (current_slot == 0) ? 1 : 0;
    // std::string other_super = fs_mgr_get_super_partition_name(other_slot);
    // if (super_name != other_super) {
    //     if (auto metadata = ReadMetadata(other_super, other_slot)) {
    //         metadata_list.emplace_back(std::move(metadata));
    //     }
    // }

    // for (const auto& metadata : metadata_list) {
    //     for (const auto& partition : metadata->partitions) {
    //         std::string partition_name = GetPartitionName(partition);
    //         if (std::find(partitions.begin(), partitions.end(), partition_name) ==
    //             partitions.end()) {
    //             partitions.emplace_back(partition_name);
    //         }
    //     }
    // }
    return partitions;
}

bool GetDeviceLockStatus() {
    // return android::base::GetProperty("ro.boot.verifiedbootstate", "") != "orange";
    return false;
}

// bool UpdateAllPartitionMetadata(FastbootDevice* device, const std::string& super_name,
//                                 const android::fs_mgr::LpMetadata& metadata) {
//     size_t num_slots = 1;
//     auto boot_control_hal = device->boot_control_hal();
//     if (boot_control_hal) {
//         num_slots = boot_control_hal->GetNumSlots();
//     }

//     bool ok = true;
//     for (size_t i = 0; i < num_slots; i++) {
//         ok &= UpdatePartitionTable(super_name, metadata, i);
//     }
//     return ok;
// }

// std::string GetSuperSlotSuffix(FastbootDevice* device, const std::string& partition_name) {
//     // If the super partition does not have a slot suffix, this is not a
//     // retrofit device, and we should take the current slot.
//     std::string current_slot_suffix = device->GetCurrentSlot();
//     uint32_t current_slot_number = SlotNumberForSlotSuffix(current_slot_suffix);
//     std::string super_partition = fs_mgr_get_super_partition_name(current_slot_number);
//     if (GetPartitionSlotSuffix(super_partition).empty()) {
//         return current_slot_suffix;
//     }

//     // Otherwise, infer the slot from the partition name.
//     std::string slot_suffix = GetPartitionSlotSuffix(partition_name);
//     if (!slot_suffix.empty()) {
//         return slot_suffix;
//     }
//     return current_slot_suffix;
// }

// AutoMountMetadata::AutoMountMetadata() {
//     android::fs_mgr::Fstab proc_mounts;
//     if (!ReadFstabFromFile("/proc/mounts", &proc_mounts)) {
//         LOG(ERROR) << "Could not read /proc/mounts";
//         return;
//     }

//     if (GetEntryForMountPoint(&proc_mounts, "/metadata")) {
//         mounted_ = true;
//         return;
//     }

//     if (!ReadDefaultFstab(&fstab_)) {
//         LOG(ERROR) << "Could not read default fstab";
//         return;
//     }
//     mounted_ = EnsurePathMounted(&fstab_, "/metadata");
//     should_unmount_ = true;
// }

// AutoMountMetadata::~AutoMountMetadata() {
//     if (mounted_ && should_unmount_) {
//         EnsurePathUnmounted(&fstab_, "/metadata");
//     }
// }
