// Native LUKS crypto operations tests using loopback devices


#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <fstream>
#include <string>
#include <cstring>
#include <errno.h>
#include <vector>
#include <algorithm>
#include <chrono>

#ifdef HAVE_LIBCRYPTSETUP
#include <libcryptsetup.h>
#include "../crypto_native.h"
#endif

// Helper class to manage loopback devices for testing
class LoopbackDevice {
public:
    LoopbackDevice() : loop_fd_(-1), loop_device_(""), backing_file_("") {}
    
    ~LoopbackDevice() {
        Cleanup();
    }
    
    // Create a loopback device backed by a temporary file
    bool Create(size_t size_mb = 32) {
        // Create temporary backing file
        char template_file[] = "/tmp/luks_test_XXXXXX";
        int fd = mkstemp(template_file);
        if (fd < 0) {
            error_msg_ = "Failed to create backing file: " + std::string(strerror(errno));
            return false;
        }
        backing_file_ = template_file;
        
        // Allocate space (32 MB by default - sufficient for LUKS2)
        if (ftruncate(fd, size_mb * 1024 * 1024) < 0) {
            error_msg_ = "Failed to resize backing file: " + std::string(strerror(errno));
            close(fd);
            return false;
        }
        close(fd);
        
        // Find available loop device
        int control_fd = open("/dev/loop-control", O_RDWR);
        if (control_fd < 0) {
            error_msg_ = "Failed to open loop-control: " + std::string(strerror(errno));
            return false;
        }
        
        int loop_num = ioctl(control_fd, LOOP_CTL_GET_FREE);
        close(control_fd);
        
        if (loop_num < 0) {
            error_msg_ = "Failed to get free loop device: " + std::string(strerror(errno));
            return false;
        }
        
        loop_device_ = "/dev/loop" + std::to_string(loop_num);
        
        // Open loop device
        loop_fd_ = open(loop_device_.c_str(), O_RDWR);
        if (loop_fd_ < 0) {
            error_msg_ = "Failed to open loop device: " + std::string(strerror(errno));
            return false;
        }
        
        // Open backing file
        int backing_fd = open(backing_file_.c_str(), O_RDWR);
        if (backing_fd < 0) {
            error_msg_ = "Failed to open backing file: " + std::string(strerror(errno));
            return false;
        }
        
        // Associate loop device with backing file
        if (ioctl(loop_fd_, LOOP_SET_FD, backing_fd) < 0) {
            error_msg_ = "Failed to set loop device: " + std::string(strerror(errno));
            close(backing_fd);
            return false;
        }
        close(backing_fd);
        
        return true;
    }
    
    void Cleanup() {
        if (loop_fd_ >= 0) {
            // Detach loop device
            ioctl(loop_fd_, LOOP_CLR_FD);
            close(loop_fd_);
            loop_fd_ = -1;
        }
        
        if (!backing_file_.empty()) {
            unlink(backing_file_.c_str());
            backing_file_ = "";
        }
    }
    
    std::string GetDevice() const { return loop_device_; }
    std::string GetError() const { return error_msg_; }
    bool IsValid() const { return loop_fd_ >= 0 && !loop_device_.empty(); }
    
private:
    int loop_fd_;
    std::string loop_device_;
    std::string backing_file_;
    std::string error_msg_;
};

// Helper to clean up device mapper devices
void CleanupDeviceMapper(const std::string& name) {
#ifdef HAVE_LIBCRYPTSETUP
    struct crypt_device *cd = NULL;
    if (crypt_init_by_name(&cd, name.c_str()) >= 0) {
        crypt_deactivate(cd, name.c_str());
        crypt_free(cd);
    }
#endif
}

