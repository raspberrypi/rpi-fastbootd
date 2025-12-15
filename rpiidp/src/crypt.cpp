#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include "idpcrypt.h"
#include "utility.h"

// Exception thrown when secure keyfile generation fails
class SecureKeyfileError : public std::runtime_error {
public:
    explicit SecureKeyfileError(const std::string& message) : std::runtime_error(message) {}
};

// RAII wrapper for secure keyfiles that automatically cleans up on destruction
// Every constructed SecureKeyfile object is guaranteed to be valid
class SecureKeyfile {
public:
    // Constructor that generates keyfile from block device
    // Throws SecureKeyfileError on failure - no invalid objects possible
    explicit SecureKeyfile(std::string_view blkdev);

    // Non-copyable, non-assignable, but movable
    SecureKeyfile(const SecureKeyfile&) = delete;
    SecureKeyfile& operator=(const SecureKeyfile&) = delete;
    SecureKeyfile& operator=(SecureKeyfile&&) = delete;

    SecureKeyfile(SecureKeyfile&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    ~SecureKeyfile() {
        // Check needed: path() returns reference (could be modified) and move ctor clears path
        if (!path_.empty()) {
            std::remove(path_.c_str());
        }
    }

    // Get the path - always valid since object construction guarantees validity
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;

    // Default constructor deleted - no empty/invalid objects allowed
    SecureKeyfile() = delete;

    // Constructor from existing path (for internal use only)
    explicit SecureKeyfile(std::filesystem::path path) : path_(std::move(path)) {}

    // Private helper methods for keyfile generation
    static std::string getDeviceId(const std::string& block_device);
    static std::string readSysFile(const std::string& path);
    static bool pathExists(const std::string& path);
};

// Define to redirect cryptsetup stdout to sdterr
#ifdef OUT2ERR
#include <unistd.h>
#endif


#define ERR(msg) std::cerr << "IDP: ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl
#define MSG(msg) std::cout << "IDP: " << msg << std::endl


namespace cryptsetup{

