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
