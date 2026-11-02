/*
 * Copyright (C) 2026 Raspberry Pi Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

#include "file_write.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rpi::file_write {

std::optional<mode_t> ParseMode(const std::string& text) {
    if (text.empty() || text.size() > 4) return std::nullopt;
    mode_t mode = 0;
    for (char c : text) {
        if (c < '0' || c > '7') return std::nullopt;
        mode = mode * 8 + (c - '0');
    }
    if (mode > 0777) return std::nullopt;
    return mode;
}

Result WriteFile(const std::string& path, const char* data, std::size_t size,
                 std::optional<mode_t> mode) {
    // Checked before opening: the open truncates, and an empty stage used to
    // wipe the file it then failed to write.
    if (size == 0) {
        return Result::Fail("Buffer size zero. Check file was successfully staged");
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode.value_or(0666));
    if (fd < 0) {
        return Result::Fail("Error opening file, ERRNO: " + std::to_string(errno));
    }
    // open() only applies a mode to a file it creates, less the umask. vfat
    // takes its modes from the mount and accepts fchmod without applying it,
    // so read the mode back rather than trust the call.
    if (mode) {
        struct stat st{};
        if (fchmod(fd, *mode) != 0 || fstat(fd, &st) != 0 || (st.st_mode & 07777) != *mode) {
            close(fd);
            return Result::Fail("Cannot set mode on " + path + ": the filesystem does not keep it");
        }
    }
    std::size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int e = n < 0 ? errno : EIO;
            close(fd);
            return Result::Fail("Write failed on " + path + ": " + strerror(e));
        }
        done += static_cast<std::size_t>(n);
    }
    // ext4 allocates on writeback, so a full disk can first show here. Device
    // nodes and sysfs files have nothing to sync and say EINVAL.
    if (fsync(fd) != 0 && errno != EINVAL) {
        int e = errno;
        close(fd);
        return Result::Fail("Sync failed on " + path + ": " + strerror(e));
    }
    if (close(fd) != 0) {
        return Result::Fail("Close failed on " + path + ": " + strerror(errno));
    }
    return Result::Ok();
}

}  // namespace rpi::file_write
