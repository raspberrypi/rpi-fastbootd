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
#include <optional>
#include <string>
#include <sys/types.h>

namespace rpi::file_write {

struct Result {
    bool ok{false};
    std::string err{};
    static Result Ok() { return {true, {}}; }
    static Result Fail(std::string e) { return {false, std::move(e)}; }
};

// Permission bits written in octal, at most 0777; nullopt for anything else.
std::optional<mode_t> ParseMode(const std::string& text);

// Replace PATH's contents with DATA, reporting any write, sync or close error.
// Without MODE a new file gets 0666 less the umask and an existing one keeps
// its mode, as oem download-file always has. A MODE is set before any data is
// written, so a secret is never readable at a looser mode; on vfat, which
// cannot keep one, the write fails.
Result WriteFile(const std::string& path, const char* data, std::size_t size,
                 std::optional<mode_t> mode);

}  // namespace rpi::file_write
