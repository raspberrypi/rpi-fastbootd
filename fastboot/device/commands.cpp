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

#include "commands.h"

#include <inttypes.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <algorithm>
#include <unordered_set>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

// GPIO control via libgpiod (conditional compilation)
#ifdef HAVE_LIBGPIOD
#include <gpiod.h>
#endif

// Crypto operations via libcryptsetup (conditional compilation)
#ifdef HAVE_LIBCRYPTSETUP
#include "crypto_native.h"
#endif
// #include <android/hardware/boot/1.1/IBootControl.h>
// #include <cutils/android_reboot.h>
// #include <ext4_utils/wipe.h>
// #include <fs_mgr.h>
// #include <fs_mgr/roots.h>
// #include <libgsi/libgsi.h>
// #include <liblp/builder.h>
// #include <liblp/liblp.h>
// #include <libsnapshot/snapshot.h>
//#include <storage_literals/storage_literals.h>
#include <uuid/uuid.h>

// #include <bootloader_message/bootloader_message.h>

// #include "BootControlClient.h"
#include "constants.h"
#include "fastboot_device.h"
#include "flashing.h"
#include "utility.h"
#include "storage_literals.h"
#include "rpiparted.h"
#include "idpdevice.h"

// librpifwcrypto
#include <rpifwcrypto.h>

#include <openssl/evp.h>

#include <iostream>
#include <fstream>
#include <cerrno>

#ifdef FB_ENABLE_FETCH
static constexpr bool kEnableFetch = true;
#else
static constexpr bool kEnableFetch = false;
#endif

#define KEYFILE "/data/fastboot.key"

// using android::fs_mgr::MetadataBuilder;
// using android::hal::CommandResult;
// using ::android::hardware::hidl_string;
// using android::snapshot::SnapshotManager;
// using MergeStatus = android::hal::BootControlClient::MergeStatus;

using namespace android::storage_literals;

struct VariableHandlers {
    // Callback to retrieve the value of a single variable.
    std::function<bool(FastbootDevice*, const std::vector<std::string>&, std::string*)> get;
    // Callback to retrieve all possible argument combinations, for getvar all.
    std::function<std::vector<std::vector<std::string>>(FastbootDevice*)> get_all_args;
};

static bool IsSnapshotUpdateInProgress(FastbootDevice* device) {
    // auto hal = device->boot1_1();
    // if (!hal) {
        return false;
    // }
    // auto merge_status = hal->getSnapshotMergeStatus();
    // return merge_status == MergeStatus::SNAPSHOTTED || merge_status == MergeStatus::MERGING;
}

static bool IsProtectedPartitionDuringMerge(FastbootDevice* device, const std::string& name) {
    static const std::unordered_set<std::string> ProtectedPartitionsDuringMerge = {
            "userdata", "metadata", "misc"};
    if (ProtectedPartitionsDuringMerge.count(name) == 0) {
        return false;
    }
    return IsSnapshotUpdateInProgress(device);
}

static void GetAllVars(FastbootDevice* device, const std::string& name,
                       const VariableHandlers& handlers) {
    if (!handlers.get_all_args) {
        std::string message;
        if (!handlers.get(device, std::vector<std::string>(), &message)) {
            return;
        }
        device->WriteInfo(android::base::StringPrintf("%s:%s", name.c_str(), message.c_str()));
        return;
    }

    auto all_args = handlers.get_all_args(device);
    for (const auto& args : all_args) {
        std::string message;
        if (!handlers.get(device, args, &message)) {
            continue;
        }
        std::string arg_string = android::base::Join(args, ":");
        device->WriteInfo(android::base::StringPrintf("%s:%s:%s", name.c_str(), arg_string.c_str(),
                                                      message.c_str()));
    }
}

const std::unordered_map<std::string, VariableHandlers> kVariableMap = {
        {FB_VAR_VERSION, {GetVersion, nullptr}},
        {FB_VAR_VERSION_BOOTLOADER, {GetBootloaderVersion, nullptr}},
        // {FB_VAR_VERSION_BASEBAND, {GetBasebandVersion, nullptr}},
        // {FB_VAR_VERSION_OS, {GetOsVersion, nullptr}},
        // {FB_VAR_VERSION_VNDK, {GetVndkVersion, nullptr}},
        {FB_VAR_PRODUCT, {GetProduct, nullptr}},
        {FB_VAR_SERIALNO, {GetSerial, nullptr}},
        // {FB_VAR_VARIANT, {GetVariant, nullptr}},
        {FB_VAR_SECURE, {GetSecure, nullptr}},
        // {FB_VAR_UNLOCKED, {GetUnlocked, nullptr}},
        {FB_VAR_MAX_DOWNLOAD_SIZE, {GetMaxDownloadSize, nullptr}},
        // {FB_VAR_CURRENT_SLOT, {::GetCurrentSlot, nullptr}},
        // {FB_VAR_SLOT_COUNT, {GetSlotCount, nullptr}},
        // {FB_VAR_HAS_SLOT, {GetHasSlot, GetAllPartitionArgsNoSlot}},
        // {FB_VAR_SLOT_SUCCESSFUL, {GetSlotSuccessful, nullptr}},
        // {FB_VAR_SLOT_UNBOOTABLE, {GetSlotUnbootable, nullptr}},
        {FB_VAR_PARTITION_SIZE, {GetPartitionSize, GetAllPartitionArgsWithSlot}},
        {FB_VAR_PARTITION_TYPE, {GetPartitionType, GetAllPartitionArgsWithSlot}},
        {FB_VAR_IS_LOGICAL, {GetPartitionIsLogical, GetAllPartitionArgsWithSlot}},
        {FB_VAR_IS_USERSPACE, {GetIsUserspace, nullptr}},
        // {FB_VAR_IS_FORCE_DEBUGGABLE, {GetIsForceDebuggable, nullptr}},
        // {FB_VAR_OFF_MODE_CHARGE_STATE, {GetOffModeChargeState, nullptr}},
        // {FB_VAR_BATTERY_VOLTAGE, {GetBatteryVoltage, nullptr}},
        // {FB_VAR_BATTERY_SOC_OK, {GetBatterySoCOk, nullptr}},
        // {FB_VAR_HW_REVISION, {GetHardwareRevision, nullptr}},
        // {FB_VAR_SUPER_PARTITION_NAME, {GetSuperPartitionName, nullptr}},
        // {FB_VAR_SNAPSHOT_UPDATE_STATUS, {GetSnapshotUpdateStatus, nullptr}},
        {FB_VAR_CPU_ABI, {GetCpuAbi, nullptr}},
        // {FB_VAR_SYSTEM_FINGERPRINT, {GetSystemFingerprint, nullptr}},
        // {FB_VAR_VENDOR_FINGERPRINT, {GetVendorFingerprint, nullptr}},
        // {FB_VAR_DYNAMIC_PARTITION, {GetDynamicPartition, nullptr}},
        // {FB_VAR_FIRST_API_LEVEL, {GetFirstApiLevel, nullptr}},
        // {FB_VAR_SECURITY_PATCH_LEVEL, {GetSecurityPatchLevel, nullptr}},
        // {FB_VAR_TREBLE_ENABLED, {GetTrebleEnabled, nullptr}},
        {FB_VAR_MAX_FETCH_SIZE, {GetMaxFetchSize, nullptr}},
        {FB_VAR_PUBKEY, {GetPubkey, nullptr}},
        {FB_VAR_REV_MANUFACTURER, {GetRevisionManufacturer, nullptr}},
        {FB_VAR_REV_MEMORY, {GetRevisionMemory, nullptr}},
        {FB_VAR_REV_PROCESSOR, {GetRevisionProcessor, nullptr}},
        {FB_VAR_REV_TYPE, {GetRevisionType, nullptr}},
        {FB_VAR_REV_REVISION, {GetRevisionRevision, nullptr}},
        {FB_VAR_MMC_SECTOR_SIZE, {GetMmcSectorSize, nullptr}},
        {FB_VAR_MMC_SECTOR_COUNT, {GetMmcSectorCount, nullptr}},
        {FB_VAR_MMC_CID, {GetMmcCid, nullptr}},
        {FB_VAR_MAC_ETHERNET, {GetMacEthernet, nullptr}},
        {FB_VAR_MAC_WIFI, {GetMacWifi, nullptr}},
        {FB_VAR_MAC_BT, {GetMacBt, nullptr}},
        {FB_VAR_RPI_DUID, {GetRpiDuid, nullptr}},
        {FB_VAR_SIGNED_EEPROM, {GetSignedEeprom, nullptr}},
        {FB_VAR_SIGNED_OTP, {GetSignedOtp, nullptr}},
        {FB_VAR_SIGNED_DEVKEY, {GetSignedDevkey, nullptr}},
        {FB_VAR_PRIVKEY, {GetPrivkey, nullptr}},
        {FB_VAR_IPV4_ADDRESS, {GetIpv4Address, nullptr}},
        {FB_VAR_IPV4_GATEWAY, {GetIpv4Gateway, nullptr}},
        {FB_VAR_IPV4_NETMASK, {GetIpv4Netmask, nullptr}},
        {FB_VAR_IPV4_DNS, {GetIpv4Dns, nullptr}},
        {FB_VAR_IPV4_DHCP, {GetIpv4Dhcp, nullptr}},
        {FB_VAR_IPV6_ADDRESS, {GetIpv6Address, nullptr}},
        {FB_VAR_IPV6_GATEWAY, {GetIpv6Gateway, nullptr}},
        {FB_VAR_IPV6_NETMASK, {GetIpv6Netmask, nullptr}},
        {FB_VAR_IPV6_DNS, {GetIpv6Dns, nullptr}},
        {FB_VAR_IPV6_DHCP, {GetIpv6Dhcp, nullptr}},
        
};

