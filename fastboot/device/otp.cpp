/*
 * Copyright (C) 2026 Raspberry Pi Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "otp.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rpi::otp {

namespace {

// The kernel's filtered mailbox nodes permit these write tags but none of the
// matching reads, so reads use nvmem or the unrestricted node.
constexpr char kVcio[] = "/dev/vcio";
constexpr char kVcioProvisioning[] = "/dev/vcio_provisioning";
constexpr char kNvmemDir[] = "/sys/bus/nvmem/devices";
constexpr char kNvmemCustomer[] = "nvmem_cust";
constexpr char kNvmemAll[] = "nvmem_otp";

constexpr unsigned long kIoctlMboxProperty = _IOWR(100, 0, char*);

constexpr uint32_t kRequestSucceeded = 0x80000000u;

constexpr uint32_t kTagGetCustomerOtp = 0x00030021u;
constexpr uint32_t kTagSetCustomerOtp = 0x00038021u;
constexpr uint32_t kTagGetCustomerMac = 0x00030082u;
constexpr uint32_t kTagSetCustomerMac = 0x00038082u;

constexpr uint32_t kMacValueBytes = 6;

std::string Hex32(uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08x", value);
    return buf;
}

uint32_t MacTagOffset(MacInterface iface) {
    switch (iface) {
        case MacInterface::Wifi:
            return 1;
        case MacInterface::Bluetooth:
            return 2;
        case MacInterface::Ethernet:
            break;
    }
    return 0;
}

// The request code repeats `value_bytes` rather than the documented zero,
// matching the vcmailbox invocations these tags were specified against.
Result MboxProperty(const char* device, uint32_t tag, uint32_t value_bytes,
                    std::vector<uint32_t>* value) {
    const size_t value_words = (value_bytes + 3) / 4;
    value->resize(value_words, 0);

    std::vector<uint32_t> buf;
    buf.reserve(value_words + 6);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(tag);
    buf.push_back(value_bytes);
    buf.push_back(value_bytes);
    buf.insert(buf.end(), value->begin(), value->end());
    buf.push_back(0);
    buf[0] = static_cast<uint32_t>(buf.size() * sizeof(uint32_t));

    int fd = ::open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return Result::Fail(std::string("cannot open ") + device + ": " + strerror(errno));
    }
    int rc = ::ioctl(fd, kIoctlMboxProperty, buf.data());
    int saved_errno = errno;
    ::close(fd);
    if (rc < 0) {
        return Result::Fail("mailbox ioctl for tag " + Hex32(tag) + " failed: " +
                            strerror(saved_errno));
    }
    if (buf[1] != kRequestSucceeded) {
        return Result::Fail("firmware rejected tag " + Hex32(tag) + " (status " +
                            Hex32(buf[1]) + ")");
    }
    // An unhandled tag keeps its request code.
    if (!(buf[4] & kRequestSucceeded)) {
        return Result::Fail("firmware does not implement tag " + Hex32(tag) +
                            "; this SoC or bootloader does not support it");
    }
    value->assign(buf.begin() + 5, buf.begin() + 5 + value_words);
    return Result::Ok();
}

const char* WriteDevice() {
    return ::access(kVcioProvisioning, F_OK) == 0 ? kVcioProvisioning : kVcio;
}

// Each row is a native-endian word. False where there is no such device.
bool ReadNvmemRows(const char* device, size_t device_len, uint32_t start, uint32_t count,
                   std::vector<uint32_t>* out) {
    std::string path;
    if (DIR* dir = ::opendir(kNvmemDir)) {
        while (const dirent* entry = ::readdir(dir)) {
            if (strncmp(entry->d_name, device, device_len) == 0) {
                path = std::string(kNvmemDir) + "/" + entry->d_name + "/nvmem";
                break;
            }
        }
        ::closedir(dir);
    }
    if (path.empty()) {
        return false;
    }
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    std::vector<uint32_t> rows(count, 0);
    const ssize_t wanted = static_cast<ssize_t>(count * sizeof(uint32_t));
    ssize_t got = ::pread(fd, rows.data(), static_cast<size_t>(wanted),
                          static_cast<off_t>(start * sizeof(uint32_t)));
    ::close(fd);
    if (got != wanted) {
        return false;
    }
    *out = std::move(rows);
    return true;
}

bool HexDigit(char c, uint8_t* value) {
    if (c >= '0' && c <= '9') {
        *value = static_cast<uint8_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        *value = static_cast<uint8_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
        *value = static_cast<uint8_t>(c - 'A' + 10);
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool ParseMacInterface(const std::string& name, MacInterface* out) {
    if (name == "ethernet" || name == "eth") {
        *out = MacInterface::Ethernet;
    } else if (name == "wifi" || name == "wlan") {
        *out = MacInterface::Wifi;
    } else if (name == "bluetooth" || name == "bt") {
        *out = MacInterface::Bluetooth;
    } else {
        return false;
    }
    return true;
}

const char* MacInterfaceName(MacInterface iface) {
    switch (iface) {
        case MacInterface::Wifi:
            return "wifi";
        case MacInterface::Bluetooth:
            return "bluetooth";
        case MacInterface::Ethernet:
            break;
    }
    return "ethernet";
}

Result ParseMac(const std::string& text, Mac* out) {
    if (text.size() != 17) {
        return Result::Fail("'" + text + "' is not a MAC address; wanted aa:bb:cc:dd:ee:ff");
    }
    const char separator = text[2];
    if (separator != ':' && separator != '-') {
        return Result::Fail("'" + text + "' is not separated by ':' or '-'");
    }
    for (size_t octet = 0; octet < out->size(); octet++) {
        const size_t at = octet * 3;
        if (octet > 0 && text[at - 1] != separator) {
            return Result::Fail("'" + text + "' has inconsistent separators");
        }
        uint8_t high = 0, low = 0;
        if (!HexDigit(text[at], &high) || !HexDigit(text[at + 1], &low)) {
            return Result::Fail("'" + text + "' has a non-hexadecimal octet");
        }
        (*out)[octet] = static_cast<uint8_t>((high << 4) | low);
    }
    return Result::Ok();
}

std::string FormatMac(const Mac& mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return buf;
}

bool MacIsBlank(const Mac& mac) {
    for (uint8_t octet : mac) {
        if (octet != 0x00) return false;
    }
    return true;
}

bool MacIsIgnored(const Mac& mac) {
    for (uint8_t octet : mac) {
        if (octet != 0xff) return false;
    }
    return true;
}

Result ValidateMac(const Mac& mac) {
    if (MacIsBlank(mac)) {
        return Result::Fail("00:00:00:00:00:00 is how an unprogrammed row reads");
    }
    if (MacIsIgnored(mac)) {
        return Result::Fail("ff:ff:ff:ff:ff:ff is ignored by the bootloader");
    }
    if (mac[0] & 0x01) {
        return Result::Fail(FormatMac(mac) + " is a multicast address");
    }
    return Result::Ok();
}

void MacToWords(const Mac& mac, uint32_t words[2]) {
    words[0] = static_cast<uint32_t>(mac[0]) | (static_cast<uint32_t>(mac[1]) << 8) |
               (static_cast<uint32_t>(mac[2]) << 16) | (static_cast<uint32_t>(mac[3]) << 24);
    words[1] = static_cast<uint32_t>(mac[4]) | (static_cast<uint32_t>(mac[5]) << 8);
}

Mac MacFromWords(const uint32_t words[2]) {
    return Mac{static_cast<uint8_t>(words[0] & 0xff),
               static_cast<uint8_t>((words[0] >> 8) & 0xff),
               static_cast<uint8_t>((words[0] >> 16) & 0xff),
               static_cast<uint8_t>((words[0] >> 24) & 0xff),
               static_cast<uint8_t>(words[1] & 0xff),
               static_cast<uint8_t>((words[1] >> 8) & 0xff)};
}

// Not the raw MAC rows nvmem also exposes: the factory rows pack an address the
// opposite way round from these tags, so that block's order is not safe to
// assume.
Result GetCustomerMac(MacInterface iface, Mac* out) {
    std::vector<uint32_t> value(2, 0);
    Result r = MboxProperty(kVcio, kTagGetCustomerMac + MacTagOffset(iface), kMacValueBytes,
                            &value);
    if (!r.ok) return r;
    *out = MacFromWords(value.data());
    return Result::Ok();
}

Result SetCustomerMac(MacInterface iface, const Mac& mac, WriteOutcome* outcome) {
    Result valid = ValidateMac(mac);
    if (!valid.ok) return valid;

    Mac current{};
    Result read = GetCustomerMac(iface, &current);
    if (!read.ok) return read;
    if (current == mac) {
        *outcome = WriteOutcome::AlreadyMatches;
        return Result::Ok();
    }
    if (!MacIsBlank(current)) {
        return Result::Fail("customer " + std::string(MacInterfaceName(iface)) +
                            " MAC is already programmed as " + FormatMac(current) +
                            "; OTP bits cannot be cleared to write " + FormatMac(mac));
    }

    std::vector<uint32_t> value(2, 0);
    MacToWords(mac, value.data());
    Result write = MboxProperty(WriteDevice(), kTagSetCustomerMac + MacTagOffset(iface),
                                kMacValueBytes, &value);
    if (!write.ok) return write;

    Mac written{};
    read = GetCustomerMac(iface, &written);
    if (!read.ok) return read;
    if (written != mac) {
        return Result::Fail("wrote " + FormatMac(mac) + " but the row reads back as " +
                            FormatMac(written));
    }
    *outcome = WriteOutcome::Programmed;
    return Result::Ok();
}

bool ReadOtpRows(uint32_t start, uint32_t count, std::vector<uint32_t>* out) {
    if (count == 0) {
        return false;
    }
    return ReadNvmemRows(kNvmemAll, sizeof(kNvmemAll) - 1, start, count, out);
}

Result ReadCustomerOtp(uint32_t start, uint32_t count, std::vector<uint32_t>* out) {
    if (count == 0) {
        return Result::Fail("asked for no rows");
    }
    if (start >= kCustomerRows || count > kCustomerRows - start) {
        return Result::Fail("rows " + std::to_string(start) + "-" +
                            std::to_string(start + count - 1) + " are outside the customer " +
                            "region, which is rows 0-" + std::to_string(kCustomerRows - 1));
    }

    if (ReadNvmemRows(kNvmemCustomer, sizeof(kNvmemCustomer) - 1, start, count, out)) {
        return Result::Ok();
    }

    std::vector<uint32_t> value{start, count};
    Result r = MboxProperty(kVcio, kTagGetCustomerOtp, 8 + count * 4, &value);
    if (!r.ok) return r;
    out->assign(value.begin() + 2, value.begin() + 2 + count);
    return Result::Ok();
}

Result WriteCustomerOtp(uint32_t start, const std::vector<uint32_t>& values,
                        WriteOutcome* outcome) {
    const uint32_t count = static_cast<uint32_t>(values.size());
    std::vector<uint32_t> current;
    Result read = ReadCustomerOtp(start, count, &current);
    if (!read.ok) return read;

    bool all_match = true;
    for (uint32_t i = 0; i < count; i++) {
        if (current[i] == values[i]) continue;
        all_match = false;
        if (current[i] != 0) {
            return Result::Fail("customer row " + std::to_string(start + i) + " already holds " +
                                Hex32(current[i]) + "; OTP bits cannot be cleared to write " +
                                Hex32(values[i]));
        }
    }
    if (all_match) {
        *outcome = WriteOutcome::AlreadyMatches;
        return Result::Ok();
    }

    std::vector<uint32_t> value{start, count};
    value.insert(value.end(), values.begin(), values.end());
    Result write = MboxProperty(WriteDevice(), kTagSetCustomerOtp, 8 + count * 4, &value);
    if (!write.ok) return write;

    std::vector<uint32_t> written;
    read = ReadCustomerOtp(start, count, &written);
    if (!read.ok) return read;
    for (uint32_t i = 0; i < count; i++) {
        if (written[i] != values[i]) {
            return Result::Fail("wrote " + Hex32(values[i]) + " to customer row " +
                                std::to_string(start + i) + " but it reads back as " +
                                Hex32(written[i]));
        }
    }
    *outcome = WriteOutcome::Programmed;
    return Result::Ok();
}

}  // namespace rpi::otp
