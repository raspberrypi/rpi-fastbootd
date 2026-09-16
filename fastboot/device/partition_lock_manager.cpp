/*
 * Copyright 2026 Raspberry Pi Ltd. Licensed under the Apache License 2.0.
 */

#include "partition_lock_manager.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#include <android-base/logging.h>

namespace rpi {

namespace {

std::string Canonicalise(const std::string& name) {
    // If name starts with /, resolve it directly; otherwise treat as a
    // short name and prepend /dev/.
    std::string candidate = (name.size() > 0 && name[0] == '/')
                                ? name
                                : (std::string("/dev/") + name);
    char resolved[PATH_MAX];
    if (realpath(candidate.c_str(), resolved) != NULL) {
        return std::string(resolved);
    }
    // Fall back to the raw name — lock under whatever string we were given.
    // This is safe (over-serialises at worst) but diagnostics suffer.
    return candidate;
}

}  // namespace

PartitionLockManager& PartitionLockManager::Instance() {
    static PartitionLockManager instance;
    return instance;
}

PartitionLockGuard PartitionLockManager::Acquire(const std::string& partition_name) {
    std::string key = Canonicalise(partition_name);

    std::shared_ptr<std::mutex> mu;
    {
        std::lock_guard<std::mutex> g(map_mutex_);
        auto it = locks_.find(key);
        if (it == locks_.end()) {
            mu = std::make_shared<std::mutex>();
            locks_.emplace(key, mu);
        } else {
            mu = it->second;
        }
    }

    std::unique_lock<std::mutex> lock(*mu, std::try_to_lock);
    if (!lock.owns_lock()) {
        // Another session holds this partition -- a deferred write that has
        // not been joined yet, most likely. Say so, or the wait looks like a
        // hang from the host.
        LOG(INFO) << "Waiting for another session to release " << key;
        lock.lock();
    }
    return PartitionLockGuard(std::move(lock));
}

}  // namespace rpi
