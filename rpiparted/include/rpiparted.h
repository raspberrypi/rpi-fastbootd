#ifndef RPIPARTED_H
#define RPIPARTED_H

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <libfdisk/libfdisk.h>

struct FdiskContextDeleter {
    void operator()(struct fdisk_context* ctx) const;
};

class RPIparted {
public:
    RPIparted();
    ~RPIparted() = default;

    /**
     * @brief Open a device for partitioning operations
     *
     * @param device Device path to open
     * @param align_kb [optional] Align W ops on the partition table to this
     *
     * @return True on success. False on failure.
     */
    bool openDevice(const std::string& device, unsigned long align_kb);

    /**
     * @brief Write an empty partition table to the device
     *
     * @param type Partition table type: gpt,dos|mbr
     * @param id [optional] Disk label ID
     *
     * @return True on success. False on failure.
     */
    bool createPartitionTable(const std::string& type, const std::optional<std::string>& id);

    /**
     * @brief Add a new partition table entry with defaults for number,start
     *
     * @param size_bytes Size of the partition required. If zero, all remaining
     *                   space on the device will be consumed
     * @param type DOS/GPT identifier of the partition to create
     *
     * @return True on success. False on failure.
     */
    bool appendPartition(const uint64_t size_bytes, const std::string& type);

    /**
     * @brief Remove a partition from the table
     *
     * @param partnum The partition indice to remove (1 based)
     *
     * @return True on success. False on failure.
     */
    bool removePartition(const size_t partnum);

private:
    std::unique_ptr<struct fdisk_context, FdiskContextDeleter> context_;
    bool is_gpt_;
    size_t sector_size_;
    unsigned long grain_;
    bool commit();
};

#endif // RPIPARTED_H
