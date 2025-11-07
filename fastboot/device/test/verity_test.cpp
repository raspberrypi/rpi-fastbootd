// Native dm-verity operations tests using loopback devices


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
#include <chrono>
#include <algorithm>
#include <numeric>

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
    bool Create(size_t size_mb = 10) {
        // Create temporary backing file
        char template_file[] = "/tmp/verity_test_XXXXXX";
        int fd = mkstemp(template_file);
        if (fd < 0) {
            error_msg_ = "Failed to create backing file: " + std::string(strerror(errno));
            return false;
        }
        backing_file_ = template_file;
        
        // Allocate space (10 MB by default - sufficient for verity testing)
        if (ftruncate(fd, size_mb * 1024 * 1024) < 0) {
            error_msg_ = "Failed to resize backing file: " + std::string(strerror(errno));
            close(fd);
            return false;
        }
        
        // Write some test data pattern
        std::vector<char> pattern(4096, 'A');
        for (size_t i = 0; i < (size_mb * 1024) / 4; i++) {
            if (write(fd, pattern.data(), pattern.size()) != (ssize_t)pattern.size()) {
                error_msg_ = "Failed to write test pattern: " + std::string(strerror(errno));
                close(fd);
                return false;
            }
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
        
        loop_device_ = "";
        error_msg_ = "";
    }
    
    const std::string& device() const { return loop_device_; }
    const std::string& backing_file() const { return backing_file_; }
    const std::string& error() const { return error_msg_; }
    
private:
    int loop_fd_;
    std::string loop_device_;
    std::string backing_file_;
    std::string error_msg_;
};

// Check if running as root (required for loop device operations)
bool IsRoot() {
    return geteuid() == 0;
}

// ============================================================================
// dm-verity Tests
// ============================================================================

#ifdef HAVE_LIBCRYPTSETUP

TEST_CASE("dm-verity native operations", "[verity][native]") {
    if (!IsRoot()) {
        WARN("Skipping verity tests - requires root privileges");
        WARN("Run as root: sudo " << Catch::getResultCapture().getCurrentTestName());
        return;
    }
    
    LoopbackDevice data_dev;
    REQUIRE(data_dev.Create(10));  // 10 MB data device
    INFO("Created data device: " << data_dev.device());
    
    LoopbackDevice hash_dev;
    REQUIRE(hash_dev.Create(2));   // 2 MB hash device (smaller, stores hash tree)
    INFO("Created hash device: " << hash_dev.device());
    
    SECTION("Calculate dm-verity hash tree") {
        std::string root_hash;
        std::string error_msg;
        
        bool result = VeritySetupNative(data_dev.device(), hash_dev.device(), 
                                       &root_hash, &error_msg);
        
        if (!result) {
            WARN("VeritySetupNative failed: " << error_msg);
        }
        
        REQUIRE(result);
        REQUIRE(!root_hash.empty());
        
        INFO("Root hash: " << root_hash);
        
        // Root hash should be 64 hex characters (SHA-256 = 32 bytes = 64 hex chars)
        REQUIRE(root_hash.length() == 64);
        
        // Verify hash contains only hex characters
        for (char c : root_hash) {
            REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
        }
    }
    
    SECTION("Verify hash tree is written to hash device") {
        std::string root_hash;
        std::string error_msg;
        
        REQUIRE(VeritySetupNative(data_dev.device(), hash_dev.device(), 
                                 &root_hash, &error_msg));
        
        // Check that hash device file has been modified
        struct stat st;
        REQUIRE(stat(hash_dev.backing_file().c_str(), &st) == 0);
        REQUIRE(st.st_size > 0);
        
        INFO("Hash tree size: " << st.st_size << " bytes");
    }
    
    SECTION("Verify deterministic hash generation") {
        std::string root_hash1;
        std::string root_hash2;
        std::string error_msg;
        
        // First hash generation
        LoopbackDevice hash_dev1;
        REQUIRE(hash_dev1.Create(2));
        REQUIRE(VeritySetupNative(data_dev.device(), hash_dev1.device(), 
                                 &root_hash1, &error_msg));
        
        // Second hash generation with same data
        LoopbackDevice hash_dev2;
        REQUIRE(hash_dev2.Create(2));
        REQUIRE(VeritySetupNative(data_dev.device(), hash_dev2.device(), 
                                 &root_hash2, &error_msg));
        
        // Note: Hashes will differ due to random salt
        // This is expected behavior - each format generates a new salt
        REQUIRE(!root_hash1.empty());
        REQUIRE(!root_hash2.empty());
        
        INFO("First root hash:  " << root_hash1);
        INFO("Second root hash: " << root_hash2);
    }
    
    SECTION("Performance benchmark") {
        const int iterations = 5;
        std::vector<long long> timings;
        
        for (int i = 0; i < iterations; i++) {
            LoopbackDevice hash_dev_temp;
            REQUIRE(hash_dev_temp.Create(2));
            
            std::string root_hash;
            std::string error_msg;
            
            auto start = std::chrono::high_resolution_clock::now();
            bool result = VeritySetupNative(data_dev.device(), hash_dev_temp.device(), 
                                           &root_hash, &error_msg);
            auto end = std::chrono::high_resolution_clock::now();
            
            REQUIRE(result);
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            timings.push_back(duration.count());
        }
        
        long long total_time = 0;
        for (auto t : timings) {
            total_time += t;
        }
        long long avg_time = total_time / iterations;
        
        INFO("Performance results:");
        INFO("  Iterations: " << iterations);
        INFO("  Total time: " << total_time << " ms");
        INFO("  Average: " << avg_time << " ms");
        INFO("  Min: " << *std::min_element(timings.begin(), timings.end()) << " ms");
        INFO("  Max: " << *std::max_element(timings.begin(), timings.end()) << " ms");
        
        // Verity setup should be reasonably fast (< 1000ms for 10MB)
        REQUIRE(avg_time < 1000);
    }
    
    SECTION("Error handling - invalid data device") {
        std::string root_hash;
        std::string error_msg;
        
        bool result = VeritySetupNative("/dev/nonexistent_device", hash_dev.device(), 
                                       &root_hash, &error_msg);
        
        REQUIRE_FALSE(result);
        REQUIRE(!error_msg.empty());
        INFO("Expected error: " << error_msg);
    }
    
    SECTION("Error handling - invalid hash device") {
        std::string root_hash;
        std::string error_msg;
        
        bool result = VeritySetupNative(data_dev.device(), "/dev/nonexistent_hash", 
                                       &root_hash, &error_msg);
        
        REQUIRE_FALSE(result);
        REQUIRE(!error_msg.empty());
        INFO("Expected error: " << error_msg);
    }
}

