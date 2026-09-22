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

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rpi::otp {

struct Result {
    bool ok{false};
    std::string err{};
    static Result Ok() { return {true, {}}; }
    static Result Fail(std::string e) { return {false, std::move(e)}; }
};

using Mac = std::array<uint8_t, 6>;

// Addressed by index, so the same width on every part though the absolute rows
// differ: 36-43 pre-BCM2712, 77-84 on BCM2712.
constexpr uint32_t kCustomerRows = 8;

enum class MacInterface { Ethernet, Wifi, Bluetooth };

bool ParseMacInterface(const std::string& name, MacInterface* out);
const char* MacInterfaceName(MacInterface iface);

Result ParseMac(const std::string& text, Mac* out);
std::string FormatMac(const Mac& mac);

// Rejects what the firmware refuses or disregards: multicast, all-zero (how an
// unprogrammed row reads) and all-ones ("no override").
Result ValidateMac(const Mac& mac);

bool MacIsBlank(const Mac& mac);
bool MacIsIgnored(const Mac& mac);

// Octet order the MAC tags use: first octet in the low byte of word 0.
void MacToWords(const Mac& mac, uint32_t words[2]);
Mac MacFromWords(const uint32_t words[2]);

Result GetCustomerMac(MacInterface iface, Mac* out);

// Fuses only blow from 0 to 1, so a write over a differently populated row ORs
// the two values together. Both writers read first and refuse rather than
// corrupt.
enum class WriteOutcome { Programmed, AlreadyMatches };

Result SetCustomerMac(MacInterface iface, const Mac& mac, WriteOutcome* outcome);

// Absolute rows, as `vcgencmd otp_dump` numbers them. False where no such
// device exists: every pre-BCM2712 part.
bool ReadOtpRows(uint32_t start, uint32_t count, std::vector<uint32_t>* out);

Result ReadCustomerOtp(uint32_t start, uint32_t count, std::vector<uint32_t>* out);
Result WriteCustomerOtp(uint32_t start, const std::vector<uint32_t>& values,
                        WriteOutcome* outcome);

}  // namespace rpi::otp
