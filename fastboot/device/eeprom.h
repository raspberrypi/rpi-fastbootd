/*
 * Copyright (C) 2026 Raspberry Pi Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rpi::eeprom {

// Sanity bounds. We refuse to operate outside these — anything beyond
// implies either a probe failure or the wrong device was selected.
//   Min: 64 KiB  — smaller than this is implausible for a Pi bootloader SPI.
//   Max: 16 MiB  — larger than any SPI flash Raspberry Pi has shipped to date.
constexpr std::size_t kMinImageSize = 64 * 1024;
constexpr std::size_t kMaxImageSize = 16 * 1024 * 1024;

// Fallback used only if probing fails to report a size (it really shouldn't).
constexpr std::size_t kFallbackImageSize = 512 * 1024;

struct Result {
    bool ok{false};
    std::string err{};
    static Result Ok() { return {true, {}}; }
    static Result Fail(std::string e) { return {false, std::move(e)}; }
};

// Best-effort detection of the bootloader SPI device on the current platform.
// Empty string if no candidate exists.
std::string DetectSpiDev();

// Probe the chip and report its size in bytes. Returns 0 in size on failure
// (and populates err). Cheap to call — opens and closes a flashrom session.
Result QuerySize(const std::string& spidev, std::size_t* size_out);

Result Read(const std::string& spidev, std::vector<uint8_t>* out);
Result Write(const std::string& spidev, const std::vector<uint8_t>& image);
Result Verify(const std::string& spidev, const std::vector<uint8_t>& expected);

// Identity read directly from the SPI flash via /dev/spidevN.0 ioctls.
// No libflashrom dependency — uses standardised JEDEC commands.
struct ChipInfo {
    uint8_t  jedec_manufacturer{0};   // SPI cmd 0x9F byte 0
    uint16_t jedec_device{0};         // SPI cmd 0x9F bytes 1-2 (big-endian)
    std::vector<uint8_t> unique_id;   // SPI cmd 0x4B; empty if unsupported
    uint32_t spi_speed_hz{0};         // Bus speed used for the probe
};
Result QueryChipInfo(const std::string& spidev, ChipInfo* info);

// Bootloader build timestamp exposed by the running firmware. Returns the
// decimal seconds-since-epoch string from /proc/device-tree/chosen/bootloader/
// build-timestamp, or empty string if unavailable.
std::string BootloaderBuildTimestamp();

// Read a big-endian u32 device-tree property. Returns false if missing or short.
bool ReadDtUInt32(const char* path, uint32_t* value_out);

// Extract MFG_VER from an EEPROM image (scans for "MFG_VER: N" like
// rpi-eeprom-update). Returns 0 if not found.
uint32_t ImageMfgVer(const std::vector<uint8_t>& image);

// Reject images whose MFG_VER is below the board minimum. Mirrors
// rpi-eeprom-update with STRICT_MIN_VER_CHECK=1.
Result CheckMinBootVer(const std::vector<uint8_t>& image);

std::string BytesToHex(const std::vector<uint8_t>& buf);
std::string Sha256Hex(const std::vector<uint8_t>& buf);

}  // namespace rpi::eeprom
