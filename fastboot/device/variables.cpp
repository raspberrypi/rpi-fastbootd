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

#include "variables.h"

#include <algorithm>
#include <cstring>
#include <inttypes.h>
#include <stdio.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
// #include <android/hardware/boot/1.1/IBootControl.h>
// #include <ext4_utils/ext4_utils.h>
// #include <fs_mgr.h>
// #include <liblp/liblp.h>

// #include "BootControlClient.h"
#include "fastboot_device.h"
#include "flashing.h"
#include "utility.h"
#include "spawn.h"
#include "wait.h"

#ifdef FB_ENABLE_FETCH
static constexpr bool kEnableFetch = true;
#else
static constexpr bool kEnableFetch = false;
#endif

// using MergeStatus = android::hal::BootControlClient::MergeStatus;
// using aidl::android::hardware::fastboot::FileSystemType;
// using namespace android::fs_mgr;
using namespace std::string_literals;

constexpr char kFastbootProtocolVersion[] = "0.4";

bool GetVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    *message = kFastbootProtocolVersion;
    return true;
}

bool GetBootloaderVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/version", message);
    return true;
}

bool GetBasebandVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                        std::string* message) {
    *message = android::base::GetProperty("ro.build.expect.baseband", "");
    return true;
}

bool GetOsVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    *message = android::base::GetProperty("ro.build.version.release", "");
    return true;
}

bool GetVndkVersion(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                    std::string* message) {
    *message = android::base::GetProperty("ro.vndk.version", "");
    return true;
}

bool GetProduct(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    android::base::ReadFileToString("/sys/firmware/devicetree/base/model", message);
    return true;
}

bool GetSerial(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    std::string serial;
    if (android::base::ReadFileToString("/proc/device-tree/chosen/rpi-serial64", &serial)) {
        *message = serial;
    } else if (android::base::ReadFileToString("/proc/device-tree/serial-number", &serial)) {
        *message = serial;
    }
    return true;
}

bool GetRpiDuid(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                std::string *message)
{
    android::base::ReadFileToString("/proc/device-tree/chosen/rpi-duid", message);
    return true;
}

namespace {
    void inspectOtp(std::function<bool(std::string *, std::string *)> inspectorfn, std::string *message) {
        if (inspectorfn == nullptr) {
            return;
        }

        std::string otp_dump;
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/otp.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);
    
        char *arg[] = {"/usr/bin/vcgencmd", "otp_dump", NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/usr/bin/vcgencmd", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);
    
        if (ret)
        {
            *message = strerror(errno);
            return;
        }
        else if (subprocess_rc)
        {
            *message = "vcgencmd failed";
            return;
        }
        else
        {
            android::base::ReadFileToString("/tmp/otp.log", &otp_dump);
        }

        std::istringstream stream(otp_dump);
        std::string line;
        while (std::getline(stream, line)) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
    
                if (inspectorfn(&key, &value)) {
                    return;
                }
            }
        }
    }
} // namespace anonymous

bool GetRevisionProcessor(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                          std::string *message)
{
    inspectOtp([&message](std::string *key, std::string *value){
        if (*key == "32") {
            uint32_t value_int = std::stoul(*value, 0, 16);

            *message = android::base::StringPrintf("0x%X", (value_int & 0xF000) >> 12);
            return true;
        }
        return false;
    },message);

    return true;
}

bool GetRevisionManufacturer(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                             std::string *message)
{
    inspectOtp([&message](std::string *key, std::string *value) {
        if (*key == "32") {
            uint32_t value_int = std::stoul(*value, 0, 16);

            *message = android::base::StringPrintf("0x%X", (value_int & 0xF0000) >> 16);
            return true;
        }
        return false;
    },message);
    return true;
}

bool GetRevisionMemory(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                       std::string *message)
{
    inspectOtp([&message](std::string *key, std::string *value) {
        if (*key == "32") {
            uint32_t value_int = std::stoul(*value, 0, 16);

            *message = android::base::StringPrintf("0x%X", (value_int & 0x700000) >> 20);
            return true;
        }
        return false;
    },message);

    return true;
}

bool GetRevisionType(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                     std::string *message)
{
    inspectOtp([&message](std::string *key, std::string *value) {
        if (*key == "32") {
            uint32_t value_int = std::stoul(*value, 0, 16);

            *message = android::base::StringPrintf("0x%X", (value_int & 0x0FF0) >> 4);
            return true;
        }
        return false;
    },message);
    return true;
}

bool GetRevisionRevision(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                         std::string *message)
{
    inspectOtp([&message](std::string *key, std::string *value) {
        if (*key == "32") {
            uint32_t value_int = std::stoul(*value, 0, 16);

            *message = android::base::StringPrintf("0x%X", (value_int & 0x0F));
            return true;
        }
        return false;
    },message);

    return true;
}

bool GetMacEthernet(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                    std::string* message) {
    std::string eth_mac_lo, eth_mac_up;
    uint required = 2;
    inspectOtp([&eth_mac_lo, &eth_mac_up, &required](std::string *key, std::string *value) {
        if (*key == "50") {
            eth_mac_lo = value->substr(0, 4);
            // Insert colons between pairs
            for (size_t i = 2; i < eth_mac_lo.length(); i += 3) {
                eth_mac_lo.insert(i, ":");
            }
            required--;
        } else if (*key == "51") {
            eth_mac_up = *value;
            for (size_t i = 2; i < eth_mac_up.length(); i += 3) {
                eth_mac_up.insert(i, ":");
            }
            required--;
        }

        if (!required) {
            return true;
        } else {
            return false;
        }
    }, message);
    *message = eth_mac_up + ":" + eth_mac_lo;
    return true;
}

bool GetMacWifi(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                std::string *message)
{
    std::string wifi_mac_lo, wifi_mac_up;
    uint required = 2;
    inspectOtp([&wifi_mac_lo, &wifi_mac_up, &required](std::string *key, std::string *value) {
        if (*key == "52") {
            wifi_mac_lo = value->substr(0, 4);
            for (size_t i = 2; i < wifi_mac_lo.length(); i += 3) {
                wifi_mac_lo.insert(i, ":");
            }
            required--;
        } else if (*key == "53") {
            wifi_mac_up = *value;
            for (size_t i = 2; i < wifi_mac_up.length(); i += 3) {
                wifi_mac_up.insert(i, ":");
            }
            required--;
        }
        
        if (!required)
        {
            return true;
        } else {
            return false;
        }
    }, message);
    *message = wifi_mac_up + ":" + wifi_mac_lo;
    return true;
}

bool GetMacBt(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
              std::string *message)
{
    std::string mac_lo, mac_hi;
    uint required = 2;
    inspectOtp([&mac_lo, &mac_hi, &required](std::string *key, std::string *value) {
        if (*key == "54")
        {
            mac_lo = value->substr(0, 4);
            for (size_t i = 2; i < mac_lo.length(); i += 3)
            {
                mac_lo.insert(i, ":");
            }
            required--;
        }
        else if (*key == "55")
        {
            mac_hi = *value;
            for (size_t i = 2; i < mac_hi.length(); i += 3)
            {
                mac_hi.insert(i, ":");
            }
            required--;
        }
        
        if (!required)
        {
            return true;
        } else {
            return false;
        }
    }, message);
    *message = mac_hi + ":" + mac_lo;
    return true;
}

