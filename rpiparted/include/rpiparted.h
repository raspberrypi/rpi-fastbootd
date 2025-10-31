#ifndef RPIPARTED_H
#define RPIPARTED_H

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <libfdisk/libfdisk.h>

struct FdiskContextDeleter {
    FdiskContextDeleter(bool* assigned);
    void operator()(struct fdisk_context* ctx) const;
private:
    bool* assigned_;
};

struct PartitionAttributes {
    // Size of the new partition in bytes.
    // - 0: consume all remaining free space on the device.
    uint64_t size_bytes;

    // Partition type identifier:
    // - DOS/MBR: hex type code as string (e.g. "83", "0x83", "c", "0xc").
    // - GPT: type GUID string (e.g. "C12A7328-F81F-11D2-BA4B-00A0C93EC93B").
    std::string type_id;

    // Optional human-readable partition name (Linux::PARTLABEL)
    // - Applied only when operating on a GPT label (ignored on DOS/MBR).
    // - GPT stores up to 36 UTF‑16 code points.
    std::optional<std::string> partlabel;

    // Optional partition UUID (Linux::PARTUUID).
    // - Applied only when operating on a GPT label (ignored on DOS/MBR).
    // - Must be a valid UUID string (8-4-4-4-12 hex).
    std::optional<std::string> partuuid;
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
     * @brief Close a partitioning device
     *
     * @return None
     */
    void closeDevice();

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
     * @param attrs The new partition attributes
     *
     * @return True on success. False on failure.
     */
    bool appendPartition(const PartitionAttributes& attrs);

    /**
     * @brief Remove a partition from the table
     *
     * @param partnum The partition indice to remove (1 based)
     *
     * @return True on success. False on failure.
     */
    bool removePartition(const size_t partnum);

    /**
     * @brief Write all partition table changes in memory to disk
     *
     * @return True on success. False on failure.
     */
    bool commit();

    /**
     * @brief Instruct the kernel to re-read the partition table on the device
     *
     * In order for this to succeed, there must be no open fds on the device
     * from before the partition table was changed. Assuming rpiparted was
     * exclusively used to perform partition table operations, closing and
     * re-opening the device is essential to ensure the kernel is ready to
     * re-read the partition table. Closing the device flushes all buffers and
     * indicates that rpiparted is done with the old state.
     *
     * @return True on success. False on failure.
     */
    bool rereadPartitionTable();
private:
    std::unique_ptr<struct fdisk_context, FdiskContextDeleter> context_;
    bool is_gpt_;
    size_t sector_size_;
    unsigned long grain_;
    bool device_assigned_ = false;
};

#endif // RPIPARTED_H
