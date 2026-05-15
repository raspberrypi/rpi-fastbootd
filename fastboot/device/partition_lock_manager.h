/*
 * fastbootd partition-level mutex. Serialises any operation that mutates
 * a partition so flash/erase/veritysetup/verityappend cannot interleave
 * on the same device across USB+TCP connections. [S19]
 *
 * Copyright 2026 Raspberry Pi Ltd. Licensed under the Apache License 2.0.
 */

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace rpi {

// Scoped guard — holding this blocks any other caller attempting to
// Acquire() the same canonical partition path until the guard is
// destroyed.
class PartitionLockGuard {
  public:
    PartitionLockGuard() = default;
    explicit PartitionLockGuard(std::unique_lock<std::mutex> lock)
        : lock_(std::move(lock)) {}
    PartitionLockGuard(PartitionLockGuard&&) = default;
    PartitionLockGuard& operator=(PartitionLockGuard&&) = default;

  private:
    std::unique_lock<std::mutex> lock_;
};

// Singleton. Canonicalises `partition_name` via realpath() when possible
// so a raw /dev/name and a by-slot symlink resolving to the same node
// share a single mutex.
class PartitionLockManager {
  public:
    static PartitionLockManager& Instance();

    // Block until no other caller holds the lock for the canonical path
    // of `partition_name`. Returns a guard whose destructor releases.
    PartitionLockGuard Acquire(const std::string& partition_name);

  private:
    PartitionLockManager() = default;
    std::mutex map_mutex_;
    std::map<std::string, std::shared_ptr<std::mutex>> locks_;

    // Non-copyable.
    PartitionLockManager(const PartitionLockManager&) = delete;
    PartitionLockManager& operator=(const PartitionLockManager&) = delete;
};

}  // namespace rpi
