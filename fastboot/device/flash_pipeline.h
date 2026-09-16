/*
 * Deferred ("lazy-blocking") flash writes for fastbootd.
 *
 * Copyright 2026 Raspberry Pi Ltd. Licensed under the Apache License 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

// Fastboot is lockstep: download: N+1 waits for flash: N to answer, so the
// link idles during every write. Once a session opts in, flash: answers as soon
// as the chunk is queued and a worker writes it; only the OKAY moves earlier.
//
// An OKAY cannot be retracted, so one write is in flight per session and
// Submit() returns the previous write's result. The dispatcher calls Join()
// before any command other than download:/flash:, so the last write's failure
// lands on whatever follows. A client that flashes and then disconnects still
// gets its data written, but is never told if that failed.
class FlashPipeline {
  public:
    // The deferred half of a flash: everything from the first byte written
    // onwards. Takes the payload it was submitted with and an error string to
    // populate; returns 0 or a negative errno.
    using WriteFn = std::function<int(std::vector<char>&, std::string*)>;

    // Headroom Enable() insists on beyond the staging buffers. fastbootd runs
    // from a ramdisk with the rest of the system in the same memory, so taking
    // the last free page to win a few seconds on a flash is a bad trade.
    static constexpr size_t kMemoryReserveBytes = 128 * 1024 * 1024;

    FlashPipeline() = default;
    ~FlashPipeline();

    FlashPipeline(const FlashPipeline&) = delete;
    FlashPipeline& operator=(const FlashPipeline&) = delete;

    bool enabled() const { return enabled_; }

    // Turn deferral on for this session. Deferring holds two staging buffers
    // of `staging_bytes` where the plain path holds one; `already_allocated`
    // is the download buffer's current capacity, which counts towards that.
    // Fails when MemAvailable cannot cover the shortfall plus the reserve,
    // after subtracting what other enabled sessions have pledged but may not
    // have allocated yet.
    bool Enable(size_t staging_bytes, size_t already_allocated, std::string* err);

    // Drain, then turn deferral off. Returns the drained write's result.
    int Disable(std::string* err);

    // Hand `work` to the worker and return the PREVIOUS write's result.
    // `*data` is swapped out to the worker and left holding the previously
    // retired buffer, cleared but with its capacity intact -- so the next
    // download refills an allocation that is already faulted in.
    // `desc` names this write in any deferred error ("mmcblk0p4 chunk 7").
    int Submit(const std::string& desc, std::vector<char>* data, WriteFn work, std::string* err);

    // Block until nothing is in flight; return the outstanding write's result.
    // The result is reported once and then cleared.
    int Join(std::string* err);

    // Block until no session in the process has a write in progress. Says
    // nothing about results -- those belong to the session that submitted
    // them -- only that the bytes have been written and synced.
    static void WaitAllIdle();

  private:
    struct Outcome {
        int rc = 0;
        std::string err;
        std::vector<char> buffer;
    };

    void Unpledge();

    bool enabled_ = false;
    size_t staging_bytes_ = 0;
    std::future<Outcome> in_flight_;
    std::string in_flight_desc_;
    uint64_t submit_seq_ = 0;
    // Retired buffer, handed back to the caller on the next Submit().
    std::vector<char> spare_;
};
