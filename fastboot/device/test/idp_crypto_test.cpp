// IDP LUKS provisioning tests
// Verifies that the native libcryptsetup path supports the full range of
// IDPluks parameters that may appear in IDP v2 provisioning configs.
//
// IDP LUKS fields (from nav.cpp validateLUKS / createEncryptedPartition):
//   Required: key_size (int), cipher (string), hash (string), mname (string), etype (string)
//   Optional: label (string), uuid (string)
//   Set by parser: sector_size (from IGconf_device_sector_size: 512|4096)
//                  pAlignmentBytes (from image-palign-bytes)

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <string>
#include <cstring>
#include <errno.h>
#include <glob.h>

#ifdef HAVE_LIBCRYPTSETUP
#include <libcryptsetup.h>
#include "../crypto_native.h"
#endif

// All device-mapper names used by tests in this file
static const char* const kAllMapperNames[] = {
    "idp_test_cipher",
    "idp_test_hash",
    "idp_test_sector",
    "idp_test_etype",
    "idp_test_profile",
    "idp_test_legacy",
    "idp_close_test",
    "cryptdata",
    "crypt_a",
    "crypt_b",
};

// Clean up any leaked mapper devices and temp files from prior runs/crashes
static void CleanupAllTestResources() {
#ifdef HAVE_LIBCRYPTSETUP
    for (const auto* name : kAllMapperNames) {
        crypt_deactivate(NULL, name);
    }
#endif

    // Remove any orphaned backing files from crashed runs
    glob_t g = {};
    if (glob("/tmp/idp_luks_test_*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            unlink(g.gl_pathv[i]);
        }
        globfree(&g);
    }
}

// Catch2 listener that runs cleanup before each test case
struct TestCleanupListener : Catch::EventListenerBase {
    using EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        CleanupAllTestResources();
    }

    void testCaseEnded(Catch::TestCaseStats const&) override {
        CleanupAllTestResources();
    }
};
CATCH_REGISTER_LISTENER(TestCleanupListener)

// Helper class to manage loopback devices for testing
class LoopbackDevice {
public:
    LoopbackDevice() : loop_fd_(-1) {}

    ~LoopbackDevice() { Cleanup(); }

    bool Create(size_t size_mb = 32) {
        char template_file[] = "/tmp/idp_luks_test_XXXXXX";
        int fd = mkstemp(template_file);
        if (fd < 0) {
            error_msg_ = "Failed to create backing file: " + std::string(strerror(errno));
            return false;
        }
        backing_file_ = template_file;

        if (ftruncate(fd, size_mb * 1024 * 1024) < 0) {
            error_msg_ = "Failed to resize backing file: " + std::string(strerror(errno));
            close(fd);
            return false;
        }
        close(fd);

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
        loop_fd_ = open(loop_device_.c_str(), O_RDWR);
        if (loop_fd_ < 0) {
            error_msg_ = "Failed to open loop device: " + std::string(strerror(errno));
            return false;
        }

        int backing_fd = open(backing_file_.c_str(), O_RDWR);
        if (backing_fd < 0) {
            error_msg_ = "Failed to open backing file: " + std::string(strerror(errno));
            return false;
        }

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

private:
    int loop_fd_;
    std::string loop_device_;
    std::string backing_file_;
    std::string error_msg_;
};

#ifdef HAVE_LIBCRYPTSETUP
static void CleanupMapper(const std::string& name) {
    crypt_deactivate(NULL, name.c_str());
}

// Helper to verify LUKS2 header properties after formatting
struct LuksHeaderInfo {
    std::string cipher;
    std::string cipher_mode;
    std::string label;
    std::string uuid;
    int key_size;         // bytes
    uint32_t sector_size;
    uint64_t data_offset; // 512-byte sectors
};

static bool ReadLuksHeader(const std::string& device, LuksHeaderInfo& info) {
    struct crypt_device *cd = NULL;
    if (crypt_init(&cd, device.c_str()) < 0) return false;
    if (crypt_load(cd, CRYPT_LUKS2, NULL) < 0) { crypt_free(cd); return false; }

    const char* c = crypt_get_cipher(cd);
    const char* m = crypt_get_cipher_mode(cd);
    const char* l = crypt_get_label(cd);
    const char* u = crypt_get_uuid(cd);

    info.cipher = c ? c : "";
    info.cipher_mode = m ? m : "";
    info.label = l ? l : "";
    info.uuid = u ? u : "";
    info.key_size = crypt_get_volume_key_size(cd);
    info.sector_size = crypt_get_sector_size(cd);
    info.data_offset = crypt_get_data_offset(cd);

    crypt_free(cd);
    return true;
}
#endif


// ============================================================================
// IDP field: cipher (required string)
// Specifies the dm-crypt cipher, e.g. "aes-xts-plain64"
// ============================================================================

TEST_CASE("IDP cipher", "[idp][crypto][cipher]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string mapped_name = "idp_test_cipher";
    std::string error_msg;

    SECTION("aes-xts-plain64 (default)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "aes");
        CHECK(hdr.cipher_mode == "xts-plain64");

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }

    SECTION("aes-cbc-essiv:sha256") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-cbc-essiv:sha256";
        fmt.key_size_bits = 256;  // CBC uses 256-bit key

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "aes");
        CHECK(hdr.cipher_mode == "cbc-essiv:sha256");
        CHECK(hdr.key_size == 32);

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }

    SECTION("serpent-xts-plain64") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "serpent-xts-plain64";
        fmt.key_size_bits = 512;

        bool ok = CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg);
        if (!ok) {
            // Serpent may not be available in kernel; skip rather than fail
            WARN("serpent not available: " << error_msg);
            return;
        }

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "serpent");
        CHECK(hdr.cipher_mode == "xts-plain64");
        CHECK(hdr.key_size == 64);

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }

    SECTION("twofish-xts-plain64") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "twofish-xts-plain64";
        fmt.key_size_bits = 512;

        bool ok = CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg);
        if (!ok) {
            WARN("twofish not available: " << error_msg);
            return;
        }

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "twofish");
        CHECK(hdr.cipher_mode == "xts-plain64");
        CHECK(hdr.key_size == 64);

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }
#endif
}


