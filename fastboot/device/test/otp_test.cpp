/*
 * Copyright (C) 2026 Raspberry Pi Ltd
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

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "otp.h"

using rpi::otp::Mac;

namespace {

Mac Parsed(const std::string& text) {
    Mac mac{};
    REQUIRE(rpi::otp::ParseMac(text, &mac).ok);
    return mac;
}

}  // namespace

TEST_CASE("MAC text round-trips", "[otp][mac]") {
    REQUIRE(rpi::otp::FormatMac(Parsed("2c:cf:67:e6:d5:e5")) == "2c:cf:67:e6:d5:e5");
    REQUIRE(rpi::otp::FormatMac(Parsed("2C-CF-67-E6-D5-E5")) == "2c:cf:67:e6:d5:e5");
    REQUIRE(Parsed("00:11:22:33:44:56") == Mac{0x00, 0x11, 0x22, 0x33, 0x44, 0x56});
}

TEST_CASE("Malformed MAC text is rejected", "[otp][mac]") {
    Mac mac{};
    REQUIRE_FALSE(rpi::otp::ParseMac("", &mac).ok);
    REQUIRE_FALSE(rpi::otp::ParseMac("2c:cf:67:e6:d5", &mac).ok);
    REQUIRE_FALSE(rpi::otp::ParseMac("2c:cf:67:e6:d5:e5:e6", &mac).ok);
    REQUIRE_FALSE(rpi::otp::ParseMac("2ccf67e6d5e5", &mac).ok);
    REQUIRE_FALSE(rpi::otp::ParseMac("2c:cf:67:e6:d5:gg", &mac).ok);
    REQUIRE_FALSE(rpi::otp::ParseMac("2c:cf-67:e6:d5:e5", &mac).ok);
    REQUIRE_FALSE(rpi::otp::ParseMac("2c/cf/67/e6/d5/e5", &mac).ok);
}

// The word order the firmware's MAC tags use, from the worked example in the
// Raspberry Pi OTP documentation: 11:22:33:44:55:66 is sent as
// 0x44332211 0x00006655, and aa:bb:cc:dd:ee:ff reads back as
// 0xddccbbaa 0x0000ffee.
TEST_CASE("MAC packs into mailbox words documentation order", "[otp][mac]") {
    uint32_t words[2] = {0, 0};
    rpi::otp::MacToWords(Parsed("11:22:33:44:55:66"), words);
    REQUIRE(words[0] == 0x44332211u);
    REQUIRE(words[1] == 0x00006655u);

    const uint32_t read_back[2] = {0xddccbbaau, 0x0000ffeeu};
    REQUIRE(rpi::otp::FormatMac(rpi::otp::MacFromWords(read_back)) == "aa:bb:cc:dd:ee:ff");

    rpi::otp::MacToWords(Parsed("2c:cf:67:e6:d5:e5"), words);
    REQUIRE(rpi::otp::MacFromWords(words) == Parsed("2c:cf:67:e6:d5:e5"));
}

TEST_CASE("Unwritable addresses are refused", "[otp][mac]") {
    REQUIRE(rpi::otp::ValidateMac(Parsed("2c:cf:67:e6:d5:e5")).ok);

    // A blank row reads as all zeroes, all ones is what the bootloader treats
    // as "no override", and the low bit of the first octet is multicast.
    REQUIRE_FALSE(rpi::otp::ValidateMac(Parsed("00:00:00:00:00:00")).ok);
    REQUIRE_FALSE(rpi::otp::ValidateMac(Parsed("ff:ff:ff:ff:ff:ff")).ok);
    REQUIRE_FALSE(rpi::otp::ValidateMac(Parsed("01:cf:67:e6:d5:e5")).ok);
    REQUIRE(rpi::otp::ValidateMac(Parsed("02:cf:67:e6:d5:e5")).ok);

    REQUIRE(rpi::otp::MacIsBlank(Parsed("00:00:00:00:00:00")));
    REQUIRE_FALSE(rpi::otp::MacIsBlank(Parsed("00:00:00:00:00:01")));
    REQUIRE(rpi::otp::MacIsIgnored(Parsed("ff:ff:ff:ff:ff:ff")));
    REQUIRE_FALSE(rpi::otp::MacIsIgnored(Parsed("fe:ff:ff:ff:ff:ff")));
}

TEST_CASE("Interface names map to the firmware's tag order", "[otp][mac]") {
    rpi::otp::MacInterface iface = rpi::otp::MacInterface::Wifi;

    REQUIRE(rpi::otp::ParseMacInterface("ethernet", &iface));
    REQUIRE(iface == rpi::otp::MacInterface::Ethernet);
    REQUIRE(rpi::otp::ParseMacInterface("eth", &iface));
    REQUIRE(iface == rpi::otp::MacInterface::Ethernet);
    REQUIRE(rpi::otp::ParseMacInterface("wifi", &iface));
    REQUIRE(iface == rpi::otp::MacInterface::Wifi);
    REQUIRE(rpi::otp::ParseMacInterface("bluetooth", &iface));
    REQUIRE(iface == rpi::otp::MacInterface::Bluetooth);
    REQUIRE(rpi::otp::ParseMacInterface("bt", &iface));
    REQUIRE(iface == rpi::otp::MacInterface::Bluetooth);

    REQUIRE_FALSE(rpi::otp::ParseMacInterface("", &iface));
    REQUIRE_FALSE(rpi::otp::ParseMacInterface("ethernet0", &iface));

    REQUIRE(std::string(rpi::otp::MacInterfaceName(rpi::otp::MacInterface::Ethernet)) ==
            "ethernet");
    REQUIRE(std::string(rpi::otp::MacInterfaceName(rpi::otp::MacInterface::Wifi)) == "wifi");
    REQUIRE(std::string(rpi::otp::MacInterfaceName(rpi::otp::MacInterface::Bluetooth)) ==
            "bluetooth");
}

// Range checks must refuse before any transaction: a start row of 0xffffffff is
// the firmware's irreversible lock command.
TEST_CASE("Customer rows outside the region are refused", "[otp][rows]") {
    std::vector<uint32_t> rows;

    REQUIRE_FALSE(rpi::otp::ReadCustomerOtp(0, 0, &rows).ok);
    REQUIRE_FALSE(rpi::otp::ReadCustomerOtp(rpi::otp::kCustomerRows, 1, &rows).ok);
    REQUIRE_FALSE(rpi::otp::ReadCustomerOtp(0, rpi::otp::kCustomerRows + 1, &rows).ok);
    REQUIRE_FALSE(rpi::otp::ReadCustomerOtp(rpi::otp::kCustomerRows - 1, 2, &rows).ok);
    REQUIRE_FALSE(rpi::otp::ReadCustomerOtp(0xffffffffu, 1, &rows).ok);

    // The absolute-row reader has no region to bound it, so an empty request
    // is the only refusal it owes a caller.
    REQUIRE_FALSE(rpi::otp::ReadOtpRows(0, 0, &rows));

    rpi::otp::WriteOutcome outcome = rpi::otp::WriteOutcome::Programmed;
    REQUIRE_FALSE(rpi::otp::WriteCustomerOtp(0, {}, &outcome).ok);
    REQUIRE_FALSE(
            rpi::otp::WriteCustomerOtp(rpi::otp::kCustomerRows - 1, {1, 2}, &outcome).ok);
}