TEST_CASE("LUKS Crypto Operations", "[crypto][luks]") {
    // Check if running with sufficient privileges
    if (getuid() != 0) {
        WARN("Skipping crypto tests (requires root privileges for loopback devices)");
        return;
    }
    
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available, skipping native crypto tests");
    return;
#else
    LoopbackDevice loop;
    REQUIRE(loop.Create());
    INFO("Created loopback device: " << loop.GetDevice());
    
    const std::string test_key = "test_hardware_key_12345678901234567890";
    const std::string test_label = "test_luks_label";
    const std::string test_cipher = "aes-xts-plain64";
    const std::string mapped_name = "test_luks_device";
    std::string error_msg;
    
    SECTION("LUKS Format and Open") {
        INFO("Formatting LUKS device: " << loop.GetDevice());
        
        // Format the device
        bool format_result = CryptInitNative(loop.GetDevice(), test_label, test_cipher, 
                                            test_key, &error_msg);
        if (!format_result) {
            WARN("Format failed: " << error_msg);
        }
        REQUIRE(format_result);
        
        // Verify LUKS header was created
        struct crypt_device *cd = NULL;
        int r = crypt_init(&cd, loop.GetDevice().c_str());
        REQUIRE(r >= 0);
        
        r = crypt_load(cd, CRYPT_LUKS, NULL);
        REQUIRE(r >= 0);
        
        // Check label
        const char* label = crypt_get_label(cd);
        REQUIRE(label != nullptr);
        REQUIRE(std::string(label) == test_label);
        
        crypt_free(cd);
        
        // Open the device
        bool open_result = CryptOpenNative(loop.GetDevice(), mapped_name,
                                          test_key, &error_msg);
        if (!open_result) {
            WARN("Open failed: " << error_msg);
        }
        REQUIRE(open_result);
        
        // Verify device mapper device exists
        std::string dm_device = "/dev/mapper/" + mapped_name;
        struct stat st;
        REQUIRE(stat(dm_device.c_str(), &st) == 0);
        
        // Cleanup
        CleanupDeviceMapper(mapped_name);
    }
    
    SECTION("Key Slot Management - Add User Passphrase") {
        // First format the device
        REQUIRE(CryptInitNative(loop.GetDevice(), test_label, test_cipher,
                               test_key, &error_msg));
        
        // Add user passphrase to slot 1
        const std::string user_pass = "user_password_123";
        bool result = CryptSetPasswordNative(loop.GetDevice(), test_key, 
                                            user_pass, false, &error_msg);
        if (!result) {
            WARN("Add passphrase failed: " << error_msg);
        }
        REQUIRE(result);
        
        // Verify both keys work
        struct crypt_device *cd = NULL;
        REQUIRE(crypt_init(&cd, loop.GetDevice().c_str()) >= 0);
        REQUIRE(crypt_load(cd, CRYPT_LUKS, NULL) >= 0);
        
        // Test hardware key (slot 0)
        int hw_slot = crypt_activate_by_passphrase(cd, NULL, CRYPT_ANY_SLOT,
                                                    test_key.c_str(), test_key.size(), 0);
        REQUIRE(hw_slot == 0);
        
        // Test user passphrase (slot 1)
        int user_slot = crypt_activate_by_passphrase(cd, NULL, CRYPT_ANY_SLOT,
                                                     user_pass.c_str(), user_pass.size(), 0);
        REQUIRE(user_slot == 1);
        
        crypt_free(cd);
    }
    
    SECTION("Key Slot Management - Remove User Passphrase") {
        // Format and add user passphrase
        REQUIRE(CryptInitNative(loop.GetDevice(), test_label, test_cipher,
                               test_key, &error_msg));
        
        const std::string user_pass = "user_password_456";
        REQUIRE(CryptSetPasswordNative(loop.GetDevice(), test_key, 
                                      user_pass, false, &error_msg));
        
        // Remove user passphrase
        bool result = CryptSetPasswordNative(loop.GetDevice(), test_key,
                                            "", true, &error_msg);
        if (!result) {
            WARN("Remove passphrase failed: " << error_msg);
        }
        REQUIRE(result);
        
        // Verify user passphrase no longer works
        struct crypt_device *cd = NULL;
        REQUIRE(crypt_init(&cd, loop.GetDevice().c_str()) >= 0);
        REQUIRE(crypt_load(cd, CRYPT_LUKS, NULL) >= 0);
        
        // Hardware key should still work
        int hw_slot = crypt_activate_by_passphrase(cd, NULL, CRYPT_ANY_SLOT,
                                                    test_key.c_str(), test_key.size(), 0);
        REQUIRE(hw_slot == 0);
        
        // User passphrase should fail
        int user_slot = crypt_activate_by_passphrase(cd, NULL, CRYPT_ANY_SLOT,
                                                     user_pass.c_str(), user_pass.size(), 0);
        REQUIRE(user_slot < 0);
        
        crypt_free(cd);
    }
    
    SECTION("Error Handling - Invalid Device") {
        bool result = CryptInitNative("/dev/nonexistent", test_label, test_cipher,
                                     test_key, &error_msg);
        REQUIRE_FALSE(result);
        REQUIRE(!error_msg.empty());
    }
    
    SECTION("Error Handling - Invalid Key") {
        REQUIRE(CryptInitNative(loop.GetDevice(), test_label, test_cipher,
                               test_key, &error_msg));
        
        // Try to open with wrong key
        std::string wrong_key = "wrong_key_1234567890";
        bool result = CryptOpenNative(loop.GetDevice(), mapped_name,
                                     wrong_key, &error_msg);
        REQUIRE_FALSE(result);
        REQUIRE(!error_msg.empty());
    }
    
    SECTION("End-to-End - Full Lifecycle") {
        // 1. Format device
        REQUIRE(CryptInitNative(loop.GetDevice(), test_label, test_cipher,
                               test_key, &error_msg));
        
        // 2. Add user passphrase
        const std::string user_pass = "lifecycle_test_pass";
        REQUIRE(CryptSetPasswordNative(loop.GetDevice(), test_key,
                                      user_pass, false, &error_msg));
        
        // 3. Open device with hardware key
        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name,
                               test_key, &error_msg));
        
        std::string dm_device = "/dev/mapper/" + mapped_name;
        struct stat st;
        REQUIRE(stat(dm_device.c_str(), &st) == 0);
        
        // 4. Close device
        CleanupDeviceMapper(mapped_name);
        REQUIRE(stat(dm_device.c_str(), &st) < 0);
        
        // 5. Reopen with user passphrase
        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name,
                               user_pass, &error_msg));
        REQUIRE(stat(dm_device.c_str(), &st) == 0);
        
        // 6. Cleanup
        CleanupDeviceMapper(mapped_name);
        
        // 7. Remove user passphrase
        REQUIRE(CryptSetPasswordNative(loop.GetDevice(), test_key,
                                      "", true, &error_msg));
        
        // 8. Verify user passphrase no longer works
        REQUIRE_FALSE(CryptOpenNative(loop.GetDevice(), mapped_name,
                                     user_pass, &error_msg));
    }
#endif
}

TEST_CASE("LUKS Performance", "[crypto][performance]") {
    if (getuid() != 0) {
        WARN("Skipping performance test (requires root)");
        return;
    }
    
#ifdef HAVE_LIBCRYPTSETUP
    LoopbackDevice loop;
    if (!loop.Create()) {
        WARN("Failed to create loopback device: " << loop.GetError());
        return;
    }
    
    const std::string test_key = "perf_key_1234567890123456";
    const std::string test_label = "perf_label";
    const std::string test_cipher = "aes-xts-plain64";
    std::string error_msg;
    
    SECTION("Format performance") {
        auto start = std::chrono::high_resolution_clock::now();
        bool result = CryptInitNative(loop.GetDevice(), test_label, test_cipher,
                                     test_key, &error_msg);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        REQUIRE(result);
        INFO("LUKS format took " << duration.count() << "ms");
    }
#endif
}