static bool GetVarAll(FastbootDevice* device) {
    for (const auto& [name, handlers] : kVariableMap) {
        GetAllVars(device, name, handlers);
    }
    return true;
}

// Helper to read a file and return its contents as a string
static std::string readSysFile(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Helper to check if a path exists
static bool pathExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

static std::string getDeviceId(const std::string& block_device) {
    std::string device = block_device;
    if (device.find("/dev/") == 0) {
        device = device.substr(5);
    }
    
    // Extract base device name (remove partition)
    std::string base_device = device;
    if (device.find("mmcblk") != std::string::npos) {
        size_t p_pos = device.find("p");
        if (p_pos != std::string::npos) {
            base_device = device.substr(0, p_pos);
        }
    } else if (device.find("nvme") != std::string::npos) {
        size_t p_pos = device.find_last_of("p");
        if (p_pos != std::string::npos) {
            base_device = device.substr(0, p_pos);
        }
    } else {
        // SATA/SCSI: sda1 -> sda
        while (!base_device.empty() && std::isdigit(base_device.back())) {
            base_device.pop_back();
        }
    }
    
    std::string sys_block_path = "/sys/class/block/" + base_device;
    
    // Try MMC/SD Card CID first
    std::string device_id = readSysFile(sys_block_path + "/device/cid");
    if (!device_id.empty()) {
        return device_id;
    }
    
    // Try NVMe EUI
    device_id = readSysFile(sys_block_path + "/eui");
    if (!device_id.empty()) {
        return device_id;
    }
    
    // Try device serial
    device_id = readSysFile(sys_block_path + "/device/serial");
    if (!device_id.empty()) {
        return device_id;
    }
    
    // Check if this is a USB device - return error
    if (pathExists(sys_block_path + "/device")) {
        char resolved_path[PATH_MAX];
        std::string device_link = sys_block_path + "/device";
        if (realpath(device_link.c_str(), resolved_path) != nullptr) {
            std::string device_path = resolved_path;
            if (device_path.find("/usb") != std::string::npos) {
                return "ERROR_USB_NOT_SUPPORTED";
            }
        }
    }
    
    return "";
}

static std::string executeCommand(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        PLOG(WARNING) << "popen failed for command: " << command;
        return "";
    }
    
    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

// LUKS key generation from hardware device ID using rpi-fw-crypto
static std::string generateLuksKeyFromPartition(const std::string& block_device) {
    std::string device_id = getDeviceId(block_device);
    
    if (device_id.empty()) {
        return "";
    }
    
    if (device_id == "ERROR_USB_NOT_SUPPORTED") {
        return "ERROR_USB_NOT_SUPPORTED";
    }

    // Use RpiFwCrypto class to calculate HMAC
    rpi::RpiFwCrypto crypto;

    // Check if key is provisioned
    auto status = crypto.GetCachedProvisioningStatus();
    if (!status || !*status) {
        return "";
    }

    // Convert device_id string to vector
    std::vector<uint8_t> message(device_id.begin(), device_id.end());
    auto hmac_result = crypto.CalculateHmac(message);
    if (!hmac_result) {
        return "";
    }

    return *hmac_result;
}

static void PostWipeData() {
    std::string err;
    // Reset mte state of device.
    // if (!WriteMiscMemtagMessage({}, &err)) {
    //     LOG(ERROR) << "Failed to reset MTE state: " << err;
    // }
}

const std::unordered_map<std::string, std::function<bool(FastbootDevice*)>> kSpecialVars = {
        {"all", GetVarAll},
        {"dmesg", GetDmesg},
};

bool GetVarHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteFail("Missing argument");
    }

    // "all" and "dmesg" are multiline and handled specially.
    auto found_special = kSpecialVars.find(args[1]);
    if (found_special != kSpecialVars.end()) {
        if (!found_special->second(device)) {
            return false;
        }
        return device->WriteOkay("");
    }

    // args[0] is command name, args[1] is variable.
    auto found_variable = kVariableMap.find(args[1]);
    if (found_variable == kVariableMap.end()) {
        return device->WriteFail("Unknown variable");
    }

    std::string message;
    std::vector<std::string> getvar_args(args.begin() + 2, args.end());
    if (!found_variable->second.get(device, getvar_args, &message)) {
        return device->WriteFail(message);
    }
    return device->WriteOkay(message);
}

bool OemPostWipeData(FastbootDevice* device) {
    // auto fastboot_hal = device->fastboot_hal();
    // if (!fastboot_hal) {
        return false;
    // }

    // auto status = fastboot_hal->doOemSpecificErase();
    // if (status.isOk()) {
    //     device->WriteStatus(FastbootResult::OKAY, "Erasing succeeded");
    //     return true;
    // }
    // switch (status.getExceptionCode()) {
    //     case EX_UNSUPPORTED_OPERATION:
    //         return false;
    //     case EX_SERVICE_SPECIFIC:
    //         device->WriteStatus(FastbootResult::FAIL, status.getDescription());
    //         return false;
    //     default:
    //         LOG(ERROR) << "Erase operation failed" << status.getDescription();
    //         return false;
    // }
}

bool EraseHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteStatus(FastbootResult::FAIL, "Invalid arguments");
    }

    if (GetDeviceLockStatus()) {
        return device->WriteStatus(FastbootResult::FAIL, "Erase is not allowed on locked devices");
    }

    auto partition_name = args[1];
    partition_name.insert(0, "/dev/");
    if (IsProtectedPartitionDuringMerge(device, partition_name)) {
        auto message = "Cannot erase " + partition_name + " while a snapshot update is in progress";
        return device->WriteFail(message);
    }

    PartitionHandle handle;
    if (!OpenPartition(device, partition_name, &handle)) {
        return device->WriteStatus(FastbootResult::FAIL, "Partition doesn't exist");
    }
    if (android::wipe_block_device(handle.fd()) == 0) {
        //Perform oem PostWipeData if Android userdata partition has been erased
        // bool support_oem_postwipedata = false;
        // if (partition_name == "userdata") {
        //     PostWipeData();
        //     support_oem_postwipedata = OemPostWipeData(device);
        // }

        // if (!support_oem_postwipedata) {
            return device->WriteStatus(FastbootResult::OKAY, "Erasing succeeded");
        // } else {
        //     //Write device status in OemPostWipeData(), so just return true
        //     return true;
        // }
    }
    return device->WriteStatus(FastbootResult::FAIL, "Erasing failed");
}

namespace {
    #define PARTINIT_USAGE "e.g:\r\n\toem partinit mmcblk0 <label> [id]"
    static bool oem_cmd_partinit(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 4) {
            return device->WriteStatus(FastbootResult::FAIL, "Invalid argument count. Wanted oem partinit mmcblk0 <label> [id]");
        }
        auto target_device = args[2];
        auto disk_label = args[3];
        target_device.insert(0, "/dev/");
        RPIparted disk;
        bool result;

        if (!disk.openDevice(target_device, 4*1024)) {
            return device->WriteStatus(FastbootResult::FAIL, "Incorrect block device. " PARTINIT_USAGE);
        }

        // label_id is optional.
        if (args.size() == 5) {
           result = disk.createPartitionTable(disk_label.c_str(), args[4].c_str());
        }
        else {
           result = disk.createPartitionTable(disk_label.c_str(), std::nullopt);
        }

        if (!result) {
            return device->WriteStatus(FastbootResult::FAIL, "Failed to create disk label." PARTINIT_USAGE);
        }

        result = disk.commit();
        if (!result) {
            return device->WriteStatus(FastbootResult::FAIL, "Error writing PT." PARTINIT_USAGE);
        }

