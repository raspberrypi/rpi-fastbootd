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

// Ask the kernel to adopt the partition table now on disk.
//
// This is the one implementation of that operation. Writing a table, or writing
// an image that carries one, does not change the kernel's in-memory view of it:
// /dev/<dev>pN keeps the offsets and sizes of the previous layout, and anything
// acting on a partition number afterwards -- mkfs, cryptsetup, a mount, a size
// query -- silently operates on the old geometry.
//
// BLKRRPART fails with EBUSY whenever any partition of the disk is open, and
// the kernel's own rescan is what creates those partitions -- so a re-read that
// has just succeeded leaves udev probing the nodes it produced, and a re-read
// issued moments later hits EBUSY through no fault of the caller. Retrying for
// a bounded period rides that out. A device still busy at the end of it is
// genuinely held by something (a leftover mount, a stale device-mapper node),
// and no caller may treat that as the kernel having adopted the new layout.
//
// Whether the caller's fd was opened O_EXCL makes no difference: verified on
// 6.18 that an exclusive whole-disk fd re-reads fine, and that a single open
// partition blocks it either way.
//
// Pass timeout_sec = 0 for a single attempt, which is also the way to use this
// as a probe for whether a disk is in use at all.
//
// Returns 0 on success, otherwise the errno of the last attempt.
namespace rpiparted {

int rereadPartitionTable(int fd, int timeout_sec = 5);
int rereadPartitionTable(const std::string& dev, int timeout_sec = 5);

} // namespace rpiparted

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
     * Writing the table does not make the kernel adopt it. Unless the caller
     * has its own re-read step, prefer commitAndReread().
     *
     * @return True on success. False on failure.
     */
    bool commit();

    /**
     * @brief Write partition table changes to disk and have the kernel adopt them
     *
     * commit() alone leaves the kernel serving the partition table it read
     * earlier, so /dev/<dev>pN keeps the offsets and sizes of the previous
     * layout. Anything acting on a partition number afterwards - mkfs,
     * cryptsetup, a size query - then silently operates on the old geometry.
     *
     * Closes the device, so the fd held while the table was rewritten cannot
     * block the re-read, then retries BLKRRPART while it reports EBUSY. A
     * partition still held open elsewhere - a stale device-mapper node, a
     * mount - keeps the old table in place, and is reported as a failure
     * rather than left for the caller to trip over.
     *
     * The device is left closed. Re-open it to continue partitioning.
     *
     * @param timeout_sec How long to keep retrying while the device is busy
     *
     * @return True on success. False on failure.
     */
    bool commitAndReread(int timeout_sec = 5);

private:
    std::unique_ptr<struct fdisk_context, FdiskContextDeleter> context_;
    bool is_gpt_;
    size_t sector_size_;
    unsigned long grain_;
    bool device_assigned_ = false;
};

#endif // RPIPARTED_H