bool GetMmcCid(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
               std::string *message)
{
    std::string cid;
    android::base::ReadFileToString("/sys/block/mmcblk0/device/cid", &cid);
    *message = cid;
    return true;
}

bool GetMmcSectorSize(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                      std::string *message)
{
    std::string sector_size;
    android::base::ReadFileToString("/sys/block/mmcblk0/queue/hw_sector_size", &sector_size);
    *message = sector_size;
    return true;
}

bool GetMmcSectorCount(FastbootDevice * /* device */, const std::vector<std::string> & /* args */,
                       std::string *message)
{
    std::string total_sectors;
    android::base::ReadFileToString("/sys/block/mmcblk0/size", &total_sectors);
    *message = total_sectors;
    return true;
}

bool GetSignedEeprom(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                           std::string* message) {
    std::string signed_raw = {};
    if (android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/signed", &signed_raw)) {
        try {
            *message = (std::stoi(signed_raw) & (1 << 0)) ? "present" : "not present";
        } catch (const std::invalid_argument& e) {
            *message = "not available";
        } catch (const std::out_of_range& e) {
            *message = "not available";
        }
    } else {
        *message = "not available";
    }
    return true;
}

bool GetSignedDevkey(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                         std::string* message) {
    std::string signed_raw = {};
    if (android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/signed", &signed_raw)) {
        try {
            *message = (std::stoi(signed_raw) & (1 << 2)) ? "present" : "not present";
        } catch (const std::invalid_argument& e) {
            *message = "not available";
        } catch (const std::out_of_range& e) {
            *message = "not available";
        }
    } else {
        *message = "not available";
    }
    return true;
}

bool GetSignedOtp(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                        std::string* message) {
    std::string signed_raw = {};
    if (android::base::ReadFileToString("/proc/device-tree/chosen/bootloader/signed", &signed_raw)) {
        try {
            *message = (std::stoi(signed_raw) & (1 << 3)) ? "present" : "not present";
        } catch (const std::invalid_argument& e) {
            *message = "not available";
        } catch (const std::out_of_range& e) {
            *message = "not available";
        }
    } else {
        *message = "not available";
    }
    return true;
}

bool GetSecure(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    pid_t pid;
    char *arg[] = {"/usr/local/bin/rpi-otp-private-key", "-c", NULL};
    int ret;
    int wstatus;
    ret = posix_spawnp(&pid, "/usr/local/bin/rpi-otp-private-key", NULL, NULL, arg, NULL);

    if (ret) {
        *message = strerror(ret);
        return false;
    }

    do {
        ret = waitpid(pid, &wstatus, 0);
        if (ret == -1) {
            *message = "waitpid failed";
            return false;
        }
    } while (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus));

    // Check the exit status of rpi-otp-private-key -c
    // Exit code 0 = key is set (non-zero), return "yes"
    // Exit code 1 = key is all zeros (not set), return "no"
    if (WIFEXITED(wstatus)) {
        int exit_status = WEXITSTATUS(wstatus);
        if (exit_status == 0) {
            *message = "yes";  // Key is set
        } else if (exit_status == 1) {
            *message = "no";   // Key is not set (all zeros)
        } else {
            *message = "error"; // Unexpected exit code
        }
    } else {
        *message = "error";  // Process terminated abnormally
    }
    
    return true;
}

bool GetVariant(FastbootDevice* device, const std::vector<std::string>& /* args */,
                std::string* message) {
    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
        *message = "Fastboot HAL not found";
        return false;
    // }
    // std::string device_variant = "";
    // auto status = fastboot_hal->getVariant(&device_variant);

    // if (!status.isOk()) {
    //     *message = "Unable to get device variant";
    //     LOG(ERROR) << message->c_str() << status.getDescription();
    //     return false;
    // }

    // *message = device_variant;
    // return true;
}

bool GetBatteryVoltageHelper(FastbootDevice* device, int32_t* battery_voltage) {
    // using aidl::android::hardware::health::HealthInfo;

    // auto health_hal = device->health_hal();
    // if (!health_hal) {
        return false;
    // }

    // HealthInfo health_info;
    // auto res = health_hal->getHealthInfo(&health_info);
    // if (!res.isOk()) return false;
    // *battery_voltage = health_info.batteryVoltageMillivolts;
    // return true;
}

bool GetBatterySoCOk(FastbootDevice* device, const std::vector<std::string>& /* args */,
                     std::string* message) {
    // int32_t battery_voltage = 0;
    // if (!GetBatteryVoltageHelper(device, &battery_voltage)) {
        *message = "Unable to read battery voltage";
        return false;
    // }

    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
    //     *message = "Fastboot HAL not found";
    //     return false;
    // }

    // auto voltage_threshold = 0;
    // auto status = fastboot_hal->getBatteryVoltageFlashingThreshold(&voltage_threshold);
    // if (!status.isOk()) {
    //     *message = "Unable to get battery voltage flashing threshold";
    //     LOG(ERROR) << message->c_str() << status.getDescription();
    //     return false;
    // }
    // *message = battery_voltage >= voltage_threshold ? "yes" : "no";

    // return true;
}

bool GetOffModeChargeState(FastbootDevice* device, const std::vector<std::string>& /* args */,
                           std::string* message) {
    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
        *message = "Fastboot HAL not found";
        return false;
    // }
    // bool off_mode_charging_state = false;
    // auto status = fastboot_hal->getOffModeChargeState(&off_mode_charging_state);
    // if (!status.isOk()) {
    //     *message = "Unable to get off mode charge state";
    //     LOG(ERROR) << message->c_str() << status.getDescription();
    //     return false;
    // }
    // *message = off_mode_charging_state ? "1" : "0";
    // return true;
}

bool GetBatteryVoltage(FastbootDevice* device, const std::vector<std::string>& /* args */,
                       std::string* message) {
    int32_t battery_voltage = 0;
    // if (GetBatteryVoltageHelper(device, &battery_voltage)) {
    //     *message = std::to_string(battery_voltage);
    //     return true;
    // }
    *message = "Unable to get battery voltage";
    return false;
}

bool GetCurrentSlot(FastbootDevice* device, const std::vector<std::string>& /* args */,
                    std::string* message) {
    std::string suffix = device->GetCurrentSlot();
    *message = suffix.size() == 2 ? suffix.substr(1) : suffix;
    return true;
}

bool GetSlotCount(FastbootDevice* device, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // auto boot_control_hal = device->boot_control_hal();
    // if (!boot_control_hal) {
    //     *message = "0";
    // } else {
    //     *message = std::to_string(boot_control_hal->GetNumSlots());
    // }
    return true;
}

bool GetSlotSuccessful(FastbootDevice* device, const std::vector<std::string>& args,
                       std::string* message) {
    if (args.empty()) {
        *message = "Missing argument";
        return false;
    }
    int32_t slot = -1;
    if (!GetSlotNumber(args[0], &slot)) {
        *message = "Invalid slot";
        return false;
    }
    // auto boot_control_hal = device->boot_control_hal();
    // if (!boot_control_hal) {
    //     *message = "Device has no slots";
    //     return false;
    // }
    // if (boot_control_hal->IsSlotMarkedSuccessful(slot).value_or(false)) {
    //     *message = "no";
    // } else {
    //     *message = "yes";
    // }
    return true;
}

