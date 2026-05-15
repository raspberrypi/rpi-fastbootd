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
#include "flashing.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <sparse/sparse.h>

#include "fastboot_device.h"
#include "partition_lock_manager.h"
#include "utility.h"

using namespace std::literals;

namespace {

constexpr uint32_t SPARSE_HEADER_MAGIC = 0xed26ff3a;

// Context passed through the libsparse callback's void* priv, so the
// callback can report a specific error string back to the caller.
struct SparseCallbackCtx {
    PartitionHandle* handle;
    uint64_t block_device_size;
    std::string* err;  // may be null
};

void SetErr(std::string* err, const std::string& msg) {
    if (err && err->empty()) {
        *err = msg;
    }
}

}  // namespace

int FlashRawDataChunk(PartitionHandle* handle, const char* data, size_t len, std::string* err) {
    size_t ret = 0;
    const size_t max_write_size = 1048576;
    void* aligned_buffer;
    auto pagesize = sysconf(_SC_PAGESIZE);

    if (posix_memalign(&aligned_buffer, pagesize, max_write_size)) {
        PLOG(ERROR) << "Failed to allocate write buffer";
        SetErr(err, "posix_memalign of " + std::to_string(max_write_size) +
                        "B write buffer failed");
        return -ENOMEM;
    }

    auto aligned_buffer_unique_ptr = std::unique_ptr<void, decltype(&free)>{aligned_buffer, free};

    while (ret < len) {
        int this_len = std::min(max_write_size, len - ret);
        memcpy(aligned_buffer_unique_ptr.get(), data, this_len);
        // In case of non page aligned writes, reopen without O_DIRECT flag
        if (this_len & 0xFFF) {
            if (handle->Reset(O_WRONLY) != true) {
                PLOG(ERROR) << "Failed to reset file descriptor";
                SetErr(err, "failed to drop O_DIRECT on " + handle->path() +
                                " for unaligned tail write");
                return -EIO;
            }
        }

        off64_t offset = lseek64(handle->fd(), 0, SEEK_CUR);
        int this_ret = write(handle->fd(), aligned_buffer_unique_ptr.get(), this_len);
        if (this_ret < 0) {
            int saved = errno;
            PLOG(ERROR) << "Failed to flash data of len " << len;
            SetErr(err, "write(" + std::to_string(this_len) + "B) to " +
                            handle->path() + " at offset " +
                            std::to_string(offset) + " failed: " + strerror(saved));
            errno = saved;
            return -saved;
        }
        data += this_ret;
        ret += this_ret;
    }
    return 0;
}

int FlashRawData(PartitionHandle* handle, const std::vector<char>& downloaded_data,
                 std::string* err) {
    return FlashRawDataChunk(handle, downloaded_data.data(), downloaded_data.size(), err);
}

int WriteCallback(void* priv, const void* data, size_t len) {
    SparseCallbackCtx* ctx = reinterpret_cast<SparseCallbackCtx*>(priv);
    PartitionHandle* handle = ctx->handle;
    if (!data) {
        off64_t before = lseek64(handle->fd(), 0, SEEK_CUR);
        if (lseek64(handle->fd(), len, SEEK_CUR) < 0) {
            int saved = errno;
            PLOG(ERROR) << "lseek failed";
            off64_t target = (before < 0) ? -1 : before + static_cast<off64_t>(len);
            SetErr(ctx->err, "sparse skip: lseek to offset " +
                                 std::to_string(target) + " on " +
                                 handle->path() + " (size " +
                                 std::to_string(ctx->block_device_size) +
                                 ") failed: " + strerror(saved));
            errno = saved;
            return -saved;
        }
        return 0;
    }
    return FlashRawDataChunk(handle, reinterpret_cast<const char*>(data), len, ctx->err);
}

