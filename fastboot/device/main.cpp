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

#include <stdarg.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <sparse/sparse.h>

#include "commands.h"
#include "fastboot_device.h"
#include "shared_idp.h"
#include "tcp_client.h"

static constexpr int kMaxTcpWorkersHardCap = 8;
static constexpr const char* kSdramSizeDtPath =
    "/proc/device-tree/chosen/rpi-sdram-size-gbit";

static int GetMaxTcpWorkers() {
    int fd = open(kSdramSizeDtPath, O_RDONLY);
    if (fd < 0) {
        LOG(WARNING) << "Cannot read " << kSdramSizeDtPath
                     << ", defaulting to 2 TCP workers";
        return 2;
    }

    uint32_t sdram_gbit = 0;
    bool ok = (read(fd, &sdram_gbit, sizeof(sdram_gbit)) == sizeof(sdram_gbit));
    close(fd);

    if (!ok || sdram_gbit == 0) {
        LOG(WARNING) << "Invalid SDRAM size from DT, defaulting to 2 TCP workers";
        return 2;
    }

    // DT value is big-endian
    sdram_gbit = __builtin_bswap32(sdram_gbit);

    // Convert Gbit to bytes, reserve 256 MiB for the utility OS, budget the
    // rest for download buffers (each worker can use up to kMaxDownloadSizeDefault).
    uint64_t ram_bytes = static_cast<uint64_t>(sdram_gbit) * 1000 * 1000 * 1000 / 8;
    constexpr uint64_t kSystemReserve = 256ULL << 20;  // 256 MiB
    uint64_t budget = (ram_bytes > kSystemReserve) ? (ram_bytes - kSystemReserve) : 0;
    int workers = static_cast<int>(budget / kMaxDownloadSizeDefault);
    workers = std::clamp(workers, 1, kMaxTcpWorkersHardCap);

    LOG(INFO) << "SDRAM " << sdram_gbit << " Gbit (" << (ram_bytes >> 20)
              << " MiB), max TCP workers: " << workers;
    return workers;
}

// Thread pool with bounded task queue: N worker threads pull tasks from a
// shared queue.  Enqueue blocks if the queue is full, providing backpressure
// to the accept loop so memory budgeting is not defeated.
class ThreadPool {
  public:
    explicit ThreadPool(int num_threads)
        : max_queued_(num_threads) {
        for (int i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_workers_.notify_all();
        cv_enqueue_.notify_all();
        for (auto& t : workers_) {
            t.join();
        }
    }

    // Blocks if the queue already has max_queued_ pending tasks.
    void Enqueue(std::move_only_function<void()> task) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_enqueue_.wait(lock, [this] {
            return stop_ || static_cast<int>(tasks_.size()) < max_queued_;
        });
        if (stop_) return;
        tasks_.push(std::move(task));
        cv_workers_.notify_one();
    }

  private:
    void WorkerLoop() {
        while (true) {
            std::move_only_function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_workers_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            cv_enqueue_.notify_one();
            task();
        }
    }

    const int max_queued_;
    std::vector<std::thread> workers_;
    std::queue<std::move_only_function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_workers_;
    std::condition_variable cv_enqueue_;
    bool stop_ = false;
};

auto mode = "usb";

static void LogSparseVerboseMessage(const char* fmt, ...) {
    std::string message;

    va_list ap;
    va_start(ap, fmt);
    android::base::StringAppendV(&message, fmt, ap);
    va_end(ap);

    LOG(ERROR) << "libsparse message: " << message;
}

int main(int argc, char* argv[]) {
    android::base::InitLogging(argv, &android::base::KernelLogger);

    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "-i") {
            mode = argv[i + 1];
            break;
        }
    }
    sparse_print_verbose = LogSparseVerboseMessage;

    std::string modestr = mode;
    if (modestr.find("tcp") != std::string::npos) {
        // Multi-connection TCP mode: accept loop with thread pool.
        auto server = ClientTcpTransport::CreateServer();
        if (!server) {
            LOG(FATAL) << "Failed to create TCP server socket";
            return 1;
        }

        const int max_workers = GetMaxTcpWorkers();
        ThreadPool pool(max_workers);

        // Shared IDP context: all TCP workers share one IDPdevice so
        // multiple connections can claim and flash partitions in parallel.
        auto shared_idp = std::make_shared<SharedIDPContext>();

        LOG(INFO) << "TCP server ready, pool size: " << max_workers;

        while (true) {
            auto socket = ClientTcpTransport::AcceptHandshake(server.get());
            if (!socket) continue;

            pool.Enqueue([s = std::move(socket), shared_idp]() mutable {
                auto transport = std::make_unique<ClientTcpTransport>(std::move(s));
                FastbootDevice device(std::move(transport), shared_idp);
                device.ExecuteCommands();
                LOG(INFO) << "TCP worker finished";
            });
        }
    } else {
        // USB mode: single connection at a time.
        while (true) {
            FastbootDevice device(mode);
            device.ExecuteCommands();
        }
    }
}