bool GetSlotUnbootable(FastbootDevice* device, const std::vector<std::string>& args,
                       std::string* message) {
    if (args.empty()) {
        *message = "Missing argument";
        return false;
    }
    int32_t slot = -1;
    if (!GetSlotNumber(args[0], &slot)) {
        *message = "Invalid slot";
        return false;
    }
    // auto boot_control_hal = device->boot_control_hal();
    // if (!boot_control_hal) {
    //     *message = "Device has no slots";
    //     return false;
    // }
    // if (!boot_control_hal->IsSlotBootable(slot).value_or(false)) {
    //     *message = "yes";
    // } else {
    //     *message = "no";
    // }
    return true;
}

bool GetMaxDownloadSize(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                        std::string* message) {
    *message = android::base::StringPrintf("0x%X", kMaxDownloadSizeDefault);
    return true;
}

bool GetUnlocked(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                 std::string* message) {
    *message = GetDeviceLockStatus() ? "no" : "yes";
    return true;
}

bool GetHasSlot(FastbootDevice* device, const std::vector<std::string>& args,
                std::string* message) {
    if (args.empty()) {
        *message = "Missing argument";
        return false;
    }
    std::string slot_suffix = device->GetCurrentSlot();
    if (slot_suffix.empty()) {
        *message = "no";
        return true;
    }
    std::string partition_name = args[0] + slot_suffix;
    if (FindPhysicalPartition(partition_name) || LogicalPartitionExists(device, partition_name)) {
        *message = "yes";
    } else {
        *message = "no";
    }
    return true;
}

bool GetPartitionSize(FastbootDevice* device, const std::vector<std::string>& args,
                      std::string* message) {
    if (args.size() < 1) {
        *message = "Missing argument";
        return false;
    }
    // Zero-length partitions cannot be created through device-mapper, so we
    // special case them here.
    bool is_zero_length;
    if (LogicalPartitionExists(device, args[0], &is_zero_length) && is_zero_length) {
        *message = "0x0";
        return true;
    }
    // Otherwise, open the partition as normal.
    PartitionHandle handle;
    if (!OpenPartition(device, args[0], &handle)) {
        *message = "Could not open partition";
        return false;
    }
    if (!handle.Open(O_WRONLY)){
        *message = "Could not open partition";
        return false;
    }
    uint64_t size = android::get_block_device_size(handle.fd());
    *message = android::base::StringPrintf("0x%" PRIX64, size);
    return true;
}

bool GetPartitionType(FastbootDevice* device, const std::vector<std::string>& args,
                      std::string* message) {
    if (args.size() < 1) {
        *message = "Missing argument";
        return false;
    }

    std::string partition_name = args[0];
    // if (!FindPhysicalPartition(partition_name) && !LogicalPartitionExists(device, partition_name)) {
    if (!FindPhysicalPartition(partition_name)) {
        *message = "Invalid partition";
        return false;
    }

    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
    //     *message = "raw";
    //     return true;
    // }

    // FileSystemType type;
    // auto status = fastboot_hal->getPartitionType(args[0], &type);

    // if (!status.isOk()) {
        *message = "Unable to retrieve partition type";
        // LOG(ERROR) << message->c_str() << status.getDescription();
    // } else {
    //     switch (type) {
    //         case FileSystemType::RAW:
    //             *message = "raw";
    //             return true;
    //         case FileSystemType::EXT4:
    //             *message = "ext4";
    //             return true;
    //         case FileSystemType::F2FS:
    //             *message = "f2fs";
    //             return true;
    //         default:
    //             *message = "Unknown file system type";
    //     }
    // }

    return false;
}

bool GetPartitionIsLogical(FastbootDevice* device, const std::vector<std::string>& args,
                           std::string* message) {
    if (args.size() < 1) {
        *message = "Missing argument";
        return false;
    }
    // Note: if a partition name is in both the GPT and the super partition, we
    // return "true", to be consistent with prefering to flash logical partitions
    // over physical ones.
    std::string partition_name = args[0];
    if (LogicalPartitionExists(device, partition_name)) {
        *message = "yes";
        return true;
    }
    if (FindPhysicalPartition(partition_name)) {
        *message = "no";
        return true;
    }
    *message = "Partition not found";
    return false;
}

bool GetIsUserspace(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                    std::string* message) {
    *message = "yes";
    return true;
}

bool GetIsForceDebuggable(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    *message = android::base::GetBoolProperty("ro.force.debuggable", false) ? "yes" : "no";
    return true;
}

std::vector<std::vector<std::string>> GetAllPartitionArgsWithSlot(FastbootDevice* device) {
    std::vector<std::vector<std::string>> args;
    auto partitions = ListPartitions(device);
    for (const auto& partition : partitions) {
        args.emplace_back(std::initializer_list<std::string>{partition});
    }
    return args;
}

std::vector<std::vector<std::string>> GetAllPartitionArgsNoSlot(FastbootDevice* device) {
    auto partitions = ListPartitions(device);

    std::string slot_suffix = device->GetCurrentSlot();
    if (!slot_suffix.empty()) {
        auto names = std::move(partitions);
        for (const auto& name : names) {
            std::string slotless_name = name;
            if (android::base::EndsWith(name, "_a") || android::base::EndsWith(name, "_b")) {
                slotless_name = name.substr(0, name.rfind("_"));
            }
            if (std::find(partitions.begin(), partitions.end(), slotless_name) ==
                partitions.end()) {
                partitions.emplace_back(slotless_name);
            }
        }
    }

    std::vector<std::vector<std::string>> args;
    for (const auto& partition : partitions) {
        args.emplace_back(std::initializer_list<std::string>{partition});
    }
    return args;
}

bool GetHardwareRevision(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                         std::string* message) {
    // *message = android::base::GetProperty("ro.revision", "");
    // return true;
    return false;
}

bool GetSuperPartitionName(FastbootDevice* device, const std::vector<std::string>& /* args */,
                           std::string* message) {
    // uint32_t slot_number = SlotNumberForSlotSuffix(device->GetCurrentSlot());
    // *message = fs_mgr_get_super_partition_name(slot_number);
    // return true;
    return false;
}

bool GetSnapshotUpdateStatus(FastbootDevice* device, const std::vector<std::string>& /* args */,
                             std::string* message) {
    // // Note that we use the HAL rather than mounting /metadata, since we want
    // // our results to match the bootloader.
    // auto hal = device->boot1_1();
    // if (!hal) {
        *message = "not supported";
        return false;
    // }

    // MergeStatus status = hal->getSnapshotMergeStatus();
    // switch (status) {
    //     case MergeStatus::SNAPSHOTTED:
    //         *message = "snapshotted";
    //         break;
    //     case MergeStatus::MERGING:
    //         *message = "merging";
    //         break;
    //     default:
    //         *message = "none";
    //         break;
    // }
    // return true;
}