        return device->WriteStatus(FastbootResult::OKAY, "Initialised partition successfully");
    }

    constexpr const char * LINUX = "0fc63daf-8483-4772-8e79-3d69d8477de4";
    constexpr const char * SWAP = "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f";
    constexpr const char * HOME = "933ac7e1-2eb4-4f13-b844-0e14e2aef915";
    constexpr const char * EFI = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";
    constexpr const char * RAID = "a19d880f-05fc-4d3b-a006-743f0f84911e";
    constexpr const char * LVM = "e6d6d379-f507-44c2-a23c-238f2a3df928";
    constexpr const char * FAT32 = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7";

    static std::string map_code(std::string_view c) {
       if (c == "L" || c == "linux") return LINUX;
       if (c == "S" || c == "swap") return SWAP;
       if (c == "H" || c == "home") return HOME;
       if (c == "U" || c == "esp" || c == "uefi" ) return EFI;
       if (c == "R" || c == "raid") return RAID;
       if (c == "V" || c == "lvm") return LVM;
       if (c == "F" || c == "fat32") return FAT32;
       return std::string(c);
    }

    #define PARTAPP_USAGE "e.g:\r\n\toem partapp mmcblk0 <part code> <size>"
    static bool oem_cmd_partapp(FastbootDevice* device, const std::vector<std::string>& args) {
        int ret = -1;

        if (args.size() < 4) {
            return device->WriteStatus(FastbootResult::FAIL, "Invalid argument count. "  PARTAPP_USAGE);
        }

        auto block_device = args[2];
        std::string partition_type = map_code(args[3]);
        block_device.insert(0, "/dev/");
        RPIparted disk;

        if (!disk.openDevice(block_device, 4*1024)) {
            return device->WriteStatus(FastbootResult::FAIL, "Incorrect block device. " PARTAPP_USAGE);
        }

        PartitionAttributes attrs{};
        attrs.type_id = partition_type;
        attrs.size_bytes = 0; // default: consume remaining

        if (args.size() == 5) {
            auto size_string = args[4];

            if (!android::base::ParseUint(size_string, &attrs.size_bytes)) {
                return device->WriteStatus(FastbootResult::FAIL, "Unable to parse partition size. " PARTAPP_USAGE);
            }
        }

        if (!disk.appendPartition(attrs)) {
            return device->WriteStatus(FastbootResult::FAIL, "Failed to append partition. " PARTAPP_USAGE);
        }

        if (!disk.commit()) {
            return device->WriteStatus(FastbootResult::FAIL, "Error writing PT." PARTINIT_USAGE);
        }

        return device->WriteStatus(FastbootResult::OKAY, "Wrote new partition.");
    }

    // oem idpinit
    static bool oem_cmd_idp_init(FastbootDevice* device, const std::vector<std::string>& unused) {
       int size = device->download_data().size();
       char *ptr_data = device->download_data().data();

       if (size == 0) {
          return device->WriteFail("IDP:No data. Check description was staged");
       }

       if (device->idp) {
          return device->WriteFail("IDP:already initialised");
       }

       device->idp = new IDPdevice();

       if (!device->idp->Initialise(ptr_data, size)) {
          return device->WriteFail("IDP:invalid description");
       }

       if (!device->idp->canProvision()) {
          return device->WriteFail("IDP:cannot provision");
       }

       device->idpcookie = device->idp->createCookie();

       return device->WriteOkay("IDP:ready");
    }

    // oem idpwrite
    static bool oem_cmd_idp_write(FastbootDevice* device, const std::vector<std::string>& unused) {

       if (!device->idp) {
          return device->WriteFail("IDP:not initialised");
       }

       if (!device->idp->startProvision()) {
          return device->WriteFail("IDP:failed to start provisioning");
       }

       return device->WriteOkay("IDP:ok");
    }

    // oem idpgetblk
    static bool oem_cmd_idp_getblk(FastbootDevice* device, const std::vector<std::string>& unused) {
       if (!device->idp) {
          return device->WriteFail("IDP:not initialised");
       }

       auto bd = device->idp->getNextBlockDevice(*device->idpcookie);
       if (bd) {
          std::string blk = bd->blockDev;
          const std::string prefix = "/dev/";

          if (blk.rfind(prefix, 0) == 0) {
             blk = blk.substr(prefix.length());
          }
          std::string str = blk + ":" + bd->simg;
          device->WriteInfo(str);
       }
       return device->WriteOkay("IDP:done");
    }

    // oem idpdone
    static bool oem_cmd_idp_done(FastbootDevice* device, const std::vector<std::string>& unused) {
       if (!device->idp) {
          return device->WriteOkay("IDP:not initialised");
       }

       bool result = device->idp->endProvision();

       delete device->idp;
       device->idp = nullptr;

       if (result) {
          return device->WriteOkay("IDP:done");
       }

       return device->WriteFail("IDP:error finalising");
    }

    // oem cryptinit <blkdev> <label>
    static bool oem_cmd_cryptinit(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 4) {
            return device->WriteFail("Insufficient arguments. Usage: oem cryptinit <block_device> <label> [cipher]");
        }

        if (args[2].empty()) {
            return device->WriteFail("Block device not specified. Usage: oem cryptinit <block_device> <label> [cipher]");
        }

        auto block_device = args[2];
        block_device.insert(0, "/dev/");

        // Will appear as /dev/disk/by-label/<part_label> once opened
        if (args[3].empty()) {
            return device->WriteFail("Label not specified. Usage: oem cryptinit <block_device> <label>");
        }

        // Generate hardware-based LUKS key
        std::string luks_key = generateLuksKeyFromPartition(block_device);
        if (luks_key.empty()) {
            return device->WriteFail("Cannot generate LUKS key - unsupported device type");
        }
        if (luks_key == "ERROR_USB_NOT_SUPPORTED") {
            return device->WriteFail("USB devices not supported for LUKS encryption");
        }

        PartitionHandle handle;
        if (!OpenPartition(device, block_device, &handle, O_WRONLY | O_DIRECT)) {
            return device->WriteFail("Cannot create context for device " + block_device + " as failed to open");
        }

        std::string cipher = "aes-xts-plain64";
        if (args.size() >= 5) {
            cipher = args[4];
        }

#ifdef HAVE_LIBCRYPTSETUP
        std::string error_msg;
        if (CryptInitNative(block_device, args[3], cipher, luks_key, &error_msg)) {
            return device->WriteOkay("LUKS device formatted successfully");
        }
        
        LOG(WARNING) << "libcryptsetup failed: " << error_msg << ", falling back to command";
#endif

        std::string temp_keyfile = "/tmp/luks_key.tmp";
        std::ofstream keyfile_stream(temp_keyfile);
        if (!keyfile_stream) {
            return device->WriteFail("Cannot create temporary keyfile");
        }
        keyfile_stream << luks_key;
        keyfile_stream.close();

        std::string cipher_arg = "--cipher=" + cipher;

        char cryptsetup[]     = "cryptsetup";
        char batch_mode[]     = "--batch-mode";
        char luksFormat[]     = "luksFormat";
        char label[]          = "--label";
        char force_password[] = "--force-password";

        char * const argv[] = {
            cryptsetup,
            batch_mode,
            luksFormat,
            const_cast<char *>(cipher_arg.c_str()),
            label,
            const_cast<char *>(args[3].c_str()),
            force_password,
            const_cast<char *>(block_device.c_str()),
            (char*)"--key-file",
            const_cast<char *>(temp_keyfile.c_str()),
            NULL
        };

        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "cryptsetup", argv, NULL);

        std::remove(temp_keyfile.c_str());

        if (ret) {
            return device->WriteFail(strerror(ret));
        } else if (subprocess_rc) {
            return device->WriteFail("Cryptsetup failed");
        }

        return device->WriteOkay("Cryptsetup completed.");
    }

    // oem cryptopen <blkdev> <mapped name>
    static int oem_cmd_cryptopen(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 4) {
            return device->WriteFail("Insufficient arguments provided. Usage: oem cryptopen <block_device_path> <mapped_name>");
        }
        auto block_device_path = args[2];
        auto mapped_name = args[3];
        block_device_path.insert(0, "/dev/");

        // Generate hardware-based LUKS key
        std::string luks_key = generateLuksKeyFromPartition(block_device_path);
        if (luks_key.empty()) {
            return device->WriteFail("Cannot generate LUKS key - unsupported device type");
        }
        if (luks_key == "ERROR_USB_NOT_SUPPORTED") {
            return device->WriteFail("USB devices not supported for LUKS encryption");
        }

        PartitionHandle handle;
        if (!OpenPartition(device, block_device_path, &handle, O_WRONLY | O_DIRECT)) {
            return device->WriteFail("Cannot perform cryptopen for device " + block_device_path + " as failed to open.");
        }

        if (args[3].empty()) {
            return device->WriteFail("Cannot perform cryptopen for device " + block_device_path + " as no mapped name specified.");
        }

#ifdef HAVE_LIBCRYPTSETUP
        std::string error_msg;
        if (CryptOpenNative(block_device_path, mapped_name, luks_key, &error_msg)) {
            return device->WriteOkay("LUKS device opened successfully");
        }
        
        LOG(WARNING) << "libcryptsetup failed: " << error_msg << ", falling back to command";
#endif

        std::string temp_keyfile = "/tmp/luks_key_open.tmp";
        std::ofstream keyfile_stream(temp_keyfile);
        if (!keyfile_stream) {
            return device->WriteFail("Cannot create temporary keyfile");
        }
        keyfile_stream << luks_key;
        keyfile_stream.close();

        char cryptsetup[] = "cryptsetup";
        char luksOpen[]   = "luksOpen";

        char * const argv[] = {
            cryptsetup,
            luksOpen,
            (char*)"--key-file",
            const_cast<char *>(temp_keyfile.c_str()),
            const_cast<char *>(block_device_path.c_str()),
            const_cast<char *>(mapped_name.c_str()),
            NULL
        };

        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "cryptsetup", argv, NULL);

        std::remove(temp_keyfile.c_str());

        if (ret) {
            return device->WriteFail(strerror(ret));
        } else if (subprocess_rc) {
            return device->WriteFail("cryptsetup luksOpen failed.");
        }
    return device->WriteOkay("cryptsetup luksOpen completed.");
}

// oem cryptsetpassword <blkdev> <passphrase>
static bool oem_cmd_cryptsetpassword(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 4) {
        return device->WriteFail("Usage: oem cryptsetpassword <block_device> <passphrase>");
    }

    std::string block_device = args[2];
    std::string user_passphrase = args[3];
    block_device.insert(0, "/dev/");

    std::string luks_key = generateLuksKeyFromPartition(block_device);
    if (luks_key.empty()) {
        return device->WriteFail("Cannot generate LUKS key - unsupported device type");
    }
    if (luks_key == "ERROR_USB_NOT_SUPPORTED") {
        return device->WriteFail("USB devices not supported for LUKS encryption");
    }

#ifdef HAVE_LIBCRYPTSETUP
    std::string error_msg;
    bool remove_passphrase = user_passphrase.empty();
    if (CryptSetPasswordNative(block_device, luks_key, user_passphrase, remove_passphrase, &error_msg)) {
        if (remove_passphrase) {
            return device->WriteOkay("User passphrase removed");
        } else {
            return device->WriteOkay("User passphrase set successfully");
        }
    }
    
    LOG(WARNING) << "libcryptsetup failed: " << error_msg << ", falling back to command";