// ============================================================================
// IDP field: key_size (required int, must be multiple of 8)
// Volume key size in bits
// ============================================================================

TEST_CASE("IDP key_size", "[idp][crypto][key_size]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error_msg;

    SECTION("512-bit key (default for xts)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.key_size_bits = 512;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.key_size == 64);
    }

    SECTION("256-bit key") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.key_size_bits = 256;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.key_size == 32);
    }

    SECTION("128-bit key (for cbc)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-cbc-essiv:sha256";
        fmt.key_size_bits = 128;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.key_size == 16);
    }
#endif
}


// ============================================================================
// IDP field: hash (required string)
// PBKDF hash algorithm for key derivation
// ============================================================================

TEST_CASE("IDP hash", "[idp][crypto][hash]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string mapped_name = "idp_test_hash";
    std::string error_msg;

    SECTION("sha256") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha256";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));
        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }

    SECTION("sha512") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha512";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));
        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }

    SECTION("sha1") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha1";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));
        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }
#endif
}


// ============================================================================
// IDP field: mname (required string)
// Device-mapper name used when opening/closing the LUKS container
// ============================================================================

TEST_CASE("IDP mname", "[idp][crypto][mname]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error_msg;

    SECTION("mapper name creates correct /dev/mapper/ entry") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        const std::string mname = "cryptdata";
        REQUIRE(CryptOpenNative(loop.GetDevice(), mname, test_key, &error_msg));

        struct stat st;
        REQUIRE(stat(("/dev/mapper/" + mname).c_str(), &st) == 0);

        REQUIRE(CryptCloseNative(mname, &error_msg));
        REQUIRE(stat(("/dev/mapper/" + mname).c_str(), &st) < 0);
    }

    SECTION("different mapper names are independent") {
        LoopbackDevice loop1, loop2;
        REQUIRE(loop1.Create());
        REQUIRE(loop2.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";

        REQUIRE(CryptInitNative(loop1.GetDevice(), fmt, test_key, &error_msg));
        REQUIRE(CryptInitNative(loop2.GetDevice(), fmt, test_key, &error_msg));

        REQUIRE(CryptOpenNative(loop1.GetDevice(), "crypt_a", test_key, &error_msg));
        REQUIRE(CryptOpenNative(loop2.GetDevice(), "crypt_b", test_key, &error_msg));

        struct stat st;
        CHECK(stat("/dev/mapper/crypt_a", &st) == 0);
        CHECK(stat("/dev/mapper/crypt_b", &st) == 0);

        CleanupMapper("crypt_a");
        CleanupMapper("crypt_b");
    }
#endif
}


// ============================================================================
// IDP field: etype (required string: "raw" | "partitioned")
// Encapsulation type - affects block device path, not LUKS format.
// Verify that LUKS operations succeed regardless of etype value.
// ============================================================================

