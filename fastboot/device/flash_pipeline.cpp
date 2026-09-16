/*
 * Deferred ("lazy-blocking") flash writes for fastbootd.
 *
 * Copyright 2026 Raspberry Pi Ltd. Licensed under the Apache License 2.0.
 */

#include "flash_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>

namespace {

// The process-wide view of every session's pipeline, for the two questions
// that cross sessions: how much memory has been promised, and whether any
// write is still going to disk. Sessions share no other pipeline state.
std::atomic<size_t> g_pledged_bytes{0};
std::mutex g_inflight_mutex;
std::condition_variable g_inflight_cv;
int g_inflight_writes = 0;

void CountWriteDown() {
    {
        std::lock_guard<std::mutex> lock(g_inflight_mutex);
        --g_inflight_writes;
    }
    g_inflight_cv.notify_all();
}

// MemAvailable is the kernel's own estimate of what can be handed out without
// swapping, which is the question being asked here -- MemFree is not, since it
// ignores reclaimable page cache.
bool ReadMemAvailableBytes(size_t* out) {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) {
        return false;
    }
    std::string line;
    while (std::getline(meminfo, line)) {
        if (!android::base::StartsWith(line, "MemAvailable:")) {
            continue;
        }
        // "MemAvailable:   1234567 kB"
        auto fields = android::base::Split(android::base::Trim(line.substr(13)), " ");
        uint64_t kb = 0;
        if (fields.empty() || !android::base::ParseUint(fields[0], &kb)) {
            return false;
        }
        *out = static_cast<size_t>(kb) * 1024;
        return true;
    }
    return false;
}

}  // namespace

FlashPipeline::~FlashPipeline() {
    // Drain rather than abandon. The bytes are already committed as far as the
    // host knows, and the worker holds the partition open; letting the future
    // destruct without collecting it would block here anyway, and dropping the
    // thread is not an option std::future offers.
    if (in_flight_.valid()) {
        LOG(INFO) << "Draining deferred flash write (" << in_flight_desc_ << ") at shutdown";
        std::string err;
        Join(&err);
    }
    Unpledge();
}

void FlashPipeline::Unpledge() {
    if (enabled_) {
        g_pledged_bytes -= staging_bytes_;
        enabled_ = false;
    }
}

bool FlashPipeline::Enable(size_t staging_bytes, size_t already_allocated, std::string* err) {
    if (enabled_) {
        return true;
    }
    if (staging_bytes > SIZE_MAX / 4) {
        if (err) *err = "staging size too large";
        return false;
    }

    size_t available = 0;
    if (!ReadMemAvailableBytes(&available)) {
        if (err) *err = "cannot read MemAvailable from /proc/meminfo";
        return false;
    }

    // Deferring holds two staging buffers where the plain path holds one: the
    // chunk being written and the chunk being downloaded. Whatever the
    // download buffer already holds has already left MemAvailable, so only the
    // shortfall is asked for. Other sessions that have enabled but not yet
    // downloaded have taken nothing from MemAvailable either; their pledge
    // stands in for it.
    const size_t shortfall = 2 * staging_bytes - std::min(already_allocated, staging_bytes);
    const size_t pledged = g_pledged_bytes.load();
    const size_t needed = shortfall + kMemoryReserveBytes + pledged;
    if (available < needed) {
        if (err) {
            *err = "not enough free memory to defer flash writes: need " +
                   std::to_string(needed / (1024 * 1024)) + " MiB (" +
                   std::to_string(pledged / (1024 * 1024)) +
                   " MiB pledged by other sessions), have " +
                   std::to_string(available / (1024 * 1024)) + " MiB";
        }
        return false;
    }

    g_pledged_bytes += staging_bytes;
    staging_bytes_ = staging_bytes;
    enabled_ = true;
    LOG(INFO) << "Deferred flash writes enabled (staging " << (staging_bytes / (1024 * 1024))
              << " MiB, " << (available / (1024 * 1024)) << " MiB available, "
              << (pledged / (1024 * 1024)) << " MiB pledged elsewhere)";
    return true;
}