#endif

    std::string temp_hw_keyfile = "/tmp/luks_hw_key.tmp";
    std::ofstream hw_keyfile_stream(temp_hw_keyfile);
    if (!hw_keyfile_stream) {
        return device->WriteFail("Cannot create temporary hardware keyfile");
    }
    hw_keyfile_stream << luks_key;
    hw_keyfile_stream.close();

    if (user_passphrase.empty()) {
        char cryptsetup[] = "cryptsetup";
        char luksKillSlot[] = "luksKillSlot";
        char * const argv[] = {
            cryptsetup, luksKillSlot, (char*)"--key-file",
            const_cast<char *>(temp_hw_keyfile.c_str()),
            const_cast<char *>(block_device.c_str()), (char*)"1", NULL
        };
        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "cryptsetup", argv, NULL);
        std::remove(temp_hw_keyfile.c_str());
        if (ret != 0 || subprocess_rc != 0) {
            return device->WriteFail("Failed to remove user passphrase");
        }
        return device->WriteOkay("User passphrase removed.");
    }

    std::string temp_user_keyfile = "/tmp/luks_user_key.tmp";
    std::ofstream user_keyfile_stream(temp_user_keyfile);
    if (!user_keyfile_stream) {
        std::remove(temp_hw_keyfile.c_str());
        return device->WriteFail("Cannot create temporary user keyfile");
    }
    user_keyfile_stream << user_passphrase;
    user_keyfile_stream.close();

    char cryptsetup_kill[] = "cryptsetup";
    char luksKillSlot[] = "luksKillSlot";
    char * const kill_argv[] = {
        cryptsetup_kill, luksKillSlot, (char*)"--key-file",
        const_cast<char *>(temp_hw_keyfile.c_str()),
        const_cast<char *>(block_device.c_str()), (char*)"1", NULL
    };
    int kill_subprocess_rc = -1;
    rpi::process_spawn_blocking(&kill_subprocess_rc, "cryptsetup", kill_argv, NULL);

    char cryptsetup[] = "cryptsetup";
    char luksAddKey[] = "luksAddKey";
    char * const argv[] = {
        cryptsetup, luksAddKey, (char*)"--key-file",
        const_cast<char *>(temp_hw_keyfile.c_str()), (char*)"--key-slot", (char*)"1",
        const_cast<char *>(block_device.c_str()),
        const_cast<char *>(temp_user_keyfile.c_str()), NULL
    };

    int subprocess_rc = -1;
    int ret = rpi::process_spawn_blocking(&subprocess_rc, "cryptsetup", argv, NULL);

    std::remove(temp_hw_keyfile.c_str());
    std::remove(temp_user_keyfile.c_str());

    if (ret != 0 || subprocess_rc != 0) {
        return device->WriteFail("Failed to set user passphrase");
    }
    return device->WriteOkay("User passphrase set.");
}

ssize_t bulk_write(int bulk_in, const char *buf, size_t length)
    {
        size_t count = 0;
        ssize_t ret;

        do {
            ret = TEMP_FAILURE_RETRY(write(bulk_in, buf + count, length - count));
            if (ret < 0) {
                return -1;
            } else {
                count += ret;
            }
        } while (count < length);

        return count;
    }


    // oem led <LED> <value>
    // oem led PWR 1
    #define LED_USAGE "Usage:\r\n\toem led PWR 0"
    #define LED_DEVICE_PATH "/sys/devices/platform/leds/leds/"
    constexpr auto LED_ALLOWED_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static bool oem_cmd_led(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 4) {
            return device->WriteFail("Incorrect arguments for LED command: " LED_USAGE);
        }

        auto led = args[2];
        auto value = args[3];        
        char led_device_path[PATH_MAX];
        if (led.length() == 0 ||
            (led.length() != strspn(led.c_str(), LED_ALLOWED_CHARS)) ||
            (snprintf(led_device_path, PATH_MAX, LED_DEVICE_PATH "%s/brightness", led.c_str()) >= PATH_MAX))  {
            device->WriteInfo("Could not find LED device: " LED_USAGE);
            return device->WriteOkay("");
        }

        int led_device = open(led_device_path, O_WRONLY | O_TRUNC);
        if (led_device == -1) {
            device->WriteInfo("Could not open LED device: " LED_USAGE);
            return device->WriteOkay("");
        }

        // Happy to let the kernel validate this for us
        if (value.length() != bulk_write(led_device, value.c_str(), value.length())) {
            device->WriteInfo("Could not wite LED value.");
            return device->WriteOkay("");
        } else {
            return device->WriteOkay("Wrote LED value");
        }
    }

    #define DOWNLOAD_FILE_USAGE "e.g:\r\n\toem download-file <path-to-destination>"
    static bool oem_cmd_download_file(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return device->WriteStatus(FastbootResult::FAIL, "Invalid argument count. Wanted oem download-file <current-path> <path-to-destination>");
        }
        auto path = args[2].c_str();
        FILE* file = fopen(path, "w+");
        if (file == NULL){
            return device->WriteFail("Error opening file, ERRNO: " + std::to_string(errno));
        }
        int size = device->download_data().size();
        int cursor = 0;
        int page_size = 0;
        char *ptr_data = device->download_data().data();

        if (size==0){
            return device->WriteFail("Buffer size zero. Check file was successfully staged");
        }
        page_size = sysconf(_SC_PAGE_SIZE);

        while (cursor < size)
        {
            cursor += fwrite(ptr_data + cursor, sizeof device->download_data().data()[0], std::min(page_size, size - cursor), file);
            fflush(file);
        }

        device->download_data().clear();
        return device->WriteStatus(FastbootResult::OKAY, "Write Successful!");
    }

    #define UPLOAD_FILE_USAGE "e.g:\r\n\toem upload-file <path-on-client>"
    static bool oem_cmd_upload_file(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return device->WriteStatus(FastbootResult::FAIL, "Invalid argument count. Wanted [oem upload-file <path-on-client>]");
        }
        auto path = args[2].c_str();
    
        FILE* file = fopen(path, "r");
        if (file == NULL){
            return device->WriteFail("Error opening file, ERRNO: " + std::to_string(errno));
        }

        // Get file size
        fseek(file, 0, SEEK_END);
        int size = ftell(file);
        rewind(file);
        if (size==0){
            return device->WriteFail("Filesize zero. Will not upload empty file");
        }
        // Resize data buffer to match that size
        device->download_data().resize(size);

        int cursor = 0;
        int page_size = sysconf(_SC_PAGE_SIZE);
        char *ptr_data = device->download_data().data();

        while (cursor < size)
        {
            cursor += fread(ptr_data + cursor, sizeof device->download_data().data()[0], std::min(page_size, size - cursor), file);
        }
        return device->WriteStatus(FastbootResult::OKAY, "Write Successful!");
    }

// Native GPIO control using libgpiod v2.x API
#ifdef HAVE_LIBGPIOD
    // Parse and set GPIO lines: "chip offset=value [offset=value...]"
    // Example: "gpioset gpiochip0 23=1" or "gpioset gpiochip0 23=1 24=0"
    bool SetGpioLinesNative(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return false;
        }
        
        const std::string& chip_name = args[1];
        
        std::vector<unsigned int> line_nums;
        std::vector<int> line_values;
        
        for (size_t i = 2; i < args.size(); i++) {
            size_t eq_pos = args[i].find('=');
            if (eq_pos == std::string::npos) {
                LOG(WARNING) << "Invalid GPIO line format: " << args[i];
                return false;
            }
            
            try {
                unsigned int line_num = std::stoul(args[i].substr(0, eq_pos));
                int value = std::stoi(args[i].substr(eq_pos + 1));
                
                line_nums.push_back(line_num);
                line_values.push_back(value ? 1 : 0);  // Normalize to 0 or 1
            } catch (const std::exception& e) {
                LOG(WARNING) << "Failed to parse GPIO line: " << args[i] 
                            << " - " << e.what();
                return false;
            }
        }
        
        struct gpiod_chip* chip = gpiod_chip_open(("/dev/" + chip_name).c_str());
        if (!chip) {
            LOG(WARNING) << "Failed to open GPIO chip '" << chip_name 
                        << "': " << strerror(errno);
            return false;
        }
        
        struct gpiod_line_settings* settings = gpiod_line_settings_new();
        if (!settings) {
            gpiod_chip_close(chip);
            LOG(WARNING) << "Failed to create line settings";
            return false;
        }
        
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
        
        struct gpiod_line_config* line_cfg = gpiod_line_config_new();
        if (!line_cfg) {
            gpiod_line_settings_free(settings);
            gpiod_chip_close(chip);
            LOG(WARNING) << "Failed to create line config";
            return false;
        }
        
        for (size_t i = 0; i < line_nums.size(); i++) {
            gpiod_line_settings_set_output_value(settings, 
                static_cast<enum gpiod_line_value>(line_values[i]));
            
            int ret = gpiod_line_config_add_line_settings(line_cfg, &line_nums[i], 1, settings);
            if (ret < 0) {
                gpiod_line_config_free(line_cfg);
                gpiod_line_settings_free(settings);
                gpiod_chip_close(chip);
                LOG(WARNING) << "Failed to add line " << line_nums[i] << " to config";
                return false;
            }
        }
        
        struct gpiod_request_config* req_cfg = gpiod_request_config_new();
        if (!req_cfg) {
            gpiod_line_config_free(line_cfg);
            gpiod_line_settings_free(settings);
            gpiod_chip_close(chip);
            LOG(WARNING) << "Failed to create request config";
            return false;
        }
        
        gpiod_request_config_set_consumer(req_cfg, "fastbootd");
        
        struct gpiod_line_request* request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
        
        gpiod_request_config_free(req_cfg);
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        
        if (!request) {
            gpiod_chip_close(chip);
            LOG(WARNING) << "Failed to request GPIO lines: " << strerror(errno);
            return false;
        }
        
        for (size_t i = 0; i < line_nums.size(); i++) {
            LOG(INFO) << "Set GPIO " << chip_name << " line " << line_nums[i] 
                     << " to " << line_values[i];
        }
        
        gpiod_line_request_release(request);
        gpiod_chip_close(chip);
        
        return true;
    }