   // Translate vector to args and invoke cryptsetup
   bool run(std::vector<std::string> args, std::string str)
   {
      std::vector<char*> argv = utils::to_execvp_argv(args);

#if 0
      // Dump all args
      for (size_t i = 0; i < argv.size() && argv[i] != nullptr; ++i) {
         std::cout << "argv[" << i << "]: " << argv[i] << std::endl;
      }
#endif

      int rc = -1;
#ifdef OUT2ERR
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
      int ret = utils::process_spawn_blocking(&rc, argv[0], argv.data(), nullptr, &actions);
      posix_spawn_file_actions_destroy(&actions);
#else
      int ret = utils::process_spawn_blocking(&rc, argv[0], argv.data(), nullptr, nullptr);
#endif

      if (ret || rc) {
         std::string msg = (std::string(argv[0]) +
               " [" +
               str +
               "] " +
               "exit " +
               std::to_string(rc) +
               " ret " +
               std::to_string(ret)); ERR(msg);
      }

      return (ret == 0) && (rc == 0) ? true : false;
   }
}


bool IDPluks::Create(std::string_view blkdev, std::optional<std::string> userkey) const
{
   std::string msg = ("Creating " +
         summary() +
         " on " +
         std::string(blkdev)); MSG(msg);

   if (userkey) {
      ERR("User-provided key for LUKS container creation is not supported yet");
      return false;
   }

   if (version != 2) {
      ERR("Unsupported LUKS version: " << version);
      return false;
   }

   // Generate secure keyfile using HMAC from firmware crypto
   try {
      SecureKeyfile keyfile(blkdev);
      // RAII ensures keyfile is automatically cleaned up when it goes out of scope
      return cryptsetup::run(luks2createcmd(blkdev, keyfile.path()), "create");
   } catch (const SecureKeyfileError& e) {
      ERR("Failed to generate secure keyfile: " << e.what());
      return false;
   }
}


bool IDPluks::Open(std::string_view blkdev) const
{
   std::string msg = ("Opening " +
         std::string(mname) +
         " on " +
         summary() +
         " [" + std::string(blkdev) + "]"); MSG(msg);

   // Generate secure keyfile using HMAC from firmware crypto
   try {
      SecureKeyfile keyfile(blkdev);
      // RAII ensures keyfile is automatically cleaned up when it goes out of scope
      return cryptsetup::run(luks2opencmd(blkdev, keyfile.path()), "open");
   } catch (const SecureKeyfileError& e) {
      ERR("Failed to generate secure keyfile: " << e.what());
      return false;
   }
}


bool IDPluks::Close(std::optional<std::string_view> blkdev) const
{
   std::string_view dev = blkdev.value_or(mname);
   std::string msg = ("Closing " + std::string(dev)); MSG(msg);

   return cryptsetup::run(luks2closecmd(dev), "close");
}


// SecureKeyfile implementation

// Helper to read a file and return its contents as a string
std::string SecureKeyfile::readSysFile(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Helper to check if a path exists
bool SecureKeyfile::pathExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

// Get unique device ID from sysfs (based on getDeviceId from commands.cpp)
std::string SecureKeyfile::getDeviceId(const std::string& block_device) {
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

SecureKeyfile::SecureKeyfile(std::string_view blkdev)
{
   // Get unique device ID from sysfs
   std::string device_id = getDeviceId(std::string(blkdev));
   if (device_id.empty()) {
      throw SecureKeyfileError("Could not determine device ID for " + std::string(blkdev));
   }

   if (device_id == "ERROR_USB_NOT_SUPPORTED") {
      throw SecureKeyfileError("USB devices not supported for LUKS encryption");
   }

   // Create a unique temporary filename for the device ID
   std::string blkdev_str(blkdev);
   std::replace(blkdev_str.begin(), blkdev_str.end(), '/', '_');

   std::filesystem::path device_id_file = "/tmp/idp_device_id_" + blkdev_str + ".tmp";
   std::filesystem::path keyfile = "/tmp/idp_key_" + blkdev_str + ".tmp";

   // Write device ID to temporary file
   std::ofstream id_file(device_id_file);
   if (!id_file) {
      throw SecureKeyfileError("Could not create device ID file: " + device_id_file.string());
   }
   id_file << device_id;
   id_file.close();

   // Use rpi-fw-crypto to generate HMAC with key slot 1
   // Using hex output format for consistency with OEM commands
   std::vector<std::string> args = {
      "rpi-fw-crypto",
      "hmac",
      "--in", device_id_file.string(),
      "--key-id", "1",
      "--out", keyfile.string(),
      "--outform", "hex"
   };

   // Create temporary file to capture stderr
   std::filesystem::path stderr_file = "/tmp/idp_rpi_fw_crypto_stderr_" + blkdev_str + ".tmp";

   // Set up file actions to redirect stderr
   posix_spawn_file_actions_t file_actions;
   posix_spawn_file_actions_init(&file_actions);
   posix_spawn_file_actions_addopen(&file_actions, STDERR_FILENO, stderr_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);

   std::vector<char*> argv = utils::to_execvp_argv(args);
   int rc = -1;
   int ret = utils::process_spawn_blocking(&rc, argv[0], argv.data(), nullptr, &file_actions);

   posix_spawn_file_actions_destroy(&file_actions);

   // Clean up device ID file
   std::remove(device_id_file.c_str());

   if (ret || rc) {
      // Read stderr content for better error reporting
      std::string error_msg = "rpi-fw-crypto failed: exit " + std::to_string(rc) + " ret " + std::to_string(ret);
      std::string stderr_content = readSysFile(stderr_file.string());
      if (!stderr_content.empty()) {
         // Remove trailing newlines for cleaner error messages
         while (!stderr_content.empty() && (stderr_content.back() == '\n' || stderr_content.back() == '\r')) {
            stderr_content.pop_back();
         }
         error_msg += " - " + stderr_content;
      }

      // Clean up stderr file
      std::remove(stderr_file.c_str());

      throw SecureKeyfileError(error_msg);
   }

   // Clean up stderr file (success case)
   std::remove(stderr_file.c_str());

   // Verify keyfile was created
   if (!pathExists(keyfile.string())) {
      throw SecureKeyfileError("HMAC keyfile was not created: " + keyfile.string());
   }

   // Remove trailing newline from rpi-fw-crypto output
   // HMAC-SHA256 hex output should be exactly 64 characters
   if (truncate(keyfile.c_str(), 64) != 0) {
      throw SecureKeyfileError("Could not truncate keyfile to remove trailing newline: " + keyfile.string());
   }

   // Success - store the keyfile path
   path_ = keyfile;

   std::string msg = ("Generated secure keyfile for " + std::string(blkdev) + " -> " + keyfile.string());
   MSG(msg);
}


// IDPluks implementation

std::vector<std::string>  IDPluks::luks2createcmd(std::string_view blkdev, std::filesystem::path keyfile) const
{
   std::vector<std::string> args;

   args.push_back("cryptsetup");
   //args.push_back("--debug");
   args.push_back("--batch-mode");
   args.push_back("luksFormat");
   args.push_back("--type");
   args.push_back("luks2");
   args.push_back("--force-password");

   if (key_size)
      args.push_back("--key-size=" + std::to_string(key_size));

   if (!hash.empty())
      args.push_back("--hash=" + hash);

   if (!cipher.empty())
      args.push_back("--cipher=" + cipher);

   if (sector_size)
      args.push_back("--sector-size=" + std::to_string(sector_size));

   args.push_back("--align-payload=" + std::to_string(pAlignmentBytes / sector_size));

   args.push_back("--key-file=" + keyfile.string());

   if (label && !label->empty())
      args.push_back("--label=" + *label);

   if (uuid && !uuid->empty())
      args.push_back("--uuid=" + *uuid);

   args.push_back(std::string(blkdev));

   return args;
}


std::vector<std::string>  IDPluks::luks2opencmd(std::string_view blkdev, std::filesystem::path keyfile) const
{
   std::vector<std::string> args;

   args.push_back("cryptsetup");
   args.push_back("luksOpen");
   args.push_back(std::string(blkdev));
   args.push_back(mname);
   args.push_back("--key-file=" + keyfile.string());

   return args;
}


std::vector<std::string>  IDPluks::luks2closecmd(std::string_view mname) const
{
   std::vector<std::string> args;

   args.push_back("cryptsetup");
   args.push_back("luksClose");
   args.push_back(std::string(mname));

   return args;
}
