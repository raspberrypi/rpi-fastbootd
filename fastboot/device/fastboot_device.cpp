/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "fastboot_device.h"

#include <algorithm>
#include <exception>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include "constants.h"
#include "flashing.h"
#include "tcp_client.h"
#include "usb_client.h"

using std::string_literals::operator""s;

namespace sph = std::placeholders;

std::unordered_map<std::string, CommandHandler> FastbootDevice::BuildCommandMap() {
    return {
              {FB_CMD_SET_ACTIVE, SetActiveHandler},
              {FB_CMD_DOWNLOAD, DownloadHandler},
              {"stage", DownloadHandler},  // Compat: older rpi-imager sends "stage:" on the wire
              {FB_CMD_UPLOAD, UploadHandler},
              {FB_CMD_GETVAR, GetVarHandler},
              {FB_CMD_SHUTDOWN, ShutDownHandler},
              {FB_CMD_REBOOT, RebootHandler},
              {FB_CMD_REBOOT_BOOTLOADER, RebootBootloaderHandler},
            //   {FB_CMD_REBOOT_FASTBOOT, RebootFastbootHandler},
            //   {FB_CMD_REBOOT_RECOVERY, RebootRecoveryHandler},
              {FB_CMD_ERASE, EraseHandler},
              {FB_CMD_FLASH, FlashHandler},
              {FB_CMD_OEM, OemCmdHandler},
              {FB_CMD_FETCH, FetchHandler},
    };
}

// Restricted map for TCP workers running alongside a USB control plane.
// Only download/flash plus a version-gated getvar (needed for the upstream
// `fastboot` CLI's connection handshake) are allowed; everything else FAILs.
std::unordered_map<std::string, CommandHandler> FastbootDevice::BuildDataPlaneCommandMap() {
    return {
              {FB_CMD_DOWNLOAD, DownloadHandler},
              {FB_CMD_FLASH, FlashHandler},
              {FB_CMD_GETVAR, GetVarHandlerDataPlane},
    };
}

FastbootDevice::FastbootDevice(const char* mode)
    : kCommandMap(BuildCommandMap()),
      active_slot_("") {
    std::string modestr = mode;
    if (modestr.find("tcp") != std::string::npos) {
        transport_ = std::make_unique<ClientTcpTransport>();
    } else {
        transport_ = std::make_unique<ClientUsbTransport>();
    }
}

FastbootDevice::FastbootDevice(std::unique_ptr<Transport> transport,
                               bool data_plane_only)
    : kCommandMap(data_plane_only ? BuildDataPlaneCommandMap() : BuildCommandMap()),
      transport_(std::move(transport)),
      active_slot_("") {}

FastbootDevice::~FastbootDevice() {
    CloseDevice();
}

void FastbootDevice::CloseDevice() {
    transport_->Close();
}

std::string FastbootDevice::GetCurrentSlot() {
    // // Check if a set_active ccommand was issued earlier since the boot control HAL
    // // returns the slot that is currently booted into.
    // if (!active_slot_.empty()) {
    //     return active_slot_;
    // }
    // // Non-A/B devices must not have boot control HALs.
    // if (!boot_control_hal_) {
    //     return "";
    // }
    // std::string suffix = boot_control_hal_->GetSuffix(boot_control_hal_->GetCurrentSlot());
    return "";
}

// BootControlClient* FastbootDevice::boot1_1() const {
//     if (boot_control_hal_->GetVersion() >= android::hal::BootControlVersion::BOOTCTL_V1_1) {
//         return boot_control_hal_.get();
//     }
//     return nullptr;
// }