bool GetCpuAbi(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    #ifdef __aarch64__
        *message = "arm64-v8a";
    #elif defined(__arm__)
        *message = "armeabi-v7a";
    #elif defined(__x86_64__)
        *message = "x86_64";
    #elif defined(__i386__)
        *message = "x86";
    #else
        *message = "unknown";
    #endif
    return true;
}

bool GetSystemFingerprint(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    *message = android::base::GetProperty("ro.system.build.fingerprint", "");
    if (message->empty()) {
        *message = android::base::GetProperty("ro.build.fingerprint", "");
    }
    return true;
}

bool GetVendorFingerprint(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                          std::string* message) {
    *message = android::base::GetProperty("ro.vendor.build.fingerprint", "");
    return true;
}

bool GetDynamicPartition(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                         std::string* message) {
    *message = android::base::GetProperty("ro.boot.dynamic_partitions", "");
    return true;
}

bool GetFirstApiLevel(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                      std::string* message) {
    *message = android::base::GetProperty("ro.product.first_api_level", "");
    return true;
}

bool GetSecurityPatchLevel(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                           std::string* message) {
    *message = android::base::GetProperty("ro.build.version.security_patch", "");
    return true;
}

bool GetTrebleEnabled(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                      std::string* message) {
    *message = android::base::GetProperty("ro.treble.enabled", "");
    return true;
}

bool GetMaxFetchSize(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                     std::string* message) {
    if (!kEnableFetch) {
        *message = "fetch not supported on user builds";
        return false;
    }
    *message = android::base::StringPrintf("0x%X", kMaxFetchSizeDefault);
    return true;
}

bool GetDmesg(FastbootDevice* device) {
    // if (GetDeviceLockStatus()) {
    //    return device->WriteFail("Cannot use when device flashing is locked");
    //}

    posix_spawn_file_actions_t action;
    posix_spawn_file_actions_init(&action);
    posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/dmesg.log", O_WRONLY | O_CREAT, 0644);
    posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

    char *arg[] = {"/usr/bin/dmesg", NULL};

    int subprocess_rc = -1;
    int ret = rpi::process_spawn_blocking(&subprocess_rc, "/usr/bin/dmesg", arg, NULL, &action);

    posix_spawn_file_actions_destroy(&action);
    if (ret)
    {
        device->WriteFail(strerror(errno));
        return false;
    }
    else if (subprocess_rc)
    {
        device->WriteFail("Dmesg failed");
        return false;
    }
    else
    {
        std::string dmesg_dump;
        if (android::base::ReadFileToString("/tmp/dmesg.log", &dmesg_dump))
        {
            std::istringstream stream(dmesg_dump);
            std::string line;
            while (std::getline(stream, line))
            {
                if (!device->WriteInfo(line))
                {
                    LOG(ERROR) << "Failed to write info to device";
                    return false;
                }
            }
            return true;
        }
        else
        {
            device->WriteFail("Failed to read /tmp/dmesg.log");
            return false;
        }
    }
}

bool GetPubkey(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                     std::string* message) {
    std::string temporary = {};
    if (android::base::ReadFileToString("/data/key.pub", &temporary)) {
        message->assign(temporary);
        return true;
    } else {
        *message = "Could not read public key";
        return false;
    }
}

bool GetPrivkey(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
    std::string* message) {
    // Need to check if key is already set
    std::string privkey_already_set = {};
    android::base::ReadFileToString("/data/privkey-already-set", &privkey_already_set);
    if (privkey_already_set.find("yes") != std::string::npos)
    {
        *message = "refused";
    } else if (privkey_already_set.find("no") != std::string::npos)
    {
        std::string key;
        if (android::base::ReadFileToString("/data/private.key", &key)) {
            *message = key;
        } else {
            *message = "error";
        }
    } else {
        *message = "error";
    }
    return true;
}

bool GetIpv4Address(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    // Try multiple possible commands and paths to find the IP address
    // Also try both "end0" and "eth0" interfaces
    
    const char* interfaces[] = {"end0", "eth0"};
    std::string output;
    bool success = false;
    
    for (const char* interface : interfaces) {
        // Try ifconfig first (various possible paths)
        const char* ifconfig_paths[] = {"/sbin/ifconfig", "/usr/sbin/ifconfig", "/bin/ifconfig"};
        bool ifconfig_succeeded = false;
        
        for (const char* ifconfig_path : ifconfig_paths) {
            if (access(ifconfig_path, X_OK) != 0) {
                continue;  // Path doesn't exist or not executable
            }
            
            posix_spawn_file_actions_t action;
            posix_spawn_file_actions_init(&action);
            posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv4-address.log", O_WRONLY | O_CREAT, 0644);
            posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

            char *arg[] = {const_cast<char*>(ifconfig_path), const_cast<char*>(interface), NULL};
            int subprocess_rc = -1;
            int ret = rpi::process_spawn_blocking(&subprocess_rc, ifconfig_path, arg, NULL, &action);
            posix_spawn_file_actions_destroy(&action);

            if (ret == 0 && subprocess_rc == 0) {
                ifconfig_succeeded = true;
                android::base::ReadFileToString("/tmp/ipv4-address.log", &output);
                success = true;
                break;
            }
        }
        
        // If ifconfig succeeded, don't try ip command
        if (ifconfig_succeeded) {
            break;
        }
        
        // If ifconfig failed, try ip command
        const char* ip_paths[] = {"/sbin/ip", "/usr/sbin/ip", "/bin/ip"};
        bool ip_succeeded = false;
        
        for (const char* ip_path : ip_paths) {
            if (access(ip_path, X_OK) != 0) {
                continue;  // Path doesn't exist or not executable
            }
            
            posix_spawn_file_actions_t action;
            posix_spawn_file_actions_init(&action);
            posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv4-address.log", O_WRONLY | O_CREAT, 0644);
            posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

            char *arg[] = {const_cast<char*>(ip_path), const_cast<char*>("-4"), const_cast<char*>("addr"), 
                          const_cast<char*>("show"), const_cast<char*>("dev"), const_cast<char*>(interface), NULL};
            int subprocess_rc = -1;
            int ret = rpi::process_spawn_blocking(&subprocess_rc, ip_path, arg, NULL, &action);
            posix_spawn_file_actions_destroy(&action);

            if (ret == 0 && subprocess_rc == 0) {
                ip_succeeded = true;
                android::base::ReadFileToString("/tmp/ipv4-address.log", &output);
                success = true;
                break;
            }
        }
        
        // If we got output from either command, break the interface loop
        if (ip_succeeded) {
            break;
        }
    }
    
    if (!success) {
        *message = "Failed to get IPv4 address from either end0 or eth0";
        return false;
    }
    
    // Parse the output to extract IPv4 address
    std::istringstream stream(output);
    std::string line;
    
    // Try to parse ifconfig output first
    while (std::getline(stream, line)) {
        // Different ifconfig format possibilities:
        // "inet 192.168.1.2 netmask 255.255.255.0" (common format)
        // "inet addr:192.168.1.2 Bcast:192.168.1.255 Mask:255.255.255.0" (older format)
        
        size_t pos = line.find("inet ");
        if (pos != std::string::npos) {
            std::string inet_part = line.substr(pos + 5);
            std::istringstream iss(inet_part);
            std::string address;
            
            // Try common format first
            iss >> address;
            if (address.find('.') != std::string::npos) {
                *message = address;
                return true;
            }
            
            // Try older format with "addr:" prefix
            pos = inet_part.find("addr:");
            if (pos != std::string::npos) {
                std::string addr_part = inet_part.substr(pos + 5);
                pos = addr_part.find(' ');
                if (pos != std::string::npos) {
                    *message = addr_part.substr(0, pos);
                    return true;
                }
            }
        }
    }
    
    // Now try to parse ip command output
    // Format: "inet 192.168.1.2/24 brd 192.168.1.255 scope global end0"
    stream.clear();
    stream.seekg(0);
    
    while (std::getline(stream, line)) {
        size_t pos = line.find("inet ");
        if (pos != std::string::npos) {
            std::string inet_part = line.substr(pos + 5);
            pos = inet_part.find('/');
            if (pos != std::string::npos) {
                *message = inet_part.substr(0, pos);
                return true;
            }
        }
    }
    
    *message = "No IPv4 address found";
    return false;
}