TEST_CASE("IDP etype agnostic", "[idp][crypto][etype]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string mapped_name = "idp_test_etype";
    std::string error_msg;

    // etype does not affect the LUKS container format itself, only the
    // block device path computation in IDPluks::BlockDev().
    // Verify that format/open/close works identically for any etype by
    // exercising the full lifecycle with the same LUKS parameters.

    SECTION("LUKS lifecycle is etype-independent") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.label = "RPICRYPT";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));
        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));

        struct stat st;
        REQUIRE(stat(("/dev/mapper/" + mapped_name).c_str(), &st) == 0);

        REQUIRE(CryptCloseNative(mapped_name, &error_msg));
    }
#endif
}


// ============================================================================
// IDP field: label (optional string)
// LUKS2 label stored in the header
// ============================================================================

TEST_CASE("IDP label", "[idp][crypto][label]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error_msg;

    SECTION("with label") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.label = "MY_LABEL";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.label == "MY_LABEL");
    }

    SECTION("without label (omitted)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        // label left empty (default)

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.label.empty());
    }
#endif
}


// ============================================================================
// IDP field: uuid (optional string)
// LUKS2 UUID; auto-generated if omitted
// ============================================================================

TEST_CASE("IDP uuid", "[idp][crypto][uuid]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error_msg;

    SECTION("explicit UUID") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        const std::string test_uuid = "12345678-1234-1234-1234-123456789abc";

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.uuid = test_uuid;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.uuid == test_uuid);
    }

    SECTION("auto-generated UUID when omitted") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        // uuid left as nullopt

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(!hdr.uuid.empty());
    }
#endif
}


// ============================================================================
// IDP field: sector_size (set from IGconf_device_sector_size: 512 | 4096)
// Encryption sector size
// ============================================================================

TEST_CASE("IDP sector_size", "[idp][crypto][sector_size]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string mapped_name = "idp_test_sector";
    std::string error_msg;

    SECTION("512-byte sectors (SD/eMMC default)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.sector_size = 512;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.sector_size == 512);

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }

    SECTION("4096-byte sectors (NVMe)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.sector_size = 4096;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.sector_size == 4096);

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }
#endif
}


// ============================================================================
// IDP field: pAlignmentBytes (set from image-palign-bytes)
// Data payload alignment; passed as data_alignment_bytes to CryptFormatParams
// ============================================================================

TEST_CASE("IDP pAlignmentBytes", "[idp][crypto][alignment]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error_msg;

    SECTION("1 MiB alignment (default)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.data_alignment_bytes = 1024 * 1024;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        // 1 MiB = 2048 x 512-byte sectors
        CHECK(hdr.data_offset >= 2048);
    }

    SECTION("8 MiB alignment") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.data_alignment_bytes = 8 * 1024 * 1024;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        // 8 MiB = 16384 x 512-byte sectors
        CHECK(hdr.data_offset >= 16384);
    }

    SECTION("no explicit alignment (library default)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.data_alignment_bytes = 0;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        // Default LUKS2 header is 16 MiB, so data_offset >= 32768 sectors
        CHECK(hdr.data_offset > 0);
    }
#endif
}


// ============================================================================
// IDP CryptCloseNative
// Verify close works correctly for all scenarios
// ============================================================================

TEST_CASE("IDP CryptCloseNative", "[idp][crypto][close]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::string error_msg;

    SECTION("close removes dm device") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));
        REQUIRE(CryptOpenNative(loop.GetDevice(), "idp_close_test", test_key, &error_msg));

        struct stat st;
        REQUIRE(stat("/dev/mapper/idp_close_test", &st) == 0);

        REQUIRE(CryptCloseNative("idp_close_test", &error_msg));
        CHECK(stat("/dev/mapper/idp_close_test", &st) < 0);
    }

    SECTION("close nonexistent device fails gracefully") {
        bool result = CryptCloseNative("nonexistent_device_12345", &error_msg);
        CHECK_FALSE(result);
        CHECK(!error_msg.empty());
    }
#endif
}


// ============================================================================
// Combined IDP storage profiles
// Realistic combinations matching IDP schema device types
// ============================================================================

