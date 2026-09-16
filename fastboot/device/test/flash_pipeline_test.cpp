// FlashPipeline bookkeeping, exercised with an injected write so no block
// device is involved: result hand-off, exception containment, buffer
// recycling, the process-wide drain, and the memory check.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../flash_pipeline.h"

namespace {

std::vector<char> Payload(size_t n) {
    return std::vector<char>(n, 'x');
}

size_t MemAvailableBytes() {
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            return std::stoull(line.substr(13)) * 1024;
        }
    }
    return 0;
}

int Succeed(std::vector<char>&, std::string*) {
    return 0;
}

}  // namespace

TEST_CASE("Join with nothing in flight is a no-op", "[pipeline]") {
    FlashPipeline p;
    std::string err = "untouched";
    REQUIRE(p.Join(&err) == 0);
    REQUIRE(err == "untouched");
}

TEST_CASE("A deferred failure is reported on the next Submit, once", "[pipeline]") {
    FlashPipeline p;
    std::vector<char> data = Payload(64);
    std::string err;

    REQUIRE(p.Submit("mmcblk0p2", &data,
                     [](std::vector<char>&, std::string* e) {
                         *e = "disk said no";
                         return -EIO;
                     },
                     &err) == 0);

    bool second_ran = false;
    data = Payload(64);
    int rc = p.Submit("mmcblk0p2", &data,
                      [&](std::vector<char>&, std::string*) {
                          second_ran = true;
                          return 0;
                      },
                      &err);
    REQUIRE(rc == -EIO);
    REQUIRE(err.find("mmcblk0p2 (write #1)") != std::string::npos);
    REQUIRE(err.find("disk said no") != std::string::npos);
    REQUIRE_FALSE(second_ran);  // refused to queue behind a failure

    err.clear();
    REQUIRE(p.Join(&err) == 0);  // reported once, then cleared
    REQUIRE(err.empty());
}

TEST_CASE("A throwing write becomes -EIO rather than escaping", "[pipeline]") {
    FlashPipeline p;
    std::vector<char> data = Payload(8);
    std::string err;
    REQUIRE(p.Submit("mmcblk0p3", &data,
                     [](std::vector<char>&, std::string*) -> int {
                         throw std::runtime_error("boom");
                     },
                     &err) == 0);
    REQUIRE(p.Join(&err) == -EIO);
    REQUIRE(err.find("write threw: boom") != std::string::npos);
}

TEST_CASE("The retired buffer comes back with its capacity", "[pipeline]") {
    FlashPipeline p;
    constexpr size_t kBig = 4 * 1024 * 1024;
    std::vector<char> data = Payload(kBig);
    std::string err;

    REQUIRE(p.Submit("a", &data, Succeed, &err) == 0);
    REQUIRE(data.empty());  // swapped out to the worker
    REQUIRE(p.Join(&err) == 0);

    data = Payload(16);
    REQUIRE(p.Submit("b", &data, Succeed, &err) == 0);
    REQUIRE(data.empty());
    REQUIRE(data.capacity() >= kBig);  // the first payload's allocation, recycled
    REQUIRE(p.Join(&err) == 0);
}

TEST_CASE("WaitAllIdle blocks until the write has landed", "[pipeline]") {
    FlashPipeline p;
    std::vector<char> data = Payload(8);
    std::string err;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> landed{false};

    REQUIRE(p.Submit("slow", &data,
                     [&](std::vector<char>&, std::string*) {
                         released.wait();
                         landed = true;
                         return 0;
                     },
                     &err) == 0);

    std::atomic<bool> waiter_done{false};
    std::thread waiter([&] {
        FlashPipeline::WaitAllIdle();
        waiter_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE_FALSE(waiter_done);  // still writing

    release.set_value();
    waiter.join();
    REQUIRE(landed);  // it returned only after the write finished
    REQUIRE(p.Join(&err) == 0);
}

TEST_CASE("Enable refuses what MemAvailable cannot cover", "[pipeline][memory]") {
    FlashPipeline p;
    std::string err;
    REQUIRE_FALSE(p.Enable(size_t{1} << 60, 0, &err));
    REQUIRE(err.find("not enough free memory") != std::string::npos);
    REQUIRE_FALSE(p.enabled());
}

TEST_CASE("Enable counts both buffers, existing capacity and other sessions' pledges",
          "[pipeline][memory]") {
    const size_t avail = MemAvailableBytes();
    const size_t reserve = FlashPipeline::kMemoryReserveBytes;
    if (avail < 4 * reserve) {
        SKIP("too little memory on this host for the arithmetic to be unambiguous");
    }
    // With M = avail - reserve and S = 10M/22: one session with an empty
    // download buffer needs 2S + reserve, which fits; a second beside the
    // first's pledge needs 3S + reserve, which does not.
    const size_t staging = (avail - reserve) / 22 * 10;
    std::string err;

    FlashPipeline a;
    REQUIRE(a.Enable(staging, 0, &err));

    FlashPipeline b;
    REQUIRE_FALSE(b.Enable(staging, 0, &err));
    REQUIRE(err.find("pledged by other sessions") != std::string::npos);

    // A buffer that already exists is not asked for again: with one staging
    // buffer allocated, b needs only one more, and that fits beside a's pledge.
    REQUIRE(b.Enable(staging, staging, &err));

    // Pledges are released with the session.
    FlashPipeline c;
    REQUIRE_FALSE(c.Enable(staging, 0, &err));
    REQUIRE(a.Disable(&err) == 0);
    REQUIRE(b.Disable(&err) == 0);
    REQUIRE(c.Enable(staging, 0, &err));
}
