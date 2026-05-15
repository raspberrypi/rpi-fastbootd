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
#include "tcp_client.h"
#include "utility.h"
#include "variables.h"

static constexpr int kMaxTcpWorkersHardCap = 8;
static constexpr const char* kSdramSizeDtPath =
    "/proc/device-tree/chosen/rpi-sdram-size-gbit";

// Cap on simultaneous TCP data-plane workers in -i usb+tcp mode. Each worker
// can hold a kMaxDownloadSizeDefault staging buffer, so the budget is
// (RAM - 256 MiB reserve) / kMaxDownloadSizeDefault, clamped to
// [1, kMaxTcpWorkersHardCap].
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

    sdram_gbit = __builtin_bswap32(sdram_gbit);

    uint64_t ram_bytes = static_cast<uint64_t>(sdram_gbit) * 1000 * 1000 * 1000 / 8;
    constexpr uint64_t kSystemReserve = 256ULL << 20;
    uint64_t budget = (ram_bytes > kSystemReserve) ? (ram_bytes - kSystemReserve) : 0;
    int workers = static_cast<int>(budget / kMaxDownloadSizeDefault);
    workers = std::clamp(workers, 1, kMaxTcpWorkersHardCap);

    LOG(INFO) << "SDRAM " << sdram_gbit << " Gbit (" << (ram_bytes >> 20)
              << " MiB), max TCP workers: " << workers;
    return workers;
}

// Bounded thread pool. Enqueue blocks when the queue is full so the accept
// loop doesn't outrun the workers and defeat the per-worker memory budget.
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

static void LogSparseVerboseMessage(const char* fmt, ...) {
    std::string message;

    va_list ap;
    va_start(ap, fmt);
    android::base::StringAppendV(&message, fmt, ap);
    va_end(ap);

    LOG(ERROR) << "libsparse message: " << message;
}

// Background thread: TCP accept loop. Each accepted connection is handed
// to a worker that runs ExecuteCommands() on a FastbootDevice. When
// data_plane_only is true the worker rejects every command except
// download/flash and the version-handshake getvar.
static void RunTcpAcceptLoop(bool data_plane_only) {
    auto server = ClientTcpTransport::CreateServer();
    if (!server) {
        LOG(ERROR) << "Failed to create TCP server socket; TCP path disabled";
        return;
    }

    const int max_workers = GetMaxTcpWorkers();
    ThreadPool pool(max_workers);
    LOG(INFO) << "TCP listener ready"
              << (data_plane_only ? " (data-plane-only)" : "")
              << ", pool size: " << max_workers;

    while (true) {
        auto socket = ClientTcpTransport::AcceptHandshake(server.get());
        if (!socket) continue;

        pool.Enqueue([s = std::move(socket), data_plane_only]() mutable {
            auto transport = std::make_unique<ClientTcpTransport>(std::move(s));
            FastbootDevice device(std::move(transport), data_plane_only);
            device.ExecuteCommands();
            LOG(INFO) << "TCP worker finished";
        });
    }
}

// LOCK the OTP private-key slot at startup, idempotently, on first call.
// Called from RunUsbLoop / the TCP-only branch *after* the gadget's ep0
// descriptors have been written (or, for TCP-only, after the listener is
// up), so a failure here cannot stop the device from enumerating to the
// host. The outcome is recorded in g_otp_lock_status (queryable via
// `getvar otp-lock-status`); GetPrivkey consults IsOtpLockStatusSafe()
// and refuses on failure, preserving the firmware-level export-protection
// guarantee of the original FATAL.
static void LockOtpKeyAtStartup() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        rpi::RpiFwCrypto crypto;
        auto status = rpi::RpiFwCrypto::GetCachedProvisioningStatus();
        if (!status) {
            int rc = static_cast<int>(status.error());
            LOG(ERROR) << "Startup: OTP key status read failed (rc=" << rc
                       << "); privkey-export operations will be refused";
            SetOtpLockStatus("read-failed:" + std::to_string(rc));
            return;
        }
        if (!*status) {
            LOG(INFO) << "Startup: OTP key is not provisioned — nothing to lock yet";
            SetOtpLockStatus("not-provisioned");
            return;
        }
        if (crypto.IsKeyLocked()) {
            LOG(INFO) << "Startup: OTP key is already LOCKED";
            SetOtpLockStatus("ok");
            return;
        }
        LOG(INFO) << "Startup: OTP key is provisioned but unlocked — LOCKing now";
        int rc = crypto.LockKey();
        if (rc != 0) {
            LOG(ERROR) << "Startup LOCK of provisioned OTP key failed (rc=" << rc
                       << "); privkey-export operations will be refused until reboot";
            SetOtpLockStatus("lock-failed:" + std::to_string(rc));
            return;
        }
        SetOtpLockStatus("ok");
    });
}