TEST_CASE("IDP storage profiles", "[idp][crypto][profiles]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string mapped_name = "idp_test_profile";
    std::string error_msg;

    SECTION("SD card profile (pi5, 512-sector, 1M align)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha256";
        fmt.key_size_bits = 512;
        fmt.sector_size = 512;
        fmt.label = "RPICRYPT";
        fmt.data_alignment_bytes = 1024 * 1024;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "aes");
        CHECK(hdr.cipher_mode == "xts-plain64");
        CHECK(hdr.key_size == 64);
        CHECK(hdr.sector_size == 512);
        CHECK(hdr.label == "RPICRYPT");
        CHECK(hdr.data_offset >= 2048);

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }

    SECTION("eMMC profile (cm5, 512-sector, 8M align)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        const std::string uuid = "aabbccdd-1122-3344-5566-778899aabbcc";

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha256";
        fmt.key_size_bits = 512;
        fmt.sector_size = 512;
        fmt.label = "RPICRYPT";
        fmt.uuid = uuid;
        fmt.data_alignment_bytes = 8 * 1024 * 1024;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "aes");
        CHECK(hdr.key_size == 64);
        CHECK(hdr.sector_size == 512);
        CHECK(hdr.label == "RPICRYPT");
        CHECK(hdr.uuid == uuid);
        CHECK(hdr.data_offset >= 16384);

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        REQUIRE(CryptCloseNative(mapped_name, &error_msg));
    }

    SECTION("NVMe profile (pi5, 4096-sector, 8M align)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha256";
        fmt.key_size_bits = 512;
        fmt.sector_size = 4096;
        fmt.label = "RPICRYPT_NVME";
        fmt.data_alignment_bytes = 8 * 1024 * 1024;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "aes");
        CHECK(hdr.key_size == 64);
        CHECK(hdr.sector_size == 4096);
        CHECK(hdr.label == "RPICRYPT_NVME");

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        REQUIRE(CryptCloseNative(mapped_name, &error_msg));
    }

    SECTION("Full options (all fields populated)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        const std::string uuid = "11223344-5566-7788-99aa-bbccddeeff00";

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha256";
        fmt.key_size_bits = 512;
        fmt.sector_size = 512;
        fmt.label = "FULL_OPTIONS";
        fmt.uuid = uuid;
        fmt.data_alignment_bytes = 1024 * 1024;

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "aes");
        CHECK(hdr.cipher_mode == "xts-plain64");
        CHECK(hdr.key_size == 64);
        CHECK(hdr.sector_size == 512);
        CHECK(hdr.label == "FULL_OPTIONS");
        CHECK(hdr.uuid == uuid);
        CHECK(hdr.data_offset >= 2048);

        // Full lifecycle
        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        struct stat st;
        REQUIRE(stat(("/dev/mapper/" + mapped_name).c_str(), &st) == 0);
        REQUIRE(CryptCloseNative(mapped_name, &error_msg));
        REQUIRE(stat(("/dev/mapper/" + mapped_name).c_str(), &st) < 0);
    }

    SECTION("Minimal options (only required fields)") {
        LoopbackDevice loop;
        REQUIRE(loop.Create());

        CryptFormatParams fmt;
        fmt.cipher = "aes-xts-plain64";
        fmt.hash = "sha256";
        fmt.key_size_bits = 512;
        // sector_size defaults to 512
        // label empty, uuid nullopt, alignment 0

        REQUIRE(CryptInitNative(loop.GetDevice(), fmt, test_key, &error_msg));

        LuksHeaderInfo hdr;
        REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
        CHECK(hdr.cipher == "aes");
        CHECK(hdr.key_size == 64);
        CHECK(hdr.label.empty());
        CHECK(!hdr.uuid.empty());  // auto-generated

        REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
        CleanupMapper(mapped_name);
    }
#endif
}


// ============================================================================
// Legacy API backward compatibility
// Existing callers in commands.cpp use the 5-arg overload
// ============================================================================

TEST_CASE("IDP legacy API", "[idp][crypto][legacy]") {
    if (getuid() != 0) { WARN("Skipping (requires root)"); return; }
#ifndef HAVE_LIBCRYPTSETUP
    WARN("libcryptsetup not available"); return;
#else
    const std::string test_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string mapped_name = "idp_test_legacy";
    std::string error_msg;

    LoopbackDevice loop;
    REQUIRE(loop.Create());

    REQUIRE(CryptInitNative(loop.GetDevice(), "legacy_label", "aes-xts-plain64",
                           test_key, &error_msg));

    LuksHeaderInfo hdr;
    REQUIRE(ReadLuksHeader(loop.GetDevice(), hdr));
    CHECK(hdr.cipher == "aes");
    CHECK(hdr.cipher_mode == "xts-plain64");
    CHECK(hdr.label == "legacy_label");

    REQUIRE(CryptOpenNative(loop.GetDevice(), mapped_name, test_key, &error_msg));
    CleanupMapper(mapped_name);
#endif
}
