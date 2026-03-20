/*
 * Copyright (C) 2025 Raspberry Pi Ltd
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

// Test harness for librpifwcrypto via the rpi::RpiFwCrypto C++ wrapper.
//
// Requires:
//   - Raspberry Pi 4B/5 hardware
//   - librpifwcrypto0 runtime package installed
//   - /dev/vcio accessible (firmware mailbox device)
//
// Tags:
//   [fwcrypto]       - all firmware crypto tests
//   [status]         - key status queries
//   [hmac]           - HMAC-SHA256 operations
//   [pubkey]         - public key retrieval
//   [provisioned]    - requires a provisioned key (skipped if not)

#include "utility.h"

// android-base/logging.h defines CHECK as a fatal assertion macro,
// which collides with Catch2's CHECK. Undefine it before including Catch2.
#undef CHECK

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Helper: can we talk to the firmware mailbox?
static bool is_firmware_accessible() {
    rpi::RpiFwCrypto crypto;
    auto status = crypto.GetCachedProvisioningStatus();
    return status.has_value();
}

// Helper: is the device key provisioned?
static bool is_key_provisioned() {
    rpi::RpiFwCrypto crypto;
    auto status = crypto.GetCachedProvisioningStatus();
    return status.has_value() && *status;
}

#define REQUIRE_FIRMWARE()                              \
    do {                                                \
        if (!is_firmware_accessible()) {                \
            SKIP("Firmware mailbox not accessible");    \
        }                                              \
    } while (0)

#define REQUIRE_PROVISIONED()                           \
    do {                                                \
        REQUIRE_FIRMWARE();                             \
        if (!is_key_provisioned()) {                    \
            SKIP("Key slot is not provisioned");        \
        }                                              \
    } while (0)

// ── Construction & status ───────────────────────────────────────────

TEST_CASE("RpiFwCrypto constructs without throwing", "[fwcrypto][status]") {
    REQUIRE_NOTHROW(rpi::RpiFwCrypto{});
}

TEST_CASE("GetCachedProvisioningStatus returns a value", "[fwcrypto][status]") {
    REQUIRE_FIRMWARE();
    rpi::RpiFwCrypto crypto;
    auto status = crypto.GetCachedProvisioningStatus();
    REQUIRE(status.has_value());
    INFO("Key provisioned: " << (*status ? "yes" : "no"));
}

TEST_CASE("GetKeyStatusString returns non-empty string", "[fwcrypto][status]") {
    REQUIRE_FIRMWARE();
    rpi::RpiFwCrypto crypto;
    std::string status_str = crypto.GetKeyStatusString();
    CHECK(!status_str.empty());
    CHECK(status_str != "Failed to get key status");
    INFO("Key status: " << status_str);
}

TEST_CASE("Multiple RpiFwCrypto instances share cached status", "[fwcrypto][status]") {
    REQUIRE_FIRMWARE();
    rpi::RpiFwCrypto a;
    rpi::RpiFwCrypto b;
    auto sa = a.GetCachedProvisioningStatus();
    auto sb = b.GetCachedProvisioningStatus();
    REQUIRE(sa.has_value());
    REQUIRE(sb.has_value());
    CHECK(*sa == *sb);
}

// ── HMAC-SHA256 ─────────────────────────────────────────────────────

TEST_CASE("CalculateHmac succeeds with provisioned key", "[fwcrypto][hmac][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    std::string message = "test message for hmac";
    std::vector<uint8_t> msg(message.begin(), message.end());

    auto result = crypto.CalculateHmac(msg);
    REQUIRE(result.has_value());

    // HMAC-SHA256 hex string should be exactly 64 characters
    CHECK(result->size() == 64);

    // Should not be all zeros
    CHECK(*result != std::string(64, '0'));
}

TEST_CASE("CalculateHmac is deterministic", "[fwcrypto][hmac][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    std::string message = "determinism check";
    std::vector<uint8_t> msg(message.begin(), message.end());

    auto r1 = crypto.CalculateHmac(msg);
    auto r2 = crypto.CalculateHmac(msg);

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(*r1 == *r2);
}

TEST_CASE("CalculateHmac differs for different messages", "[fwcrypto][hmac][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    std::vector<uint8_t> msg_a = {'A'};
    std::vector<uint8_t> msg_b = {'B'};

    auto ra = crypto.CalculateHmac(msg_a);
    auto rb = crypto.CalculateHmac(msg_b);

    REQUIRE(ra.has_value());
    REQUIRE(rb.has_value());
    CHECK(*ra != *rb);
}

TEST_CASE("CalculateHmac handles empty message", "[fwcrypto][hmac][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    std::vector<uint8_t> empty;

    auto result = crypto.CalculateHmac(empty);

    // Library may accept or reject empty input; just ensure no crash
    if (result.has_value()) {
        CHECK(result->size() == 64);
    } else {
        INFO("Empty message rejected with status: " << static_cast<int>(result.error()));
    }
}

TEST_CASE("CalculateHmac handles max-size message", "[fwcrypto][hmac][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    std::vector<uint8_t> big(RPI_FW_CRYPTO_HMAC_MSG_MAX_SIZE, 0xAA);

    auto result = crypto.CalculateHmac(big);
    REQUIRE(result.has_value());
    CHECK(result->size() == 64);
}

TEST_CASE("CalculateHmac returns valid hex characters", "[fwcrypto][hmac][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    std::vector<uint8_t> msg = {0x01, 0x02, 0x03};

    auto result = crypto.CalculateHmac(msg);
    REQUIRE(result.has_value());

    for (char c : *result) {
        bool valid_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(valid_hex);
    }
}

// ── Public key retrieval ────────────────────────────────────────────

TEST_CASE("GetPublicKey succeeds with provisioned key", "[fwcrypto][pubkey][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    auto result = crypto.GetPublicKey();

    REQUIRE(result.has_value());
    // Should be a PEM-encoded public key
    CHECK(result->find("-----BEGIN PUBLIC KEY-----") != std::string::npos);
    CHECK(result->find("-----END PUBLIC KEY-----") != std::string::npos);
}

TEST_CASE("GetPublicKey is stable across calls", "[fwcrypto][pubkey][provisioned]") {
    REQUIRE_PROVISIONED();

    rpi::RpiFwCrypto crypto;
    auto r1 = crypto.GetPublicKey();
    auto r2 = crypto.GetPublicKey();

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(*r1 == *r2);
}