int FlashSparseData(PartitionHandle* handle, std::vector<char>& downloaded_data,
                    uint64_t block_device_size, std::string* err) {
    struct sparse_file* file = sparse_file_import_buf(downloaded_data.data(),
                                                      downloaded_data.size(), true, false);
    if (!file) {
        LOG(ERROR) << "Unable to open sparse data for flashing";
        SetErr(err, "not a valid sparse image (sparse_file_import_buf failed on " +
                        std::to_string(downloaded_data.size()) + "B payload)");
        return -EINVAL;
    }

    int64_t expanded = sparse_file_len(file, false, false);
    if (expanded > 0 && static_cast<uint64_t>(expanded) > block_device_size) {
        SetErr(err, "sparse image expands to " + std::to_string(expanded) +
                        "B but partition " + handle->path() + " is only " +
                        std::to_string(block_device_size) + "B");
        return -EFBIG;
    }

    SparseCallbackCtx ctx{handle, block_device_size, err};
    return sparse_file_callback(file, false, false, WriteCallback, &ctx);
}

int FlashBlockDevice(PartitionHandle* handle, std::vector<char>& downloaded_data,
                     uint64_t block_device_size, std::string* err) {
    lseek64(handle->fd(), 0, SEEK_SET);
    if (downloaded_data.size() >= sizeof(SPARSE_HEADER_MAGIC) &&
        *reinterpret_cast<uint32_t*>(downloaded_data.data()) == SPARSE_HEADER_MAGIC) {
        return FlashSparseData(handle, downloaded_data, block_device_size, err);
    } else {
        return FlashRawData(handle, downloaded_data, err);
    }
}

// static void CopyAVBFooter(std::vector<char>* data, const uint64_t block_device_size) {
//     if (data->size() < AVB_FOOTER_SIZE) {
//         return;
//     }
//     std::string footer;
//     uint64_t footer_offset = data->size() - AVB_FOOTER_SIZE;
//     for (int idx = 0; idx < AVB_FOOTER_MAGIC_LEN; idx++) {
//         footer.push_back(data->at(footer_offset + idx));
//     }
//     if (0 != footer.compare(AVB_FOOTER_MAGIC)) {
//         return;
//     }

//     // copy AVB footer from end of data to end of block device
//     uint64_t original_data_size = data->size();
//     data->resize(block_device_size, 0);
//     for (int idx = 0; idx < AVB_FOOTER_SIZE; idx++) {
//         data->at(block_device_size - 1 - idx) = data->at(original_data_size - 1 - idx);
//     }
// }

int Flash(FastbootDevice* device, const std::string& partition_name, std::string* err) {
    PartitionHandle handle;
    auto partition_path = partition_name;
    partition_path.insert(0, "/dev/");
    // Belt-and-braces with the kernel's O_EXCL: serialise concurrent flashes
    // to the same partition across USB and TCP workers (`-i usb+tcp` mode).
    auto part_guard = rpi::PartitionLockManager::Instance().Acquire(partition_path);
    if (!OpenPartition(device, partition_path, &handle, O_WRONLY | O_DIRECT)) {
        LOG(ERROR) << "Cannot flash partition " << partition_path << " as failed to access";
        SetErr(err, "cannot access partition " + partition_path +
                        " (OpenPartition failed; partition may not exist)");
        return -ENOENT;
    }
    if (!handle.Open(O_WRONLY | O_DIRECT)){
        int saved = errno;
        LOG(ERROR) << "Cannot open partition " << partition_path;
        SetErr(err, "open(" + partition_path + ", O_WRONLY|O_DIRECT) failed: " +
                        strerror(saved ? saved : ENOENT));
        return -ENOENT;
    }

    std::vector<char> data = std::move(device->download_data());
    if (data.size() == 0) {
        LOG(ERROR) << "Cannot flash empty data vector";
        SetErr(err, "download buffer is empty; stage data before flashing " + partition_name);
        return -EINVAL;
    }
    uint64_t block_device_size = android::get_block_device_size(handle.fd());
    if (data.size() > block_device_size) {
        LOG(ERROR) << "Cannot flash " << data.size() << " bytes to block device of size "
                   << block_device_size;
        SetErr(err, "payload " + std::to_string(data.size()) + "B exceeds partition " +
                        partition_path + " size " + std::to_string(block_device_size) + "B");
        return -EOVERFLOW;
    }
    int result = FlashBlockDevice(&handle, data, block_device_size, err);
    sync();
    return result;
}