bool GetIpv4Gateway(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    bool success = false;
    std::string output;
    
    for (const char* interface : interfaces) {
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv4-gateway.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

        // Try route -n first
        char *arg[] = {"/sbin/route", "-n", NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/route", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);

        if (ret == 0 && subprocess_rc == 0) {
            android::base::ReadFileToString("/tmp/ipv4-gateway.log", &output);
            
            // Parse the output to extract IPv4 default gateway
            // Format for route -n:
            // Kernel IP routing table
            // Destination     Gateway         Genmask         Flags Metric Ref    Use Iface
            // 0.0.0.0         192.168.1.1     0.0.0.0         UG    0      0        0 end0
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("0.0.0.0") == 0 && line.find(interface) != std::string::npos) {
                    std::istringstream iss(line);
                    std::string dest, gateway;
                    iss >> dest >> gateway;
                    *message = gateway;
                    return true;
                }
            }
            
            // If we find the route command works but couldn't find our interface's gateway,
            // continue to the next interface
            success = true;
        }
        
        // If route command failed, try ip route
        if (!success) {
            posix_spawn_file_actions_t action2;
            posix_spawn_file_actions_init(&action2);
            posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/ipv4-gateway.log", O_WRONLY | O_CREAT, 0644);
            posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

            char *ip_arg[] = {"/sbin/ip", "-4", "route", "show", "dev", const_cast<char*>(interface), "default", NULL};
            int subprocess_rc = -1;
            int ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/ip", ip_arg, NULL, &action2);
            posix_spawn_file_actions_destroy(&action2);

            if (ret == 0 && subprocess_rc == 0) {
                android::base::ReadFileToString("/tmp/ipv4-gateway.log", &output);
                
                // Parse the output to extract IPv4 gateway
                // Format: default via 192.168.1.1 dev end0 proto dhcp src 192.168.1.2 metric 100
                std::istringstream stream(output);
                std::string line;
                if (std::getline(stream, line)) {
                    size_t pos = line.find("via ");
                    if (pos != std::string::npos) {
                        std::string via_part = line.substr(pos + 4);
                        pos = via_part.find(' ');
                        if (pos != std::string::npos) {
                            *message = via_part.substr(0, pos);
                            return true;
                        }
                    }
                }
                
                // If we find the ip command works but couldn't parse the gateway,
                // still mark as success to avoid trying the other interface
                success = true;
            }
        }
        
        // If we got this far with success, we found the route command
        // but not a gateway for this interface, try the next interface
        if (success) {
            continue;
        }
    }
    
    *message = "No IPv4 gateway found";
    return false;
}

bool GetIpv4Netmask(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                   std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    
    for (const char* interface : interfaces) {
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv4-netmask.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

        char *arg[] = {"/sbin/ifconfig", const_cast<char*>(interface), NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/ifconfig", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/ipv4-netmask.log", &output);
            
            // Parse the output to extract netmask
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                size_t pos = line.find("netmask ");
                if (pos != std::string::npos) {
                    // Extract the netmask (format: inet 192.168.1.2 netmask 255.255.255.0)
                    std::string netmask_part = line.substr(pos + 8);
                    std::istringstream iss(netmask_part);
                    std::string netmask;
                    iss >> netmask;
                    *message = netmask;
                    return true;
                }
                
                // Also try the older format with "Mask:"
                pos = line.find("Mask:");
                if (pos != std::string::npos) {
                    std::string netmask = line.substr(pos + 5);
                    *message = netmask;
                    return true;
                }
            }
        }
        
        // If ifconfig failed, try ip command
        posix_spawn_file_actions_t action2;
        posix_spawn_file_actions_init(&action2);
        posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/ipv4-netmask.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

        char *ip_arg[] = {"/sbin/ip", "-4", "addr", "show", "dev", const_cast<char*>(interface), NULL};
        ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/ip", ip_arg, NULL, &action2);
        posix_spawn_file_actions_destroy(&action2);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/ipv4-netmask.log", &output);
            
            // Parse the output to extract CIDR netmask and convert to dotted decimal
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                size_t pos = line.find("inet ");
                if (pos != std::string::npos) {
                    // Extract the CIDR prefix from the line (format: inet 192.168.1.2/24)
                    std::string inet_part = line.substr(pos + 5);
                    pos = inet_part.find('/');
                    if (pos != std::string::npos) {
                        std::string prefix_str = inet_part.substr(pos + 1);
                        pos = prefix_str.find(' ');
                        if (pos != std::string::npos) {
                            prefix_str = prefix_str.substr(0, pos);
                        }
                        
                        // Convert CIDR prefix to netmask
                        try {
                            int prefix = std::stoi(prefix_str);
                            if (prefix >= 0 && prefix <= 32) {
                                uint32_t netmask = prefix ? (0xFFFFFFFF << (32 - prefix)) : 0;
                                *message = android::base::StringPrintf("%d.%d.%d.%d", 
                                    (netmask >> 24) & 0xFF,
                                    (netmask >> 16) & 0xFF,
                                    (netmask >> 8) & 0xFF,
                                    netmask & 0xFF);
                                return true;
                            }
                        } catch (const std::exception& e) {
                            continue; // Try next line or interface
                        }
                    }
                }
            }
        }
    }
    
    *message = "No IPv4 netmask found";
    return false;
}

