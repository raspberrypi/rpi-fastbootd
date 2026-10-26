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

#include <catch2/catch_test_macros.hpp>

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> MakeImageWithMfgVer(uint32_t mfg_ver) {
    const std::string marker = "MFG_VER: " + std::to_string(mfg_ver);
    std::vector<uint8_t> image(rpi::eeprom::kMinImageSize, 0);
    std::copy(marker.begin(), marker.end(), image.begin());
    return image;
}

}  // namespace

TEST_CASE("ImageMfgVer extracts embedded manufacturing version", "[eeprom]") {
    REQUIRE(rpi::eeprom::ImageMfgVer(MakeImageWithMfgVer(2)) == 2);
    REQUIRE(rpi::eeprom::ImageMfgVer(MakeImageWithMfgVer(42)) == 42);
    REQUIRE(rpi::eeprom::ImageMfgVer(std::vector<uint8_t>(128, 0)) == 0);
}

TEST_CASE("CheckMinBootVer runs without error for well-formed images", "[eeprom]") {
    REQUIRE(rpi::eeprom::CheckMinBootVer(MakeImageWithMfgVer(1)).ok);
}

namespace {

// A chosen/bootloader node in a temporary directory, removed afterwards.
class BootloaderNode {
public:
    BootloaderNode() {
        char tmpl[] = "/tmp/eeprom_test_XXXXXX";
        root_ = mkdtemp(tmpl);
        node_ = root_ + "/bootloader";
    }
    ~BootloaderNode() { std::filesystem::remove_all(root_); }

    void Create() { std::filesystem::create_directory(node_); }
    void WriteSigned(const std::string& raw) {
        Create();
        std::ofstream(node_ + "/signed", std::ios::binary) << raw;
    }
    const std::string& path() const { return node_; }

private:
    std::string root_;
    std::string node_;
};

}  // namespace

TEST_CASE("ReadSignedBits reads the bootloader's secure-boot bit-field", "[eeprom]") {
    BootloaderNode node;
    node.WriteSigned(std::string("\x00\x00\x00\x09", 4));
    uint32_t bits = 0;
    REQUIRE(rpi::eeprom::ReadSignedBits(&bits, node.path()));
    REQUIRE(bits == 9);
}

TEST_CASE("ReadSignedBits reads no secure boot from a bootloader without the property",
          "[eeprom]") {
    BootloaderNode node;
    node.Create();
    uint32_t bits = 0xffffffff;
    REQUIRE(rpi::eeprom::ReadSignedBits(&bits, node.path()));
    REQUIRE(bits == 0);
}

TEST_CASE("ReadSignedBits cannot tell without a bootloader node", "[eeprom]") {
    BootloaderNode node;
    uint32_t bits = 0;
    REQUIRE_FALSE(rpi::eeprom::ReadSignedBits(&bits, node.path()));
}

TEST_CASE("ReadSignedBits refuses a malformed property rather than reading 0", "[eeprom]") {
    BootloaderNode node;
    node.WriteSigned(std::string("\x00\x08", 2));
    uint32_t bits = 0;
    REQUIRE_FALSE(rpi::eeprom::ReadSignedBits(&bits, node.path()));
}