bool FastbootDevice::WriteStatus(FastbootResult result, const std::string& message) {
    constexpr size_t kResponseReasonSize = 4;
    constexpr size_t kNumResponseTypes = 4;  // "FAIL", "OKAY", "INFO", "DATA"

    char buf[FB_RESPONSE_SZ];
    constexpr size_t kMaxMessageSize = sizeof(buf) - kResponseReasonSize;
    size_t msg_len = std::min(kMaxMessageSize, message.size());

    constexpr const char* kResultStrings[kNumResponseTypes] = {RESPONSE_OKAY, RESPONSE_FAIL,
                                                               RESPONSE_INFO, RESPONSE_DATA};

    if (static_cast<size_t>(result) >= kNumResponseTypes) {
        return false;
    }

    memcpy(buf, kResultStrings[static_cast<size_t>(result)], kResponseReasonSize);
    memcpy(buf + kResponseReasonSize, message.c_str(), msg_len);

    // A silently clipped message reads as a whole one, which is worse than a
    // short message: the reader cannot tell a complete reason from half of a
    // different one. Mark the cut so it is visible, and say so in the log where
    // the whole text still exists.
    //
    // Only for the human-facing results. An OKAY payload is a value the client
    // parses -- getvar, among others -- so it stays byte-exact and a caller
    // that overruns the buffer there has a bug this must not paper over.
    if (message.size() > msg_len) {
        LOG(WARNING) << "Response truncated to " << msg_len << " bytes: " << message;
        if (result == FastbootResult::FAIL || result == FastbootResult::INFO) {
            constexpr char kEllipsis[] = "...";
            constexpr size_t kEllipsisLen = sizeof(kEllipsis) - 1;
            static_assert(kMaxMessageSize > kEllipsisLen,
                          "response buffer too small to mark a truncated message");
            memcpy(buf + kResponseReasonSize + msg_len - kEllipsisLen, kEllipsis, kEllipsisLen);
        }
    }

    size_t response_len = kResponseReasonSize + msg_len;
    auto write_ret = this->get_transport()->Write(buf, response_len);
    if (write_ret != static_cast<ssize_t>(response_len)) {
        PLOG(ERROR) << "Failed to write " << message;
        return false;
    }

    return true;
}

bool FastbootDevice::HandleData(bool read, std::vector<char>* data) {
    return HandleData(read, data->data(), data->size());
}

bool FastbootDevice::HandleData(bool read, char* data, uint64_t size) {
    auto read_write_data_size = read ? this->get_transport()->Read(data, size)
                                     : this->get_transport()->Write(data, size);
    if (read_write_data_size == -1) {
        LOG(ERROR) << (read ? "read from" : "write to") << " transport failed";
        return false;
    }
    if (static_cast<size_t>(read_write_data_size) != size) {
        LOG(ERROR) << (read ? "read" : "write") << " expected " << size << " bytes, got "
                   << read_write_data_size;
        return false;
    }
    return true;
}

void FastbootDevice::ExecuteCommands() {
    char command[FB_RESPONSE_SZ + 1];
    for (;;) {
        auto bytes_read = transport_->Read(command, FB_RESPONSE_SZ);
        if (bytes_read == 0) {
            // Clean peer disconnect between commands — normal end of a
            // fastboot session, not an error worth alarming on.
            LOG(INFO) << "Peer closed connection; ending session";
            return;
        }
        if (bytes_read < 0) {
            PLOG(ERROR) << "Couldn't read command";
            return;
        }
        if (std::count_if(command, command + bytes_read, iscntrl) != 0) {
            WriteStatus(FastbootResult::FAIL,
                        "Command contains control character");
            continue;
        }
        command[bytes_read] = '\0';

        LOG(INFO) << "Fastboot command: " << command;

        std::vector<std::string> args;
        std::string cmd_name;
        if (android::base::StartsWith(command, "oem ")) {
            args = {command};
            cmd_name = FB_CMD_OEM;
        } else {
            args = android::base::Split(command, ":");
            cmd_name = args[0];
        }

        auto found_command = kCommandMap.find(cmd_name);
        if (found_command == kCommandMap.end()) {
            WriteStatus(FastbootResult::FAIL, "Unrecognized command " + args[0]);
            continue;
        }
        // Backstop: nothing between here and main() catches, so an exception
        // out of a handler would terminate fastbootd. Losing the daemon
        // mid-session costs the host its transport and any reason with it.
        //
        // End the session rather than continue: the handler stopped somewhere
        // it did not choose to, and this loop cannot know what it left behind.
        // Handlers that can clean up after themselves should still do so.
        bool keep_going = false;
        try {
            keep_going = found_command->second(this, args);
        } catch (const std::exception& e) {
            LOG(ERROR) << "Command '" << command << "' threw: " << e.what();
            WriteStatus(FastbootResult::FAIL, std::string("internal error: ") + e.what());
            return;
        } catch (...) {
            LOG(ERROR) << "Command '" << command << "' threw a non-standard exception";
            WriteStatus(FastbootResult::FAIL, "internal error");
            return;
        }

        if (!keep_going) {
            return;
        }
    }
}

bool FastbootDevice::WriteOkay(const std::string& message) {
    return WriteStatus(FastbootResult::OKAY, message);
}

bool FastbootDevice::WriteFail(const std::string& message) {
    return WriteStatus(FastbootResult::FAIL, message);
}

bool FastbootDevice::WriteInfo(const std::string& message) {
    return WriteStatus(FastbootResult::INFO, message);
}
