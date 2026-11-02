// oem download-file's write: modes, error reporting, and the vfat case every
// existing Imager depends on.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../file_write.h"

using rpi::file_write::ParseMode;
using rpi::file_write::WriteFile;

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/file_write_test.XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { std::system(("rm -rf '" + path + "'").c_str()); }
};

std::string Slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

mode_t ModeOf(const std::string& path) {
    struct stat st{};
    REQUIRE(stat(path.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

void Put(const std::string& path, const std::string& text, mode_t mode) {
    std::ofstream(path) << text;
    REQUIRE(chmod(path.c_str(), mode) == 0);
}

}  // namespace

TEST_CASE("Modes parse as octal permission bits", "[file_write]") {
    CHECK(ParseMode("600") == mode_t{0600});
    CHECK(ParseMode("0644") == mode_t{0644});
    CHECK(ParseMode("0") == mode_t{0});
    CHECK_FALSE(ParseMode(""));
    CHECK_FALSE(ParseMode("8"));
    CHECK_FALSE(ParseMode("4755"));   // setuid is not a file's to be given here
    CHECK_FALSE(ParseMode("06000"));
    CHECK_FALSE(ParseMode("rw"));
    CHECK_FALSE(ParseMode("-600"));
}

TEST_CASE("Without a mode, behaviour is unchanged", "[file_write]") {
    TempDir dir;
    const std::string f = dir.path + "/cfg";
    mode_t old = umask(022);

    REQUIRE(WriteFile(f, "abc", 3, std::nullopt).ok);
    CHECK(Slurp(f) == "abc");
    CHECK(ModeOf(f) == 0644);

    Put(f, "previous contents", 0640);
    REQUIRE(WriteFile(f, "new", 3, std::nullopt).ok);
    CHECK(Slurp(f) == "new");
    CHECK(ModeOf(f) == 0640);
    umask(old);
}

TEST_CASE("A mode is exact, whatever the umask or the old file", "[file_write]") {
    TempDir dir;
    const std::string f = dir.path + "/secret";
    mode_t old = umask(0);

    REQUIRE(WriteFile(f, "psk", 3, mode_t{0600}).ok);
    CHECK(ModeOf(f) == 0600);

    Put(f, "older secret", 0666);
    REQUIRE(WriteFile(f, "psk2", 4, mode_t{0600}).ok);
    CHECK(ModeOf(f) == 0600);
    CHECK(Slurp(f) == "psk2");
    umask(old);
}

TEST_CASE("An empty stage leaves the file alone", "[file_write]") {
    TempDir dir;
    const std::string f = dir.path + "/keep";
    Put(f, "precious", 0644);

    auto r = WriteFile(f, "", 0, std::nullopt);
    CHECK_FALSE(r.ok);
    CHECK(r.err.find("Buffer size zero") != std::string::npos);
    CHECK(Slurp(f) == "precious");
}

TEST_CASE("Write errors are reported, not swallowed", "[file_write]") {
    // A full disk used to report success: stdio buffered the data and the
    // failed flush was never checked.
    std::string big(64 * 1024, 'x');
    auto full = WriteFile("/dev/full", big.data(), big.size(), std::nullopt);
    CHECK_FALSE(full.ok);
    CHECK(full.err.find("No space") != std::string::npos);

    TempDir dir;
    auto missing = WriteFile(dir.path + "/no/such/dir/f", "x", 1, std::nullopt);
    CHECK_FALSE(missing.ok);
    CHECK(missing.err.find("Error opening file") != std::string::npos);
}

// vfat accepts fchmod and keeps the mount's mode, so a requested mode would
// otherwise be reported set while the file stays world-readable.
TEST_CASE("vfat: a plain write works and a mode is refused cleanly", "[file_write][root]") {
    if (geteuid() != 0) SKIP("needs root to loop-mount a FAT image");
    TempDir dir;
    const std::string img = dir.path + "/fat.img", mnt = dir.path + "/mnt";
    REQUIRE(std::system(("truncate -s 8M '" + img + "' && mkfs.vfat '" + img + "' >/dev/null && mkdir '" +
                         mnt + "' && mount -o loop '" + img + "' '" + mnt + "'").c_str()) == 0);

    // What Imager does: no mode, onto the boot partition.
    const std::string f = mnt + "/rpi-preseed.toml";
    CHECK(WriteFile(f, "config", 6, std::nullopt).ok);
    CHECK(Slurp(f) == "config");

    auto r = WriteFile(f, "secret", 6, mode_t{0600});
    CHECK_FALSE(r.ok);
    CHECK(r.err.find("Cannot set mode") != std::string::npos);
    CHECK(Slurp(f).empty());   // truncated, but nothing written at the wrong mode

    CHECK(umount(mnt.c_str()) == 0);
}
