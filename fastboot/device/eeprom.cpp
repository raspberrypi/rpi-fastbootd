/*
 * Copyright (C) 2026 Raspberry Pi Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "eeprom.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <openssl/sha.h>

#ifdef HAVE_LIBFLASHROM
extern "C" {
#include <libflashrom.h>
}
#endif

namespace rpi::eeprom {

namespace {

#ifdef HAVE_LIBFLASHROM

int LogShim(enum flashrom_log_level level, const char* fmt, va_list ap) {
    if (level > FLASHROM_MSG_INFO) return 0;
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) LOG(INFO) << "flashrom: " << buf;
    return n;
}

bool PathExists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

// RAII session that probes the chip and reports its size. The session is the
// authoritative source for "how big is this EEPROM" — every public op probes
// fresh, so size changes between calls would be detected, not papered over.
struct FlashSession {
    flashrom_programmer* prog{nullptr};
    flashrom_flashctx* fl{nullptr};
    bool initted{false};
    std::size_t chip_size{0};

    Result Open(const std::string& spidev) {
        if (!PathExists(spidev)) {
            return Result::Fail("SPI device not present: " + spidev);
        }
        if (flashrom_init(0) != 0) {
            return Result::Fail("flashrom_init failed");
        }
        initted = true;
        flashrom_set_log_callback(LogShim);

        std::string params = "dev=" + spidev + ",spispeed=16000";
        if (flashrom_programmer_init(&prog, "linux_spi", params.c_str()) != 0) {
            return Result::Fail("linux_spi programmer init failed for " + spidev);
        }
        if (flashrom_flash_probe(&fl, prog, nullptr) != 0) {
            return Result::Fail("no SPI flash chip detected on " + spidev);
        }
        chip_size = flashrom_flash_getsize(fl);
        if (chip_size < kMinImageSize || chip_size > kMaxImageSize) {
            return Result::Fail("probed flash size " + std::to_string(chip_size) +
                                " is outside sanity bounds [" +
                                std::to_string(kMinImageSize) + ".." +
                                std::to_string(kMaxImageSize) + "]");
        }
        return Result::Ok();
    }

    ~FlashSession() {
        if (fl) flashrom_flash_release(fl);
        if (prog) flashrom_programmer_shutdown(prog);
        if (initted) flashrom_shutdown();
    }
};

#endif  // HAVE_LIBFLASHROM

}  // namespace

std::string DetectSpiDev() {
    // Pi 5 (BCM2712) uses /dev/spidev10.0; Pi 4 / CM4 (BCM2711) uses /dev/spidev0.0.
    for (const auto* candidate : {"/dev/spidev10.0", "/dev/spidev0.0"}) {
        struct stat st{};
        if (::stat(candidate, &st) == 0) {
            return candidate;
        }
    }
    return {};
}

Result QuerySize(const std::string& spidev, std::size_t* size_out) {
    if (!size_out) return Result::Fail("null size_out");
    *size_out = 0;
#ifndef HAVE_LIBFLASHROM
    (void)spidev;
    return Result::Fail("fastbootd built without libflashrom support");
#else
    FlashSession s;
    if (auto r = s.Open(spidev); !r.ok) return r;
    *size_out = s.chip_size;
    return Result::Ok();
#endif
}

Result Read(const std::string& spidev, std::vector<uint8_t>* out) {
    if (!out) return Result::Fail("null output buffer");
#ifndef HAVE_LIBFLASHROM
    (void)spidev;
    return Result::Fail("fastbootd built without libflashrom support");
#else
    FlashSession s;
    if (auto r = s.Open(spidev); !r.ok) return r;
    out->assign(s.chip_size, 0);
    if (flashrom_image_read(s.fl, out->data(), out->size()) != 0) {
        return Result::Fail("flashrom_image_read failed");
    }
    return Result::Ok();
#endif
}

Result Write(const std::string& spidev, const std::vector<uint8_t>& image) {
    if (image.size() < kMinImageSize || image.size() > kMaxImageSize) {
        return Result::Fail("supplied image size " + std::to_string(image.size()) +
                            " outside sanity bounds");
    }
#ifndef HAVE_LIBFLASHROM
    (void)spidev;
    return Result::Fail("fastbootd built without libflashrom support");
#else
    FlashSession s;
    if (auto r = s.Open(spidev); !r.ok) return r;
    if (image.size() != s.chip_size) {
        return Result::Fail("image size " + std::to_string(image.size()) +
                            " != probed flash size " + std::to_string(s.chip_size));
    }
    flashrom_flag_set(s.fl, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
    flashrom_flag_set(s.fl, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
    std::vector<uint8_t> work = image;  // image_write may scribble.
    if (flashrom_image_write(s.fl, work.data(), work.size(), nullptr) != 0) {
        return Result::Fail("flashrom_image_write failed");
    }
    return Result::Ok();
#endif
}

Result Verify(const std::string& spidev, const std::vector<uint8_t>& expected) {
    if (expected.size() < kMinImageSize || expected.size() > kMaxImageSize) {
        return Result::Fail("expected size " + std::to_string(expected.size()) +
                            " outside sanity bounds");
    }
#ifndef HAVE_LIBFLASHROM
    (void)spidev;
    return Result::Fail("fastbootd built without libflashrom support");
#else
    FlashSession s;
    if (auto r = s.Open(spidev); !r.ok) return r;
    if (expected.size() != s.chip_size) {
        return Result::Fail("expected size " + std::to_string(expected.size()) +
                            " != probed flash size " + std::to_string(s.chip_size));
    }
    if (flashrom_image_verify(s.fl, expected.data(), expected.size()) != 0) {
        return Result::Fail("EEPROM contents do not match supplied image");
    }
    return Result::Ok();
#endif
}

namespace {

// Run a single half-duplex SPI transaction: send `tx`, then read `rx_len`
// bytes back into `rx`. Speed is the bus clock to use.
bool SpiTxRx(int fd, const std::vector<uint8_t>& tx,
             std::vector<uint8_t>* rx, uint32_t speed_hz) {
    rx->assign(tx.size(), 0);
    spi_ioc_transfer xfer{};
    xfer.tx_buf = reinterpret_cast<__u64>(tx.data());
    xfer.rx_buf = reinterpret_cast<__u64>(rx->data());
    xfer.len = static_cast<__u32>(tx.size());
    xfer.speed_hz = speed_hz;
    xfer.bits_per_word = 8;
    return ::ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) >= 1;
}

bool AllSameByte(const std::vector<uint8_t>& v, uint8_t b) {
    for (auto x : v) if (x != b) return false;
    return !v.empty();
}

}  // namespace

Result QueryChipInfo(const std::string& spidev, ChipInfo* info) {
    if (!info) return Result::Fail("null info");
    *info = {};

    int fd = ::open(spidev.c_str(), O_RDWR);
    if (fd < 0) {
        return Result::Fail("open " + spidev + ": errno " + std::to_string(errno));
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed_hz = 16'000'000;  // matches what flashrom uses for these parts
    ::ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ::ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ::ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz);
    ::ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed_hz);
    info->spi_speed_hz = speed_hz;

    // JEDEC ID: cmd 0x9F, response is 3 bytes after the command byte.
    {
        std::vector<uint8_t> tx{0x9F, 0, 0, 0};
        std::vector<uint8_t> rx;
        if (!SpiTxRx(fd, tx, &rx, speed_hz)) {
            ::close(fd);
            return Result::Fail("SPI_IOC_MESSAGE (JEDEC) failed: errno " +
                                std::to_string(errno));
        }
        info->jedec_manufacturer = rx[1];
        info->jedec_device = static_cast<uint16_t>((rx[2] << 8) | rx[3]);
    }

    // Refuse to report stuck-bus garbage as a real ID.
    if (info->jedec_manufacturer == 0x00 || info->jedec_manufacturer == 0xFF) {
        ::close(fd);
        return Result::Fail("JEDEC read returned " +
                            std::to_string(info->jedec_manufacturer) +
                            " — no SPI flash responding on " + spidev);
    }

    // Unique ID: cmd 0x4B + 4 dummy bytes + 8 ID bytes. Best-effort: many
    // vendors implement this (Winbond, Macronix, GigaDevice). On unsupported
    // parts the bus returns all-0x00 or all-0xFF — we treat those as absent.
    {
        std::vector<uint8_t> tx(1 + 4 + 8, 0);
        tx[0] = 0x4B;
        std::vector<uint8_t> rx;
        if (SpiTxRx(fd, tx, &rx, speed_hz)) {
            std::vector<uint8_t> id(rx.begin() + 5, rx.end());
            if (!AllSameByte(id, 0x00) && !AllSameByte(id, 0xFF)) {
                info->unique_id = std::move(id);
            }
        }
    }

    ::close(fd);
    return Result::Ok();
}

std::string BootloaderBuildTimestamp() {
    std::string raw;
    if (!android::base::ReadFileToString(
            "/proc/device-tree/chosen/bootloader/build-timestamp", &raw)) {
        return {};
    }
    // Device-tree exposes this as a 4-byte big-endian integer. Decode it.
    if (raw.size() >= 4) {
        uint32_t ts = (static_cast<uint8_t>(raw[0]) << 24) |
                      (static_cast<uint8_t>(raw[1]) << 16) |
                      (static_cast<uint8_t>(raw[2]) << 8)  |
                       static_cast<uint8_t>(raw[3]);
        return std::to_string(ts);
    }
    return android::base::Trim(raw);
}

std::string BytesToHex(const std::vector<uint8_t>& buf) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(buf.size() * 2);
    for (size_t i = 0; i < buf.size(); ++i) {
        out[i * 2] = hex[buf[i] >> 4];
        out[i * 2 + 1] = hex[buf[i] & 0xf];
    }
    return out;
}

std::string Sha256Hex(const std::vector<uint8_t>& buf) {
    std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
    SHA256(buf.data(), buf.size(), digest.data());
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    return out;
}

}  // namespace rpi::eeprom