int FlashPipeline::Disable(std::string* err) {
    int rc = Join(err);
    Unpledge();
    spare_ = std::vector<char>();
    LOG(INFO) << "Deferred flash writes disabled";
    return rc;
}

int FlashPipeline::Join(std::string* err) {
    if (!in_flight_.valid()) {
        return 0;
    }

    Outcome outcome;
    try {
        outcome = in_flight_.get();
    } catch (const std::exception& e) {
        // An exception thrown on the worker is re-thrown here by get(). Join()
        // is called from the command dispatcher and from this class's own
        // destructor, neither of which can let one through -- the dispatcher's
        // backstop sits further down, and a throwing destructor ends the
        // process. Turn it into the failure it is.
        outcome.rc = -EIO;
        outcome.err = std::string("write threw: ") + e.what();
    } catch (...) {
        outcome.rc = -EIO;
        outcome.err = "write threw a non-standard exception";
    }
    in_flight_ = {};
    const std::string desc = std::move(in_flight_desc_);
    in_flight_desc_.clear();

    // Keep the retired allocation for the next download to refill.
    spare_ = std::move(outcome.buffer);
    spare_.clear();

    if (outcome.rc != 0) {
        LOG(ERROR) << "Deferred flash write failed (" << desc << "): " << outcome.err;
        if (err) {
            // Say which write this was. The host is being told on a later
            // command than the one it belongs to, so "write failed" on its own
            // would point at the wrong thing.
            *err = "deferred write of " + desc + " failed: " + outcome.err;
        }
    }
    return outcome.rc;
}

int FlashPipeline::Submit(const std::string& desc, std::vector<char>* data, WriteFn work,
                          std::string* err) {
    // Collect the previous write before starting another. This is the blocking
    // half of "lazy-blocking": the host has already been told OKAY for that
    // write, and this is where it finds out otherwise.
    int prev = Join(err);
    if (prev != 0) {
        // Do not queue behind a failure. The image is already wrong and the
        // host is about to be told so.
        return prev;
    }

    std::vector<char> payload;
    payload.swap(*data);

    // Hand the caller the previously retired allocation, so the next download
    // resizes into pages that are already faulted in.
    *data = std::move(spare_);
    data->clear();
    spare_ = std::vector<char>();

    // Number the writes. A deferred failure is reported on a later command
    // than the one it belongs to, so "mmcblk0p4" alone would not say which of
    // the twenty-five writes to that partition went wrong.
    in_flight_desc_ = desc + " (write #" + std::to_string(++submit_seq_) + ")";

    {
        std::lock_guard<std::mutex> lock(g_inflight_mutex);
        ++g_inflight_writes;
    }
    try {
        in_flight_ = std::async(std::launch::async,
                                [work = std::move(work), buf = std::move(payload)]() mutable {
                                    // Counted down the moment the bytes are on
                                    // disk, whether or not anyone ever collects
                                    // the result: WaitAllIdle() asks about the
                                    // disk, not about who has been told.
                                    auto landed = android::base::make_scope_guard(CountWriteDown);
                                    Outcome out;
                                    out.rc = work(buf, &out.err);
                                    out.buffer = std::move(buf);
                                    return out;
                                });
    } catch (...) {
        // No thread was started, so nothing will count this write back down.
        CountWriteDown();
        throw;
    }
    return 0;
}

void FlashPipeline::WaitAllIdle() {
    std::unique_lock<std::mutex> lock(g_inflight_mutex);
    if (g_inflight_writes > 0) {
        LOG(INFO) << "Waiting for " << g_inflight_writes << " deferred flash write(s) to land";
    }
    g_inflight_cv.wait(lock, [] { return g_inflight_writes == 0; });
}