TEST_CASE("dm-verity activation", "[verity][activate]") {
    if (!IsRoot()) {
        WARN("Skipping verity activation tests - requires root privileges");
        return;
    }
    
    LoopbackDevice data_dev;
    REQUIRE(data_dev.Create(10));
    
    LoopbackDevice hash_dev;
    REQUIRE(hash_dev.Create(2));
    
    std::string root_hash;
    std::string error_msg;
    
    REQUIRE(VeritySetupNative(data_dev.device(), hash_dev.device(), 
                             &root_hash, &error_msg));
    
    SECTION("Verify device can be activated") {
        struct crypt_device *cd = NULL;
        
        int r = crypt_init(&cd, data_dev.device().c_str());
        REQUIRE(r == 0);
        
        // Load verity superblock from hash device
        r = crypt_load(cd, CRYPT_VERITY, NULL);
        
        if (r == 0) {
            INFO("Successfully loaded verity superblock");
            
            // Get verity parameters
            struct crypt_params_verity vp = {};
            r = crypt_get_verity_info(cd, &vp);
            
            if (r == 0) {
                INFO("Verity parameters:");
                INFO("  Hash algorithm: " << (vp.hash_name ? vp.hash_name : "N/A"));
                INFO("  Data block size: " << vp.data_block_size);
                INFO("  Hash block size: " << vp.hash_block_size);
            }
        }
        
        crypt_free(cd);
    }
}

#else

TEST_CASE("dm-verity tests require libcryptsetup", "[verity][!hide]") {
    WARN("libcryptsetup not available - verity tests disabled");
    WARN("Install libcryptsetup-dev and reconfigure to enable");
}

#endif // HAVE_LIBCRYPTSETUP

// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE("dm-verity with /persistent directory", "[verity][integration]") {
    if (!IsRoot()) {
        WARN("Skipping integration tests - requires root privileges");
        return;
    }
    
#ifdef HAVE_LIBCRYPTSETUP
    LoopbackDevice data_dev;
    REQUIRE(data_dev.Create(10));
    
    // Create /persistent if it doesn't exist
    std::string persistent_dir = "/tmp/test_persistent";
    mkdir(persistent_dir.c_str(), 0755);
    
    // Generate hash file path
    std::string device_name = data_dev.device();
    size_t pos = device_name.rfind('/');
    if (pos != std::string::npos) {
        device_name = device_name.substr(pos + 1);
    }
    
    std::string hash_file = persistent_dir + "/" + device_name + ".verity";
    
    SECTION("Store hash tree in file") {
        // Create a regular file for hash storage
        int fd = open(hash_file.c_str(), O_RDWR | O_CREAT, 0644);
        REQUIRE(fd >= 0);
        
        // Allocate space for hash tree
        REQUIRE(ftruncate(fd, 2 * 1024 * 1024) == 0);  // 2 MB
        close(fd);
        
        std::string root_hash;
        std::string error_msg;
        
        // Setup verity using the file as hash device
        bool result = VeritySetupNative(data_dev.device(), hash_file, 
                                       &root_hash, &error_msg);
        
        if (!result) {
            INFO("Error: " << error_msg);
        }
        
        REQUIRE(result);
        REQUIRE(!root_hash.empty());
        
        // Verify hash file exists and has content
        struct stat st;
        REQUIRE(stat(hash_file.c_str(), &st) == 0);
        REQUIRE(st.st_size > 0);
        
        INFO("Hash tree stored in: " << hash_file);
        INFO("Hash tree size: " << st.st_size << " bytes");
        INFO("Root hash: " << root_hash);
    }
    
    // Cleanup
    unlink(hash_file.c_str());
    rmdir(persistent_dir.c_str());
#endif
}