#endif // HAVE_LIBGPIOD

    static bool oem_cmd_gpioset(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return device->WriteFail("oem gpioset gpiochip0 [OPTIONS] <line=value>...");
        }

#ifdef HAVE_LIBGPIOD
        // Try libgpiod first
        if (SetGpioLinesNative(device, std::vector<std::string>(args.begin() + 1, args.end()))) {
            return device->WriteOkay("GPIO set successfully");
        }
        
        LOG(WARNING) << "libgpiod failed, falling back to gpioset command";
#endif

            // Fallback to gpioset command
        // Convert gpioset_args to char* array
        std::vector<char *> argv_vec(args.size());
        // Advance beyond 'oem'
        auto itr = args.begin();
        std::advance(itr, 1);
        std::transform(itr, args.end(), argv_vec.begin(),
                      [](const std::string& str) { return const_cast<char*>(str.c_str()); });
        argv_vec[args.size() - 1] = NULL;
        char * const *argv = argv_vec.data();

        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "gpioset", argv, NULL);

        if (ret) {
            return device->WriteFail(strerror(ret));
        } else if (subprocess_rc) {
            return device->WriteFail("gpioset failed");
        }

        return device->WriteOkay("gpioset completed.");
    }

    // oem veritysetup <device_name>
    // Calculates dm-verity hash for device and stores in /persistent/$device_name.verity
    static bool oem_cmd_veritysetup(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return device->WriteFail("Usage: oem veritysetup <device_name>");
        }

        std::string device_name = args[2];
        std::string data_device = "/dev/" + device_name;
        
        // Ensure /persistent directory exists
        if (mkdir("/persistent", 0755) != 0 && errno != EEXIST) {
            return device->WriteFail("Failed to create /persistent directory: " + std::string(strerror(errno)));
        }
        
        std::string hash_file = "/persistent/" + device_name + ".verity";
        
        // Verify data device exists
        PartitionHandle handle;
        if (!OpenPartition(device, data_device, &handle, O_RDONLY)) {
            return device->WriteFail("Cannot open device " + data_device);
        }

#ifdef HAVE_LIBCRYPTSETUP
        // Try native libcryptsetup implementation
        std::string root_hash;
        std::string error_msg;
        
        if (VeritySetupNative(data_device, hash_file, &root_hash, &error_msg)) {
            device->WriteInfo("Root hash: " + root_hash);
            device->WriteInfo("Hash stored in: " + hash_file);
            return device->WriteOkay("dm-verity setup completed");
        }
        
        LOG(WARNING) << "libcryptsetup failed: " << error_msg << ", falling back to veritysetup command";
#endif

        // Fallback to veritysetup command
        char veritysetup[] = "veritysetup";
        char format_cmd[] = "format";
        char root_hash_arg[] = "--root-hash-file";
        
        std::string root_hash_file = hash_file + ".roothash";
        
        char * const argv[] = {
            veritysetup,
            format_cmd,
            root_hash_arg,
            const_cast<char*>(root_hash_file.c_str()),
            const_cast<char*>(data_device.c_str()),
            const_cast<char*>(hash_file.c_str()),
            NULL
        };

        int subprocess_rc = -1;
        int ret = rpi::process_spawn_blocking(&subprocess_rc, "veritysetup", argv, NULL);

        if (ret) {
            return device->WriteFail("veritysetup spawn failed: " + std::string(strerror(ret)));
        } else if (subprocess_rc) {
            return device->WriteFail("veritysetup command failed");
        }
        
        // Read and display the root hash
        std::string root_hash_content;
        if (android::base::ReadFileToString(root_hash_file, &root_hash_content)) {
            // Trim whitespace
            root_hash_content.erase(root_hash_content.find_last_not_of(" \n\r\t") + 1);
            device->WriteInfo("Root hash: " + root_hash_content);
        }
        
        device->WriteInfo("Hash stored in: " + hash_file);
        return device->WriteOkay("dm-verity setup completed");
    }

    // oem verityappend <device_name> <data_size_bytes>
    // Appends dm-verity hash tree to the end of the same device
    static bool oem_cmd_verityappend(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 4) {
            return device->WriteFail("Usage: oem verityappend <device_name> <data_size_bytes>");
        }

        std::string device_name = args[2];
        std::string data_device = "/dev/" + device_name;
        
        uint64_t data_size;
        if (!android::base::ParseUint(args[3], &data_size)) {
            return device->WriteFail("Invalid data size: " + args[3]);
        }
        
        // Verify device exists
        PartitionHandle handle;
        if (!OpenPartition(device, data_device, &handle, O_RDWR)) {
            return device->WriteFail("Cannot open device " + data_device);
        }
        
        // Get device size
        uint64_t device_size = android::get_block_device_size(handle.fd());
        if (device_size == 0) {
            return device->WriteFail("Cannot get device size");
        }
        
        // Validate data size
        if (data_size > device_size) {
            return device->WriteFail("Data size exceeds device size");
        }
        
        device->WriteInfo("Device size: " + std::to_string(device_size) + " bytes");
        device->WriteInfo("Data size: " + std::to_string(data_size) + " bytes");
        device->WriteInfo("Hash tree will be appended at offset: " + std::to_string(data_size));

#ifdef HAVE_LIBCRYPTSETUP
        // Try native libcryptsetup implementation
        std::string root_hash;
        std::string error_msg;
        
        if (VeritySetupAppendedNative(data_device, data_size, &root_hash, &error_msg)) {
            device->WriteInfo("Root hash: " + root_hash);
            device->WriteInfo("Hash tree appended to device");
            device->WriteInfo("Data blocks: " + std::to_string(data_size / 4096));
            return device->WriteOkay("dm-verity appended setup completed");
        }
        
        LOG(WARNING) << "libcryptsetup failed: " << error_msg << ", falling back to veritysetup command";