// static void RemoveScratchPartition() {
//     AutoMountMetadata mount_metadata;
//     android::fs_mgr::TeardownAllOverlayForMountPoint();
// }

// bool UpdateSuper(FastbootDevice* device, const std::string& super_name, bool wipe) {
//     std::vector<char> data = std::move(device->download_data());
//     if (data.empty()) {
//         return device->WriteFail("No data available");
//     }

//     std::unique_ptr<LpMetadata> new_metadata = ReadFromImageBlob(data.data(), data.size());
//     if (!new_metadata) {
//         return device->WriteFail("Data is not a valid logical partition metadata image");
//     }

//     if (!FindPhysicalPartition(super_name)) {
//         return device->WriteFail("Cannot find " + super_name +
//                                  ", build may be missing broken or missing boot_devices");
//     }

//     std::string slot_suffix = device->GetCurrentSlot();
//     uint32_t slot_number = SlotNumberForSlotSuffix(slot_suffix);

//     std::string other_slot_suffix;
//     if (!slot_suffix.empty()) {
//         other_slot_suffix = (slot_suffix == "_a") ? "_b" : "_a";
//     }

//     // If we are unable to read the existing metadata, then the super partition
//     // is corrupt. In this case we reflash the whole thing using the provided
//     // image.
//     std::unique_ptr<LpMetadata> old_metadata = ReadMetadata(super_name, slot_number);
//     if (wipe || !old_metadata) {
//         if (!FlashPartitionTable(super_name, *new_metadata.get())) {
//             return device->WriteFail("Unable to flash new partition table");
//         }
//         RemoveScratchPartition();
//         sync();
//         return device->WriteOkay("Successfully flashed partition table");
//     }

//     std::set<std::string> partitions_to_keep;
//     bool virtual_ab = android::base::GetBoolProperty("ro.virtual_ab.enabled", false);
//     for (const auto& partition : old_metadata->partitions) {
//         // Preserve partitions in the other slot, but not the current slot.
//         std::string partition_name = GetPartitionName(partition);
//         if (!slot_suffix.empty()) {
//             auto part_suffix = GetPartitionSlotSuffix(partition_name);
//             if (part_suffix == slot_suffix || (part_suffix == other_slot_suffix && virtual_ab)) {
//                 continue;
//             }
//         }
//         std::string group_name = GetPartitionGroupName(old_metadata->groups[partition.group_index]);
//         // Skip partitions in the COW group
//         if (group_name == android::snapshot::kCowGroupName) {
//             continue;
//         }
//         partitions_to_keep.emplace(partition_name);
//     }

//     // Do not preserve the scratch partition.
//     partitions_to_keep.erase("scratch");

//     if (!partitions_to_keep.empty()) {
//         std::unique_ptr<MetadataBuilder> builder = MetadataBuilder::New(*new_metadata.get());
//         if (!builder->ImportPartitions(*old_metadata.get(), partitions_to_keep)) {
//             return device->WriteFail(
//                     "Old partitions are not compatible with the new super layout; wipe needed");
//         }

//         new_metadata = builder->Export();
//         if (!new_metadata) {
//             return device->WriteFail("Unable to build new partition table; wipe needed");
//         }
//     }

//     // Write the new table to every metadata slot.
//     if (!UpdateAllPartitionMetadata(device, super_name, *new_metadata.get())) {
//         return device->WriteFail("Unable to write new partition table");
//     }
//     RemoveScratchPartition();
//     sync();
//     return device->WriteOkay("Successfully updated partition table");
// }