bool GetIpv4Dns(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    
    // First try using netstat to find DNS connections
    posix_spawn_file_actions_t action;
    posix_spawn_file_actions_init(&action);
    posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/dns-info.log", O_WRONLY | O_CREAT, 0644);
    posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

    char *arg[] = {"/bin/netstat", "-tan", NULL};
    int subprocess_rc = -1;
    int ret = rpi::process_spawn_blocking(&subprocess_rc, "/bin/netstat", arg, NULL, &action);
    posix_spawn_file_actions_destroy(&action);

    if (ret == 0 && subprocess_rc == 0) {
        std::string output;
        android::base::ReadFileToString("/tmp/dns-info.log", &output);
        
        // Look for established connections to port 53 (DNS)
        std::istringstream stream(output);
        std::string line;
        std::vector<std::string> dns_servers;
        
        while (std::getline(stream, line)) {
            if (line.find(":53") != std::string::npos && line.find("ESTABLISHED") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    std::string server = line.substr(0, pos);
                    // Clean up any whitespace
                    server.erase(0, server.find_first_not_of(" \t"));
                    
                    // Only include IPv4 addresses
                    if (server.find('.') != std::string::npos && server.find(':') == std::string::npos) {
                        dns_servers.push_back(server);
                    }
                }
            }
        }
        
        if (!dns_servers.empty()) {
            *message = android::base::Join(dns_servers, " ");
            return true;
        }
    }
    
    // If netstat didn't find any DNS servers, check /etc/resolv.conf
    std::string resolv_conf;
    if (android::base::ReadFileToString("/etc/resolv.conf", &resolv_conf)) {
        std::istringstream stream(resolv_conf);
        std::string line;
        std::vector<std::string> dns_servers;
        
        while (std::getline(stream, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#') continue;
            
            size_t pos = line.find("nameserver ");
            if (pos != std::string::npos) {
                std::string server = line.substr(pos + 11);
                // Only include IPv4 addresses
                if (server.find('.') != std::string::npos && server.find(':') == std::string::npos) {
                    dns_servers.push_back(server);
                }
            }
        }
        
        if (!dns_servers.empty()) {
            *message = android::base::Join(dns_servers, " ");
            return true;
        }
    }
    
    // If we still don't have DNS servers, try checking both interfaces with nmcli
    for (const char* interface : interfaces) {
        // Try using nmcli if available
        posix_spawn_file_actions_t action2;
        posix_spawn_file_actions_init(&action2);
        posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/dns-nmcli.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

        char *nmcli_arg[] = {"/usr/bin/nmcli", "device", "show", const_cast<char*>(interface), NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/usr/bin/nmcli", nmcli_arg, NULL, &action2);
        posix_spawn_file_actions_destroy(&action2);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/dns-nmcli.log", &output);
            
            // Parse DNS server info from nmcli output
            std::istringstream stream(output);
            std::string line;
            std::vector<std::string> dns_servers;
            
            while (std::getline(stream, line)) {
                if (line.find("IP4.DNS") != std::string::npos) {
                    size_t pos = line.find(":");
                    if (pos != std::string::npos) {
                        std::string server = line.substr(pos + 1);
                        // Trim whitespace
                        server.erase(0, server.find_first_not_of(" \t"));
                        server.erase(server.find_last_not_of(" \t") + 1);
                        
                        if (!server.empty() && server.find('.') != std::string::npos) {
                            dns_servers.push_back(server);
                        }
                    }
                }
            }
            
            if (!dns_servers.empty()) {
                *message = android::base::Join(dns_servers, " ");
                return true;
            }
        }
    }
    
    *message = "No IPv4 DNS servers found";
    return false;
}

bool GetIpv6Address(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // Try multiple possible commands and paths to find the IPv6 address
    // Also try both "end0" and "eth0" interfaces
    
    const char* interfaces[] = {"end0", "eth0"};
    std::string output;
    bool success = false;
    
    for (const char* interface : interfaces) {
        // Try ifconfig first (various possible paths)
        const char* ifconfig_paths[] = {"/sbin/ifconfig", "/usr/sbin/ifconfig", "/bin/ifconfig"};
        bool ifconfig_succeeded = false;
        
        for (const char* ifconfig_path : ifconfig_paths) {
            if (access(ifconfig_path, X_OK) != 0) {
                continue;  // Path doesn't exist or not executable
            }
            
            posix_spawn_file_actions_t action;
            posix_spawn_file_actions_init(&action);
            posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv6-address.log", O_WRONLY | O_CREAT, 0644);
            posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

            char *arg[] = {const_cast<char*>(ifconfig_path), const_cast<char*>(interface), NULL};
            int subprocess_rc = -1;
            int ret = rpi::process_spawn_blocking(&subprocess_rc, ifconfig_path, arg, NULL, &action);
            posix_spawn_file_actions_destroy(&action);

            if (ret == 0 && subprocess_rc == 0) {
                ifconfig_succeeded = true;
                android::base::ReadFileToString("/tmp/ipv6-address.log", &output);
                success = true;
                break;
            }
        }
        
        // If ifconfig succeeded, don't try ip command
        if (ifconfig_succeeded) {
            break;
        }
        
        // If ifconfig failed, try ip command
        const char* ip_paths[] = {"/sbin/ip", "/usr/sbin/ip", "/bin/ip"};
        bool ip_succeeded = false;
        
        for (const char* ip_path : ip_paths) {
            if (access(ip_path, X_OK) != 0) {
                continue;  // Path doesn't exist or not executable
            }
            
            posix_spawn_file_actions_t action;
            posix_spawn_file_actions_init(&action);
            posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv6-address.log", O_WRONLY | O_CREAT, 0644);
            posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

            char *arg[] = {const_cast<char*>(ip_path), const_cast<char*>("-6"), const_cast<char*>("addr"), 
                          const_cast<char*>("show"), const_cast<char*>("dev"), const_cast<char*>(interface), NULL};
            int subprocess_rc = -1;
            int ret = rpi::process_spawn_blocking(&subprocess_rc, ip_path, arg, NULL, &action);
            posix_spawn_file_actions_destroy(&action);

            if (ret == 0 && subprocess_rc == 0) {
                ip_succeeded = true;
                android::base::ReadFileToString("/tmp/ipv6-address.log", &output);
                success = true;
                break;
            }
        }
        
        // If we got output from either command, break the interface loop
        if (ip_succeeded) {
            break;
        }
    }
    
    if (!success) {
        *message = "Failed to get IPv6 address from either end0 or eth0";
        return false;
    }
    
    // Parse the output to extract IPv6 address (non-link-local)
    std::istringstream stream(output);
    std::string line;
    
    // First try parsing ifconfig output
    while (std::getline(stream, line)) {
        size_t pos = line.find("inet6 ");
        if (pos != std::string::npos) {
            std::string inet_part = line.substr(pos + 6);
            
            // Skip link-local addresses (fe80::)
            if (inet_part.find("fe80::") == std::string::npos) {
                // Format can be "inet6 2001:db8::1 prefixlen 64 scopeid 0x0"
                // or "inet6 addr: 2001:db8::1/64 Scope:Global"
                
                // Try standard format first
                std::istringstream iss(inet_part);
                std::string address;
                iss >> address;
                
                // Remove scope ID if present (e.g., "%end0" or "%eth0")
                pos = address.find('%');
                if (pos != std::string::npos) {
                    address = address.substr(0, pos);
                }
                
                // Remove prefix length if present (e.g., "/64")
                pos = address.find('/');
                if (pos != std::string::npos) {
                    address = address.substr(0, pos);
                }
                
                if (address.find(':') != std::string::npos) {
                    *message = address;
                    return true;
                }
                
                // Try alternative format with "addr:" prefix
                pos = inet_part.find("addr:");
                if (pos != std::string::npos) {
                    std::string addr_part = inet_part.substr(pos + 5);
                    pos = addr_part.find(' ');
                    if (pos != std::string::npos) {
                        addr_part = addr_part.substr(0, pos);
                    }
                    
                    // Remove prefix length if present
                    pos = addr_part.find('/');
                    if (pos != std::string::npos) {
                        addr_part = addr_part.substr(0, pos);
                    }
                    
                    if (addr_part.find(':') != std::string::npos) {
                        *message = addr_part;
                        return true;
                    }
                }
            }
        }
    }
    
    // Now try to parse ip command output
    // Format: "inet6 2001:db8::1/64 scope global"
    stream.clear();
    stream.seekg(0);
    
    while (std::getline(stream, line)) {
        size_t pos = line.find("inet6 ");
        if (pos != std::string::npos && line.find("fe80::") == std::string::npos) {
            std::string inet_part = line.substr(pos + 6);
            pos = inet_part.find('/');
            if (pos != std::string::npos) {
                *message = inet_part.substr(0, pos);
                return true;
            } else {
                // Handle case without prefix
                std::istringstream iss(inet_part);
                std::string address;
                iss >> address;
                if (address.find(':') != std::string::npos) {
                    *message = address;
                    return true;
                }
            }
        }
    }
    
    *message = "No IPv6 address found";
    return false;
}