#endif

        // Fallback to veritysetup command with same device
        return device->WriteFail("veritysetup command fallback not yet implemented for appended mode");
    }

    // -------------------------------------------------------------------------
    // oem bmap-load <block_device>   e.g. oem bmap-load mmcblk0
    // oem bmap-verify <block_device> e.g. oem bmap-verify mmcblk0
    //
    // Binary bmap format (little-endian):
    //   Header (16 bytes):
    //     uint32 magic       = 0x50414D42 ('BMAP')
    //     uint32 block_size  (typically 4096)
    //     uint32 range_count
    //     uint32 reserved    = 0
    //   Per range (40 bytes each):
    //     uint32 start_block  (inclusive)
    //     uint32 end_block    (inclusive)
    //     uint8  sha256[32]
    // -------------------------------------------------------------------------

    static constexpr uint32_t BMAP_MAGIC = 0x50414D42;

    struct BmapHeader {
        uint32_t magic;
        uint32_t block_size;
        uint32_t range_count;
        uint32_t reserved;
    };

    struct BmapRange {
        uint32_t start_block;
        uint32_t end_block;
        uint8_t  sha256[32];
    };

    struct BmapState {
        std::string block_device;
        uint32_t block_size = 0;
        std::vector<BmapRange> ranges;
    };

    static BmapState g_bmap_state;

    static bool oem_cmd_bmap_load(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return device->WriteFail("Usage: oem bmap-load <block_device>");
        }
        const std::string& block_device = args[2];

        const std::vector<char>& buf = device->download_data();
        if (buf.size() < sizeof(BmapHeader)) {
            return device->WriteFail("bmap-load: download buffer too small for header");
        }

        BmapHeader hdr;
        memcpy(&hdr, buf.data(), sizeof(hdr));

        if (hdr.magic != BMAP_MAGIC) {
            return device->WriteFail("bmap-load: bad magic");
        }
        if (hdr.block_size == 0 || (hdr.block_size & (hdr.block_size - 1)) != 0) {
            return device->WriteFail("bmap-load: block_size must be a power of two");
        }

        size_t expected = sizeof(BmapHeader) + (size_t)hdr.range_count * sizeof(BmapRange);
        if (buf.size() < expected) {
            return device->WriteFail("bmap-load: download buffer too small for declared ranges");
        }

        g_bmap_state.block_device = "/dev/" + block_device;
        g_bmap_state.block_size   = hdr.block_size;
        g_bmap_state.ranges.resize(hdr.range_count);
        memcpy(g_bmap_state.ranges.data(), buf.data() + sizeof(BmapHeader),
               hdr.range_count * sizeof(BmapRange));

        return device->WriteOkay(android::base::StringPrintf(
                "bmap loaded: %u ranges, block_size=%u, device=%s",
                hdr.range_count, hdr.block_size, block_device.c_str()));
    }

    static bool oem_cmd_bmap_verify(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return device->WriteFail("Usage: oem bmap-verify <block_device>");
        }
        const std::string block_device = "/dev/" + args[2];

        if (g_bmap_state.ranges.empty()) {
            return device->WriteFail("bmap-verify: no bmap loaded; run oem bmap-load first");
        }
        if (g_bmap_state.block_device != block_device) {
            return device->WriteFail("bmap-verify: loaded bmap is for " +
                                     g_bmap_state.block_device + ", not " + block_device);
        }

        PartitionHandle handle;
        if (!OpenPartition(device, block_device, &handle, O_RDONLY)) {
            return device->WriteFail("bmap-verify: cannot access " + block_device);
        }
        if (!handle.Open(O_RDONLY)) {
            return device->WriteFail("bmap-verify: cannot open " + block_device);
        }

        const uint32_t block_size  = g_bmap_state.block_size;
        const uint32_t range_count = g_bmap_state.ranges.size();
        constexpr size_t kReadBuf  = 1 * 1024 * 1024; // 1 MB chunks
        std::vector<uint8_t> read_buf(kReadBuf);

        for (uint32_t i = 0; i < range_count; i++) {
            const BmapRange& r = g_bmap_state.ranges[i];

            device->WriteInfo(android::base::StringPrintf(
                    "verifying range %u/%u (blocks %u-%u)",
                    i, range_count, r.start_block, r.end_block));

            uint64_t offset     = (uint64_t)r.start_block * block_size;
            uint64_t range_bytes = (uint64_t)(r.end_block - r.start_block + 1) * block_size;

            if (lseek64(handle.fd(), (off64_t)offset, SEEK_SET) < 0) {
                return device->WriteFail(android::base::StringPrintf(
                        "bmap-verify: seek failed for range %u: %s", i, strerror(errno)));
            }

            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            if (!ctx) return device->WriteFail("bmap-verify: failed to allocate hash context");

            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

            uint64_t remaining = range_bytes;
            bool io_error = false;
            while (remaining > 0) {
                size_t to_read = (size_t)std::min(remaining, (uint64_t)kReadBuf);
                ssize_t n = read(handle.fd(), read_buf.data(), to_read);
                if (n <= 0) {
                    device->WriteFail(android::base::StringPrintf(
                            "bmap-verify: read error at range %u: %s", i, strerror(errno)));
                    io_error = true;
                    break;
                }
                EVP_DigestUpdate(ctx, read_buf.data(), n);
                remaining -= n;
            }

            uint8_t actual[32];
            unsigned int digest_len = 0;
            EVP_DigestFinal_ex(ctx, actual, &digest_len);
            EVP_MD_CTX_free(ctx);

            if (io_error) return false;

            if (memcmp(actual, r.sha256, 32) != 0) {
                // Format both hashes as hex for the failure message
                char expected_hex[65], actual_hex[65];
                for (int j = 0; j < 32; j++) {
                    snprintf(expected_hex + j*2, 3, "%02x", r.sha256[j]);
                    snprintf(actual_hex   + j*2, 3, "%02x", actual[j]);
                }
                return device->WriteFail(android::base::StringPrintf(
                        "%u:%s:%s", i, expected_hex, actual_hex));
            }
        }

        return device->WriteOkay(android::base::StringPrintf(
                "verified %u ranges ok", range_count));
    }

    // oem fwcrypto <status|init>
    static bool oem_cmd_fwcrypto(FastbootDevice* device, const std::vector<std::string>& args) {
        if (args.size() < 3) {
            return device->WriteFail("Usage: oem fwcrypto <status|init>");
        }

        std::string subcommand = args[2];
        rpi::RpiFwCrypto crypto;

        if (subcommand == "status") {
            // Get the cached provisioning status
            auto status = crypto.GetCachedProvisioningStatus();
            if (!status) {
                return device->WriteFail("Failed to get key status");
            }

            if (*status) {
                return device->WriteOkay("Key is provisioned");
            } else {
                return device->WriteOkay("Key is not provisioned");
            }
        } else if (subcommand == "init") {
            // Check if key is already provisioned
            auto status = crypto.GetCachedProvisioningStatus();
            if (!status) {
                return device->WriteFail("Failed to get key status");
            }

            if (*status) {
                return device->WriteFail("Key is already provisioned");
            }

            // Provision the key
            int result = crypto.ProvisionKey();
            if (result == 0) {
                return device->WriteOkay("Key provisioned successfully");
            } else {
                return device->WriteFail("Failed to provision key");
            }
        } else {
            return device->WriteFail("Unknown fwcrypto subcommand. Use 'status' or 'init'");
        }
    }
} //namespace anonymous

bool OemCmdHandler(FastbootDevice* device, const std::vector<std::string>& args) {

    std::string message = {};
    // auto status = fastboot_hal->doOemCommand(args[0], &message);
    // if (!status.isOk()) {
    //     LOG(ERROR) << "Unable to do OEM command " << args[0].c_str() << status.getDescription();
    //     return device->WriteStatus(FastbootResult::FAIL,
    //                                "Unable to do OEM command " + status.getDescription());
    // }

    // device->WriteInfo(message);
    std::vector<std::string> split_args = android::base::Split(args[0], " ");
    auto command_name = split_args[1];

    if (command_name == "led") {
        return oem_cmd_led(device, split_args);
    } else if (command_name == "cryptinit") {
        return oem_cmd_cryptinit(device, split_args);
    } else if (command_name == "cryptopen") {
        return oem_cmd_cryptopen(device, split_args);
    } else if (command_name == "cryptsetpassword") {
        return oem_cmd_cryptsetpassword(device, split_args);
    } else if (command_name == "partinit") {
        return oem_cmd_partinit(device, split_args);
    } else if (command_name == "partapp") {
        return oem_cmd_partapp(device, split_args);
    } else if (command_name == "download-file") {
        return oem_cmd_download_file(device, split_args);
    } else if (command_name == "upload-file") {
        return oem_cmd_upload_file(device, split_args);
    } else if (command_name == "gpioset") {
        return oem_cmd_gpioset(device, split_args);
    } else if (command_name == "veritysetup") {
        return oem_cmd_veritysetup(device, split_args);
    } else if (command_name == "verityappend") {
        return oem_cmd_verityappend(device, split_args);
    } else if (command_name == "idpinit") {
        return oem_cmd_idp_init(device, split_args);
    } else if (command_name == "idpwrite") {
        return oem_cmd_idp_write(device, split_args);
    } else if (command_name == "idpgetblk") {
        return oem_cmd_idp_getblk(device, split_args);
    } else if (command_name == "idpdone") {
        return oem_cmd_idp_done(device, split_args);
    } else if (command_name == "fwcrypto") {
        return oem_cmd_fwcrypto(device, split_args);
    } else if (command_name == "bmap-load") {
        return oem_cmd_bmap_load(device, split_args);
    } else if (command_name == "bmap-verify") {
        return oem_cmd_bmap_verify(device, split_args);
    }

    return device->WriteFail("Unknown OEM command.");
}

bool DownloadHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteStatus(FastbootResult::FAIL, "size argument unspecified");
    }

    if (GetDeviceLockStatus()) {
        return device->WriteStatus(FastbootResult::FAIL,
                                   "Download is not allowed on locked devices");
    }

    // arg[0] is the command name, arg[1] contains size of data to be downloaded
    // which should always be 8 bytes
    if (args[1].length() != 8) {
        return device->WriteStatus(FastbootResult::FAIL,
                                   "Invalid size (length of size != 8)");
    }
    unsigned int size;
    if (!android::base::ParseUint("0x" + args[1], &size, kMaxDownloadSizeDefault)) {
        return device->WriteStatus(FastbootResult::FAIL, "Invalid size");
    }
    if (size == 0) {
        return device->WriteStatus(FastbootResult::FAIL, "Invalid size (0)");
    }
    device->download_data().resize(size);
    if (!device->WriteStatus(FastbootResult::DATA, android::base::StringPrintf("%08x", size))) {
        return false;
    }

    if (device->HandleData(true, &device->download_data())) {
        return device->WriteStatus(FastbootResult::OKAY, "");
    }

    //PLOG(ERROR) << "Couldn't download data";
    return device->WriteStatus(FastbootResult::FAIL, "Couldn't download data");
}

bool UploadHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    unsigned int size;
    
    // download data size should already be set by oem cmd
    size = device->download_data().size();

    if (size == 0) {
        return device->WriteStatus(FastbootResult::FAIL, "Invalid size (0)");
    }

    if (!device->WriteStatus(FastbootResult::DATA, android::base::StringPrintf("%08x", size))) {
        return false;
    }

    // Data needs to already be staged to the device->download_data buffer for this to work.
    if (device->HandleData(false, &device->download_data())) {
        device->download_data().clear();
        return device->WriteStatus(FastbootResult::OKAY, "");
    }
    return device->WriteStatus(FastbootResult::FAIL, "Couldn't upload data");
}

bool SetActiveHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteStatus(FastbootResult::FAIL, "Missing slot argument");
    }

    if (GetDeviceLockStatus()) {
        return device->WriteStatus(FastbootResult::FAIL,
                                   "set_active command is not allowed on locked devices");
    }

    int32_t slot = 0;
    if (!GetSlotNumber(args[1], &slot)) {
        // Slot suffix needs to be between 'a' and 'z'.
        return device->WriteStatus(FastbootResult::FAIL, "Bad slot suffix");
    }

    // // Non-A/B devices will not have a boot control HAL.
    // auto boot_control_hal = device->boot_control_hal();
    // if (!boot_control_hal) {
    //     return device->WriteStatus(FastbootResult::FAIL,
    //                                "Cannot set slot: boot control HAL absent");
    // }
    // if (slot >= boot_control_hal->GetNumSlots()) {
    //     return device->WriteStatus(FastbootResult::FAIL, "Slot out of range");
    // }

    // If the slot is not changing, do nothing.
    if (args[1] == device->GetCurrentSlot()) {
        return device->WriteOkay("");
    }

    // Check how to handle the current snapshot state.
    // if (auto hal11 = device->boot1_1()) {
    //     auto merge_status = hal11->getSnapshotMergeStatus();
    //     if (merge_status == MergeStatus::MERGING) {
    //         return device->WriteFail("Cannot change slots while a snapshot update is in progress");
    //     }
    //     // Note: we allow the slot change if the state is SNAPSHOTTED. First-
    //     // stage init does not have access to the HAL, and uses the slot number
    //     // and /metadata OTA state to determine whether a slot change occurred.
    //     // Booting into the old slot would erase the OTA, and switching A->B->A
    //     // would simply resume it if no boots occur in between. Re-flashing
    //     // partitions implicitly cancels the OTA, so leaving the state as-is is
    //     // safe.
    //     if (merge_status == MergeStatus::SNAPSHOTTED) {
    //         device->WriteInfo(
    //                 "Changing the active slot with a snapshot applied may cancel the"
    //                 " update.");
    //     }
    // }

    // CommandResult ret = boot_control_hal->SetActiveBootSlot(slot);
    // if (ret.success) {
    //     // Save as slot suffix to match the suffix format as returned from
    //     // the boot control HAL.
    //     auto current_slot = "_" + args[1];
    //     device->set_active_slot(current_slot);
    //     return device->WriteStatus(FastbootResult::OKAY, "");
    // }
    return device->WriteStatus(FastbootResult::FAIL, "Unable to set slot");
}