// Single-threaded USB loop: serially construct a FastbootDevice on the
// USB transport and run ExecuteCommands() until it returns. Runs the
// startup OTP-lock pass on the first iteration, after the FastbootDevice
// constructor has written FunctionFS descriptors and notified READY=1, so
// any lock failure remains debuggable from the host (the gadget enumerates
// regardless and `getvar otp-lock-status` reports the outcome).
[[noreturn]] static void RunUsbLoop() {
    bool first = true;
    while (true) {
        FastbootDevice device("usb");
        if (first) {
            LockOtpKeyAtStartup();
            first = false;
        }
        device.ExecuteCommands();
    }
}

int main(int argc, char* argv[]) {
    android::base::InitLogging(argv, &android::base::KernelLogger);

    const char* mode = "usb";
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "-i") {
            mode = argv[i + 1];
            break;
        }
    }
    sparse_print_verbose = LogSparseVerboseMessage;

    // The OTP private-key LOCK pass used to live here, but a failing
    // LOG(FATAL) before the USB transport opened ep0 made any lock-time
    // problem indistinguishable on the host from "broken USB" — the gadget
    // would attach but the kernel would time out reading descriptors. The
    // pass now runs from RunUsbLoop / the TCP-only branch *after* the
    // transport is up, and records its outcome in getvar otp-lock-status.
    // GetPrivkey gates on that status so the firmware-level export
    // guarantee of the original FATAL is preserved.

    const std::string modestr = mode;
    const bool want_usb = modestr.find("usb") != std::string::npos;
    const bool want_tcp = modestr.find("tcp") != std::string::npos;

    if (!want_usb && !want_tcp) {
        LOG(FATAL) << "Unknown -i mode '" << mode << "'; expected usb, tcp, or usb+tcp";
        return 1;
    }

    // Split mode: USB carries the full control+data plane; TCP workers are
    // restricted to download/flash plus a version-handshake getvar.
    const bool split_mode = want_usb && want_tcp;
    SetTcpDataPlaneOnlyMode(split_mode);

    if (split_mode) {
        std::thread tcp_thread(RunTcpAcceptLoop, /*data_plane_only=*/true);
        tcp_thread.detach();
        RunUsbLoop();  // never returns
    } else if (want_usb) {
        RunUsbLoop();  // never returns
    } else {
        // TCP only: full command map, sequential connections.
        auto server = ClientTcpTransport::CreateServer();
        if (!server) {
            LOG(FATAL) << "Failed to create TCP server socket";
            return 1;
        }
        // No USB descriptors to worry about here, but we still want the
        // lock pass to run before serving traffic so getvar otp-lock-status
        // is meaningful and GetPrivkey is gated correctly.
        LockOtpKeyAtStartup();
        LOG(INFO) << "TCP-only mode: accepting one connection at a time";
        while (true) {
            auto socket = ClientTcpTransport::AcceptHandshake(server.get());
            if (!socket) continue;
            auto transport = std::make_unique<ClientTcpTransport>(std::move(socket));
            FastbootDevice device(std::move(transport), /*data_plane_only=*/false);
            device.ExecuteCommands();
            LOG(INFO) << "TCP connection finished";
        }
    }
}