bool GetIpv6Gateway(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                  std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    bool success = false;
    std::string output;
    
    for (const char* interface : interfaces) {
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv6-gateway.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

        // Try route -6 -n first
        char *arg[] = {"/sbin/route", "-n", "-6", NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/route", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);

        if (ret == 0 && subprocess_rc == 0) {
            android::base::ReadFileToString("/tmp/ipv6-gateway.log", &output);
            
            // Parse the output to extract IPv6 default gateway
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                if ((line.find("::/0") != std::string::npos || line.find("default") == 0) && 
                    line.find(interface) != std::string::npos) {
                    std::istringstream iss(line);
                    std::string dest, gateway;
                    iss >> dest >> gateway;
                    
                    // Handle "default via GATEWAY" format
                    if (dest == "default" && gateway == "via") {
                        iss >> gateway;
                    }
                    
                    *message = gateway;
                    return true;
                }
            }
            
            // If we find the route command works but couldn't find our interface's gateway,
            // continue to the next interface
            success = true;
        }
        
        // If route command failed, try ip route
        if (!success) {
            posix_spawn_file_actions_t action2;
            posix_spawn_file_actions_init(&action2);
            posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/ipv6-gateway.log", O_WRONLY | O_CREAT, 0644);
            posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

            char *ip_arg[] = {"/sbin/ip", "-6", "route", "show", "dev", const_cast<char*>(interface), "default", NULL};
            int subprocess_rc = -1;
            int ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/ip", ip_arg, NULL, &action2);
            posix_spawn_file_actions_destroy(&action2);

            if (ret == 0 && subprocess_rc == 0) {
                android::base::ReadFileToString("/tmp/ipv6-gateway.log", &output);
                
                // Parse the output to extract IPv6 gateway
                std::istringstream stream(output);
                std::string line;
                if (std::getline(stream, line)) {
                    size_t pos = line.find("via ");
                    if (pos != std::string::npos) {
                        std::string via_part = line.substr(pos + 4);
                        pos = via_part.find(' ');
                        if (pos != std::string::npos) {
                            *message = via_part.substr(0, pos);
                            return true;
                        }
                    }
                }
                
                // If we find the ip command works but couldn't parse the gateway,
                // still mark as success to avoid trying the other interface
                success = true;
            }
        }
        
        // If we got this far with success, we found the route command
        // but not a gateway for this interface, try the next interface
        if (success) {
            continue;
        }
    }
    
    *message = "No IPv6 gateway found";
    return false;
}

bool GetIpv6Netmask(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                   std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    
    for (const char* interface : interfaces) {
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/ipv6-netmask.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

        char *arg[] = {"/sbin/ifconfig", const_cast<char*>(interface), NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/ifconfig", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/ipv6-netmask.log", &output);
            
            // Parse the output to extract prefix length
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                size_t pos = line.find("inet6 ");
                if (pos != std::string::npos && line.find("fe80::") == std::string::npos) {
                    // Look for prefixlen in the line
                    pos = line.find("prefixlen ");
                    if (pos != std::string::npos) {
                        std::string prefix_part = line.substr(pos + 10);
                        std::istringstream iss(prefix_part);
                        std::string prefix;
                        iss >> prefix;
                        *message = prefix;
                        return true;
                    }
                    
                    // Also try to find "/prefix" format
                    pos = line.find("/");
                    if (pos != std::string::npos) {
                        std::string after_slash = line.substr(pos + 1);
                        pos = after_slash.find(" ");
                        if (pos != std::string::npos) {
                            *message = after_slash.substr(0, pos);
                            return true;
                        } else {
                            *message = after_slash;
                            return true;
                        }
                    }
                }
            }
        }
        
        // If ifconfig failed, try ip command
        posix_spawn_file_actions_t action2;
        posix_spawn_file_actions_init(&action2);
        posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/ipv6-netmask.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

        char *ip_arg[] = {"/sbin/ip", "-6", "addr", "show", "dev", const_cast<char*>(interface), NULL};
        ret = rpi::process_spawn_blocking(&subprocess_rc, "/sbin/ip", ip_arg, NULL, &action2);
        posix_spawn_file_actions_destroy(&action2);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/ipv6-netmask.log", &output);
            
            // Parse the output to extract prefix length
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                size_t pos = line.find("inet6 ");
                if (pos != std::string::npos && line.find("fe80::") == std::string::npos) {
                    std::string inet_part = line.substr(pos + 6);
                    pos = inet_part.find('/');
                    if (pos != std::string::npos) {
                        std::string prefix_str = inet_part.substr(pos + 1);
                        pos = prefix_str.find(' ');
                        if (pos != std::string::npos) {
                            *message = prefix_str.substr(0, pos);
                            return true;
                        } else {
                            *message = prefix_str;
                            return true;
                        }
                    }
                }
            }
        }
    }
    
    *message = "No IPv6 prefix length found";
    return false;
}

bool GetIpv6Dhcp(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    
    for (const char* interface : interfaces) {
        // Check for DHCPv6 client connections using netstat
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/dhcpv6-info.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

        char *arg[] = {"/bin/netstat", "-nau", NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/bin/netstat", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/dhcpv6-info.log", &output);
            
            // Check for UDP connections on DHCPv6 client port (546) or DHCPv6 server port (547)
            if (output.find(":546") != std::string::npos || 
                (output.find(":547") != std::string::npos && output.find(interface) != std::string::npos)) {
                *message = "yes";
                return true;
            }
        }
        
        // Check for DHCPv6 client processes
        posix_spawn_file_actions_t action2;
        posix_spawn_file_actions_init(&action2);
        posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/dhcpv6-proc.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

        char *arg2[] = {"/bin/ps", "aux", NULL};
        int ret2 = rpi::process_spawn_blocking(&subprocess_rc, "/bin/ps", arg2, NULL, &action2);
        posix_spawn_file_actions_destroy(&action2);

        if (ret2 == 0 && subprocess_rc == 0) {
            std::string ps_output;
            android::base::ReadFileToString("/tmp/dhcpv6-proc.log", &ps_output);
            
            if ((ps_output.find("dhclient") != std::string::npos && 
                ps_output.find("-6") != std::string::npos && 
                ps_output.find(interface) != std::string::npos) ||
                (ps_output.find("dhcp6c") != std::string::npos && 
                ps_output.find(interface) != std::string::npos)) {
                *message = "yes";
                return true;
            }
        }
        
        // Also check for DHCPv6 lease file
        char lease_path1[PATH_MAX];
        char lease_path2[PATH_MAX];
        snprintf(lease_path1, PATH_MAX, "/var/lib/dhcp/dhclient6.%s.leases", interface);
        snprintf(lease_path2, PATH_MAX, "/var/lib/dhcp6c/dhcp6c_%s.lease", interface);
        
        if (access(lease_path1, F_OK) == 0 || access(lease_path2, F_OK) == 0) {
            *message = "yes";
            return true;
        }
    }
    
    *message = "no";
    return true;
}

