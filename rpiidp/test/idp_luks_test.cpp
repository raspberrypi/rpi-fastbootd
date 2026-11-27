// IDP LUKS2 container creation: the cryptsetup command line used without
// libcryptsetup.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "idpcrypt.h"

namespace {

bool hasArg(const std::vector<std::string>& args, const std::string& arg)
{
   return std::find(args.begin(), args.end(), arg) != args.end();
}

} // namespace


TEST_CASE("IDP luks2createcmd alignment", "[idp][luks][alignment]") {
   IDPluks luks;
   luks.key_size = 512;
   luks.cipher = "aes-xts-plain64";
   luks.hash = "sha256";
   luks.mname = "test_crypt";
   luks.pAlignmentBytes = 1024 * 1024;

   // --align-payload is in 512-byte sectors, so 1MiB is 2048 at any
   // encryption sector size
   for (std::size_t sector : {512, 1024, 2048, 4096}) {
      INFO("sector_size " << sector);
      luks.sector_size = sector;
      auto args = luks.luks2createcmd("/dev/null", "/dev/null");
      CHECK(hasArg(args, "--sector-size=" + std::to_string(sector)));
      CHECK(hasArg(args, "--align-payload=2048"));
   }

   luks.sector_size = 4096;
   luks.pAlignmentBytes = 4 * 1024 * 1024;
   CHECK(hasArg(luks.luks2createcmd("/dev/null", "/dev/null"), "--align-payload=8192"));
}