bool ShutDownHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Shutting down");
    // android::base::SetProperty(ANDROID_RB_PROPERTY, "shutdown,fastboot");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

bool RebootHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Rebooting");
    // android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,from_fastboot");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

bool RebootBootloaderHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Rebooting bootloader");
    // android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,bootloader");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

bool RebootFastbootHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
    auto result = device->WriteStatus(FastbootResult::OKAY, "Rebooting fastboot");
    // android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,fastboot");
    device->CloseDevice();
    TEMP_FAILURE_RETRY(pause());
    return result;
}

// static bool EnterRecovery() {
//     const char msg_switch_to_recovery = 'r';

//     android::base::unique_fd sock(socket(AF_UNIX, SOCK_STREAM, 0));
//     if (sock < 0) {
//         PLOG(ERROR) << "Couldn't create sock";
//         return false;
//     }

//     struct sockaddr_un addr = {.sun_family = AF_UNIX};
//     strncpy(addr.sun_path, "/dev/socket/recovery", sizeof(addr.sun_path) - 1);
//     if (connect(sock.get(), (struct sockaddr*)&addr, sizeof(addr)) < 0) {
//         PLOG(ERROR) << "Couldn't connect to recovery";
//         return false;
//     }
//     // Switch to recovery will not update the boot reason since it does not
//     // require a reboot.
//     auto ret = write(sock.get(), &msg_switch_to_recovery, sizeof(msg_switch_to_recovery));
//     if (ret != sizeof(msg_switch_to_recovery)) {
//         PLOG(ERROR) << "Couldn't write message to switch to recovery";
//         return false;
//     }

//     return true;
// }

// bool RebootRecoveryHandler(FastbootDevice* device, const std::vector<std::string>& /* args */) {
//     auto status = true;
//     if (EnterRecovery()) {
//         status = device->WriteStatus(FastbootResult::OKAY, "Rebooting to recovery");
//     } else {
//         status = device->WriteStatus(FastbootResult::FAIL, "Unable to reboot to recovery");
//     }
//     device->CloseDevice();
//     TEMP_FAILURE_RETRY(pause());
//     return status;
// }

// Helper class for opening a handle to a MetadataBuilder and writing the new
// // partition table to the same place it was read.
// class PartitionBuilder {
//   public:
//     explicit PartitionBuilder(FastbootDevice* device, const std::string& partition_name);

//     bool Write();
//     bool Valid() const { return !!builder_; }
//     MetadataBuilder* operator->() const { return builder_.get(); }

//   private:
//     FastbootDevice* device_;
//     std::string super_device_;
//     uint32_t slot_number_;
//     std::unique_ptr<MetadataBuilder> builder_;
// };

// PartitionBuilder::PartitionBuilder(FastbootDevice* device, const std::string& partition_name)
//     : device_(device) {
//     std::string slot_suffix = GetSuperSlotSuffix(device, partition_name);
//     slot_number_ = android::fs_mgr::SlotNumberForSlotSuffix(slot_suffix);
//     auto super_device = FindPhysicalPartition(fs_mgr_get_super_partition_name(slot_number_));
//     if (!super_device) {
//         return;
//     }
//     super_device_ = *super_device;
//     builder_ = MetadataBuilder::New(super_device_, slot_number_);
// }

// bool PartitionBuilder::Write() {
//     auto metadata = builder_->Export();
//     if (!metadata) {
//         return false;
//     }
//     return UpdateAllPartitionMetadata(device_, super_device_, *metadata.get());
// }

bool CreatePartitionHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        return device->WriteFail("Invalid partition name and size");
    }

    if (GetDeviceLockStatus()) {
        return device->WriteStatus(FastbootResult::FAIL, "Command not available on locked devices");
    }

    uint64_t partition_size;
    std::string partition_name = args[1];
    if (!android::base::ParseUint(args[2].c_str(), &partition_size)) {
        return device->WriteFail("Invalid partition size");
    }

    // PartitionBuilder builder(device, partition_name);
    // if (!builder.Valid()) {
    //     return device->WriteFail("Could not open super partition");
    // }
    // // TODO(112433293) Disallow if the name is in the physical table as well.
    // if (builder->FindPartition(partition_name)) {
    //     return device->WriteFail("Partition already exists");
    // }

    // auto partition = builder->AddPartition(partition_name, 0);
    // if (!partition) {
    //     return device->WriteFail("Failed to add partition");
    // }
    // if (!builder->ResizePartition(partition, partition_size)) {
    //     builder->RemovePartition(partition_name);
    //     return device->WriteFail("Not enough space for partition");
    // }
    // if (!builder.Write()) {
        return device->WriteFail("Failed to write partition table");
    // }
    // return device->WriteOkay("Partition created");
}

bool DeletePartitionHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteFail("Invalid partition name and size");
    }

    if (GetDeviceLockStatus()) {
        return device->WriteStatus(FastbootResult::FAIL, "Command not available on locked devices");
    }

    std::string partition_name = args[1];

    // PartitionBuilder builder(device, partition_name);
    // if (!builder.Valid()) {
    //     return device->WriteFail("Could not open super partition");
    // }
    // builder->RemovePartition(partition_name);
    // if (!builder.Write()) {
        return device->WriteFail("Failed to write partition table");
    // }
    // return device->WriteOkay("Partition deleted");
}

bool ResizePartitionHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        return device->WriteFail("Invalid partition name and size");
    }

    if (GetDeviceLockStatus()) {
        return device->WriteStatus(FastbootResult::FAIL, "Command not available on locked devices");
    }

    uint64_t partition_size;
    std::string partition_name = args[1];
    if (!android::base::ParseUint(args[2].c_str(), &partition_size)) {
        return device->WriteFail("Invalid partition size");
    }

    // PartitionBuilder builder(device, partition_name);
    // if (!builder.Valid()) {
    //     return device->WriteFail("Could not open super partition");
    // }

    // auto partition = builder->FindPartition(partition_name);
    // if (!partition) {
    //     return device->WriteFail("Partition does not exist");
    // }

    // // Remove the updated flag to cancel any snapshots.
    // uint32_t attrs = partition->attributes();
    // partition->set_attributes(attrs & ~LP_PARTITION_ATTR_UPDATED);

    // if (!builder->ResizePartition(partition, partition_size)) {
    //     return device->WriteFail("Not enough space to resize partition");
    // }
    // if (!builder.Write()) {
        return device->WriteFail("Failed to write partition table");
    // }
    // return device->WriteOkay("Partition resized");
}

// void CancelPartitionSnapshot(FastbootDevice* device, const std::string& partition_name) {
//     PartitionBuilder builder(device, partition_name);
//     if (!builder.Valid()) return;

//     auto partition = builder->FindPartition(partition_name);
//     if (!partition) return;

//     // Remove the updated flag to cancel any snapshots.
//     uint32_t attrs = partition->attributes();
//     partition->set_attributes(attrs & ~LP_PARTITION_ATTR_UPDATED);

//     builder.Write();
// }

bool FlashHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return device->WriteStatus(FastbootResult::FAIL, "Invalid arguments");
    }

    if (GetDeviceLockStatus()) {
        return device->WriteStatus(FastbootResult::FAIL,
                                   "Flashing is not allowed on locked devices");
    }

    const auto& partition_name = args[1];
    // if (IsProtectedPartitionDuringMerge(device, partition_name)) {
    //     auto message = "Cannot flash " + partition_name + " while a snapshot update is in progress";
    //     return device->WriteFail(message);
    // }

    // if (LogicalPartitionExists(device, partition_name)) {
    //     CancelPartitionSnapshot(device, partition_name);
    // }

    int ret = Flash(device, partition_name);
    if (ret < 0) {
        return device->WriteStatus(FastbootResult::FAIL, strerror(-ret));
    }
    // if (partition_name == "userdata") {
    //     PostWipeData();
    // }

    return device->WriteStatus(FastbootResult::OKAY, "Flashing succeeded");
}

// bool UpdateSuperHandler(FastbootDevice* device, const std::vector<std::string>& args) {
//     if (args.size() < 2) {
//         return device->WriteFail("Invalid arguments");
//     }

//     if (GetDeviceLockStatus()) {
//         return device->WriteStatus(FastbootResult::FAIL, "Command not available on locked devices");
//     }

//     return device->WriteFail("Command unavailable");

//     // bool wipe = (args.size() >= 3 && args[2] == "wipe");
//     // return UpdateSuper(device, args[1], wipe);
// }