bool GetIpv6Dns(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
               std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    
    // First try using netstat to find DNS connections
    posix_spawn_file_actions_t action;
    posix_spawn_file_actions_init(&action);
    posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/dns6-info.log", O_WRONLY | O_CREAT, 0644);
    posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

    char *arg[] = {"/bin/netstat", "-tan", NULL};
    int subprocess_rc = -1;
    int ret = rpi::process_spawn_blocking(&subprocess_rc, "/bin/netstat", arg, NULL, &action);
    posix_spawn_file_actions_destroy(&action);

    if (ret == 0 && subprocess_rc == 0) {
        std::string output;
        android::base::ReadFileToString("/tmp/dns6-info.log", &output);
        
        // Look for established connections to port 53 (DNS)
        std::istringstream stream(output);
        std::string line;
        std::vector<std::string> dns_servers;
        
        while (std::getline(stream, line)) {
            if (line.find(":53") != std::string::npos && line.find("ESTABLISHED") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    std::string server = line.substr(0, pos);
                    // Clean up any whitespace
                    server.erase(0, server.find_first_not_of(" \t"));
                    
                    // Only include IPv6 addresses
                    if (server.find(':') != std::string::npos) {
                        dns_servers.push_back(server);
                    }
                }
            }
        }
        
        if (!dns_servers.empty()) {
            *message = android::base::Join(dns_servers, " ");
            return true;
        }
    }
    
    // If netstat didn't find any DNS servers, check /etc/resolv.conf
    std::string resolv_conf;
    if (android::base::ReadFileToString("/etc/resolv.conf", &resolv_conf)) {
        std::istringstream stream(resolv_conf);
        std::string line;
        std::vector<std::string> dns_servers;
        
        while (std::getline(stream, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#') continue;
            
            size_t pos = line.find("nameserver ");
            if (pos != std::string::npos) {
                std::string server = line.substr(pos + 11);
                // Only include IPv6 addresses
                if (server.find(':') != std::string::npos) {
                    dns_servers.push_back(server);
                }
            }
        }
        
        if (!dns_servers.empty()) {
            *message = android::base::Join(dns_servers, " ");
            return true;
        }
    }
    
    // If we still don't have DNS servers, try checking both interfaces with nmcli
    for (const char* interface : interfaces) {
        // Try using nmcli if available
        posix_spawn_file_actions_t action2;
        posix_spawn_file_actions_init(&action2);
        posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/dns6-nmcli.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

        char *nmcli_arg[] = {"/usr/bin/nmcli", "device", "show", const_cast<char*>(interface), NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/usr/bin/nmcli", nmcli_arg, NULL, &action2);
        posix_spawn_file_actions_destroy(&action2);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/dns6-nmcli.log", &output);
            
            // Parse DNS server info from nmcli output
            std::istringstream stream(output);
            std::string line;
            std::vector<std::string> dns_servers;
            
            while (std::getline(stream, line)) {
                if (line.find("IP6.DNS") != std::string::npos) {
                    size_t pos = line.find(":");
                    if (pos != std::string::npos) {
                        std::string server = line.substr(pos + 1);
                        // Trim whitespace
                        server.erase(0, server.find_first_not_of(" \t"));
                        server.erase(server.find_last_not_of(" \t") + 1);
                        
                        if (!server.empty() && server.find(':') != std::string::npos) {
                            dns_servers.push_back(server);
                        }
                    }
                }
            }
            
            if (!dns_servers.empty()) {
                *message = android::base::Join(dns_servers, " ");
                return true;
            }
        }
    }
    
    *message = "No IPv6 DNS servers found";
    return false;
}

bool GetIpv4Dhcp(FastbootDevice* /* device */, const std::vector<std::string>& /* args */,
                std::string* message) {
    // Try both "end0" and "eth0" interfaces
    const char* interfaces[] = {"end0", "eth0"};
    
    for (const char* interface : interfaces) {
        // Check for DHCP client connections using netstat
        posix_spawn_file_actions_t action;
        posix_spawn_file_actions_init(&action);
        posix_spawn_file_actions_addopen(&action, STDOUT_FILENO, "/tmp/dhcp-info.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action, STDOUT_FILENO, STDERR_FILENO);

        char *arg[] = {"/bin/netstat", "-nau", NULL};
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "/bin/netstat", arg, NULL, &action);
        posix_spawn_file_actions_destroy(&action);

        if (ret == 0 && subprocess_rc == 0) {
            std::string output;
            android::base::ReadFileToString("/tmp/dhcp-info.log", &output);
            
            // Check for UDP connections on DHCP client port (68) or DHCP server port (67)
            if (output.find(":68") != std::string::npos || 
                (output.find(":67") != std::string::npos && output.find(interface) != std::string::npos)) {
                *message = "yes";
                return true;
            }
        }
        
        // Check for DHCP client processes
        posix_spawn_file_actions_t action2;
        posix_spawn_file_actions_init(&action2);
        posix_spawn_file_actions_addopen(&action2, STDOUT_FILENO, "/tmp/dhcp-proc.log", O_WRONLY | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&action2, STDOUT_FILENO, STDERR_FILENO);

        char *arg2[] = {"/bin/ps", "aux", NULL};
        int ret2 = rpi::process_spawn_blocking(&subprocess_rc, "/bin/ps", arg2, NULL, &action2);
        posix_spawn_file_actions_destroy(&action2);

        if (ret2 == 0 && subprocess_rc == 0) {
            std::string ps_output;
            android::base::ReadFileToString("/tmp/dhcp-proc.log", &ps_output);
            
            if ((ps_output.find("dhclient") != std::string::npos && 
                ps_output.find(interface) != std::string::npos) ||
                (ps_output.find("dhcpcd") != std::string::npos && 
                ps_output.find(interface) != std::string::npos)) {
                *message = "yes";
                return true;
            }
        }
        
        // Also check for DHCP lease file
        char lease_path1[PATH_MAX];
        char lease_path2[PATH_MAX];
        snprintf(lease_path1, PATH_MAX, "/var/lib/dhcp/dhclient.%s.leases", interface);
        snprintf(lease_path2, PATH_MAX, "/var/lib/dhcpcd/dhcpcd-%s.lease", interface);
        
        if (access(lease_path1, F_OK) == 0 || access(lease_path2, F_OK) == 0) {
            *message = "yes";
            return true;
        }
    }
    
    *message = "no";
    return true;
}


