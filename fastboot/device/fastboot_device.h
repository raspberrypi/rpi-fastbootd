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

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "commands.h"
#include "transport.h"
#include "variables.h"

#include "idpdevice.h"

class SharedIDPContext;

class FastbootDevice {
  public:
    FastbootDevice(const char* mode="usb");
    explicit FastbootDevice(std::unique_ptr<Transport> transport,
                            std::shared_ptr<SharedIDPContext> shared_idp = nullptr);
    ~FastbootDevice();

    void CloseDevice();
    void ExecuteCommands();
    bool WriteStatus(FastbootResult result, const std::string& message);
    bool HandleData(bool read, std::vector<char>* data);
    bool HandleData(bool read, char* data, uint64_t size);
    std::string GetCurrentSlot();

    // Shortcuts for writing status results.
    bool WriteOkay(const std::string& message);
    bool WriteFail(const std::string& message);
    bool WriteInfo(const std::string& message);

    std::vector<char>& download_data() { return download_data_; }
    Transport* get_transport() { return transport_.get(); }

    void set_active_slot(const std::string& active_slot) { active_slot_ = active_slot; }

    std::unique_ptr<IDPdevice> idp;
    IDPdevice::CookiePtr idpcookie{nullptr, IDPcookieDeleter};

    // Non-null when running in multi-connection TCP mode.
    // Commands should prefer shared_idp over per-device idp when set.
    std::shared_ptr<SharedIDPContext> shared_idp;

  private:
    static std::unordered_map<std::string, CommandHandler> BuildCommandMap();

    const std::unordered_map<std::string, CommandHandler> kCommandMap;

    std::unique_ptr<Transport> transport_;
    std::vector<char> download_data_;
    std::string active_slot_;
};