// bool GsiHandler(FastbootDevice* device, const std::vector<std::string>& args) {
//     if (args.size() != 2) {
//         return device->WriteFail("Invalid arguments");
//     }

//     AutoMountMetadata mount_metadata;
//     if (!mount_metadata) {
//         return device->WriteFail("Could not find GSI install");
//     }

//     if (!android::gsi::IsGsiInstalled()) {
//         return device->WriteStatus(FastbootResult::FAIL, "No GSI is installed");
//     }

//     if (args[1] == "wipe") {
//         if (!android::gsi::UninstallGsi()) {
//             return device->WriteStatus(FastbootResult::FAIL, strerror(errno));
//         }
//     } else if (args[1] == "disable") {
//         if (!android::gsi::DisableGsi()) {
//             return device->WriteStatus(FastbootResult::FAIL, strerror(errno));
//         }
//     }
//     return device->WriteStatus(FastbootResult::OKAY, "Success");
// }

// bool SnapshotUpdateHandler(FastbootDevice* device, const std::vector<std::string>& args) {
//     // Note that we use the HAL rather than mounting /metadata, since we want
//     // our results to match the bootloader.
//     auto hal = device->boot1_1();
//     if (!hal) return device->WriteFail("Not supported");

//     // If no arguments, return the same thing as a getvar. Note that we get the
//     // HAL first so we can return "not supported" before we return the less
//     // specific error message below.
//     if (args.size() < 2 || args[1].empty()) {
//         std::string message;
//         if (!GetSnapshotUpdateStatus(device, {}, &message)) {
//             return device->WriteFail("Could not determine update status");
//         }
//         device->WriteInfo(message);
//         return device->WriteOkay("");
//     }

//     MergeStatus status = hal->getSnapshotMergeStatus();

//     if (args.size() != 2) {
//         return device->WriteFail("Invalid arguments");
//     }
//     if (args[1] == "cancel") {
//         switch (status) {
//             case MergeStatus::SNAPSHOTTED:
//             case MergeStatus::MERGING: {
//                 const auto ret = hal->SetSnapshotMergeStatus(MergeStatus::CANCELLED);
//                 if (!ret.success) {
//                     device->WriteFail("Failed to SetSnapshotMergeStatus(MergeStatus::CANCELLED) " +
//                                       ret.errMsg);
//                 }
//                 break;
//             }
//             default:
//                 break;
//         }
//     } else if (args[1] == "merge") {
//         if (status != MergeStatus::MERGING) {
//             return device->WriteFail("No snapshot merge is in progress");
//         }

//         auto sm = SnapshotManager::New();
//         if (!sm) {
//             return device->WriteFail("Unable to create SnapshotManager");
//         }
//         if (!sm->FinishMergeInRecovery()) {
//             return device->WriteFail("Unable to finish snapshot merge");
//         }
//     } else {
//         return device->WriteFail("Invalid parameter to snapshot-update");
//     }
//     return device->WriteStatus(FastbootResult::OKAY, "Success");
// }

namespace {
// Helper of FetchHandler.
class PartitionFetcher {
  public:
    static bool Fetch(FastbootDevice* device, const std::vector<std::string>& args) {
        if constexpr (!kEnableFetch) {
            return device->WriteFail("Fetch is not allowed on user build");
        }

        if (GetDeviceLockStatus()) {
            return device->WriteFail("Fetch is not allowed on locked devices");
        }

        PartitionFetcher fetcher(device, args);
        if (fetcher.Open()) {
            fetcher.Fetch();
        }
        CHECK(fetcher.ret_.has_value());
        return *fetcher.ret_;
    }

  private:
    PartitionFetcher(FastbootDevice* device, const std::vector<std::string>& args)
        : device_(device), args_(&args) {}
    // Return whether the partition is successfully opened.
    // If successfully opened, ret_ is left untouched. Otherwise, ret_ is set to the value
    // that FetchHandler should return.
    bool Open() {
        if (args_->size() < 2) {
            ret_ = device_->WriteFail("Missing partition arg");
            return false;
        }

        partition_name_ = args_->at(1);
        if (std::find(kAllowedPartitions.begin(), kAllowedPartitions.end(), partition_name_) ==
            kAllowedPartitions.end()) {
            ret_ = device_->WriteFail("Fetch is only allowed on [" +
                                      android::base::Join(kAllowedPartitions, ", ") + "]");
            return false;
        }

        if (!OpenPartition(device_, partition_name_, &handle_, O_RDONLY)) {
            ret_ = device_->WriteFail(
                    android::base::StringPrintf("Cannot open %s", partition_name_.c_str()));
            return false;
        }

        partition_size_ = android::get_block_device_size(handle_.fd());
        if (partition_size_ == 0) {
            ret_ = device_->WriteOkay(android::base::StringPrintf("Partition %s has size 0",
                                                                  partition_name_.c_str()));
            return false;
        }

        start_offset_ = 0;
        if (args_->size() >= 3) {
            if (!android::base::ParseUint(args_->at(2), &start_offset_)) {
                ret_ = device_->WriteFail("Invalid offset, must be integer");
                return false;
            }
            if (start_offset_ > std::numeric_limits<off64_t>::max()) {
                ret_ = device_->WriteFail(
                        android::base::StringPrintf("Offset overflows: %" PRIx64, start_offset_));
                return false;
            }
        }
        if (start_offset_ > partition_size_) {
            ret_ = device_->WriteFail(android::base::StringPrintf(
                    "Invalid offset 0x%" PRIx64 ", partition %s has size 0x%" PRIx64, start_offset_,
                    partition_name_.c_str(), partition_size_));
            return false;
        }
        uint64_t maximum_total_size_to_read = partition_size_ - start_offset_;
        total_size_to_read_ = maximum_total_size_to_read;
        if (args_->size() >= 4) {
            if (!android::base::ParseUint(args_->at(3), &total_size_to_read_)) {
                ret_ = device_->WriteStatus(FastbootResult::FAIL, "Invalid size, must be integer");
                return false;
            }
        }
        if (total_size_to_read_ == 0) {
            ret_ = device_->WriteOkay("Read 0 bytes");
            return false;
        }
        if (total_size_to_read_ > maximum_total_size_to_read) {
            ret_ = device_->WriteFail(android::base::StringPrintf(
                    "Invalid size to read 0x%" PRIx64 ", partition %s has size 0x%" PRIx64
                    " and fetching from offset 0x%" PRIx64,
                    total_size_to_read_, partition_name_.c_str(), partition_size_, start_offset_));
            return false;
        }

        if (total_size_to_read_ > kMaxFetchSizeDefault) {
            ret_ = device_->WriteFail(android::base::StringPrintf(
                    "Cannot fetch 0x%" PRIx64
                    " bytes because it exceeds maximum transport size 0x%x",
                    partition_size_, kMaxDownloadSizeDefault));
            return false;
        }

        return true;
    }

    // Assume Open() returns true.
    // After execution, ret_ is set to the value that FetchHandler should return.
    void Fetch() {
        CHECK(start_offset_ <= std::numeric_limits<off64_t>::max());
        if (lseek64(handle_.fd(), start_offset_, SEEK_SET) != static_cast<off64_t>(start_offset_)) {
            ret_ = device_->WriteFail(android::base::StringPrintf(
                    "On partition %s, unable to lseek(0x%" PRIx64 ": %s", partition_name_.c_str(),
                    start_offset_, strerror(errno)));
            return;
        }

        if (!device_->WriteStatus(FastbootResult::DATA,
                                  android::base::StringPrintf(
                                          "%08x", static_cast<uint32_t>(total_size_to_read_)))) {
            ret_ = false;
            return;
        }
        uint64_t end_offset = start_offset_ + total_size_to_read_;
        std::vector<char> buf(1_MiB);
        uint64_t current_offset = start_offset_;
        while (current_offset < end_offset) {
            // On any error, exit. We can't return a status message to the driver because
            // we are in the middle of writing data, so just let the driver guess what's wrong
            // by ending the data stream prematurely.
            uint64_t remaining = end_offset - current_offset;
            uint64_t chunk_size = std::min<uint64_t>(buf.size(), remaining);
            if (!android::base::ReadFully(handle_.fd(), buf.data(), chunk_size)) {
                PLOG(ERROR) << std::hex << "Unable to read 0x" << chunk_size << " bytes from "
                            << partition_name_ << " @ offset 0x" << current_offset;
                ret_ = false;
                return;
            }
            if (!device_->HandleData(false /* is read */, buf.data(), chunk_size)) {
                PLOG(ERROR) << std::hex << "Unable to send 0x" << chunk_size << " bytes of "
                            << partition_name_ << " @ offset 0x" << current_offset;
                ret_ = false;
                return;
            }
            current_offset += chunk_size;
        }

        ret_ = device_->WriteOkay(android::base::StringPrintf(
                "Fetched %s (offset=0x%" PRIx64 ", size=0x%" PRIx64, partition_name_.c_str(),
                start_offset_, total_size_to_read_));
    }

    static constexpr std::array<const char*, 3> kAllowedPartitions{
            "vendor_boot",
            "vendor_boot_a",
            "vendor_boot_b",
    };

    FastbootDevice* device_;
    const std::vector<std::string>* args_ = nullptr;
    std::string partition_name_;
    PartitionHandle handle_;
    uint64_t partition_size_ = 0;
    uint64_t start_offset_ = 0;
    uint64_t total_size_to_read_ = 0;

    // What FetchHandler should return.
    std::optional<bool> ret_ = std::nullopt;
};
}  // namespace

bool FetchHandler(FastbootDevice* device, const std::vector<std::string>& args) {
    return PartitionFetcher::Fetch(device, args);
}
