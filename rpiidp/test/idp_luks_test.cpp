// IDP LUKS2 containers: the cryptsetup command line used without
// libcryptsetup, and the dm-crypt sector size the parser gives a container.
//
// luks2.sector_size in the provisioning map sets the sector size. Without it
// the container follows the device's logical sector size
// (IGconf_device_sector_size), and it can never be smaller than that.

#include <catch2/catch_test_macros.hpp>

#include <json/json.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "idpcrypt.h"
#include "idpparser.h"

namespace {

bool hasArg(const std::vector<std::string>& args, const std::string& arg)
{
   return std::find(args.begin(), args.end(), arg) != args.end();
}

const char* kDoc = R"({
  "IGversion": "2.3.0",
  "IGmeta": {
    "IGconf_device_class": "pi5",
    "IGconf_device_storage_type": "emmc",
    "IGconf_device_sector_size": 512,
    "IGconf_device_variant": "8G",
    "IGconf_image_version": "1.0.0"
  },
  "attributes": {
    "image-name": "test",
    "image-size": 1073741824,
    "image-palign-bytes": "1M"
  },
  "layout": {
    "partitiontable": { "label": "gpt" },
    "partitionimages": {
      "boot": {
        "image": "boot.img", "simage": "boot.simg", "size": 67108864,
        "in-partition-table": "true",
        "partition-type-uuid": "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
      },
      "root": {
        "image": "root.img", "simage": "root.simg", "size": 268435456,
        "in-partition-table": "true",
        "partition-type-uuid": "0fc63daf-8483-4772-8e79-3d69d8477de4"
      }
    },
    "provisionmap": [
      { "attributes": { "PMAPversion": "1.5.0", "system_type": "flat" } },
      { "partitions": [ { "image": "boot" } ] },
      {
        "encrypted": {
          "luks2": {
            "key_size": 512, "cipher": "aes-xts-plain64", "hash": "sha256",
            "uuid": "00000000-0000-0000-0000-000000000003",
            "mname": "root_crypt", "etype": "partitioned"
          },
          "partitions": [ { "image": "root" } ]
        }
      }
    ]
  }
})";

// The description with the given device sector size and, if any,
// luks2.sector_size.
std::string description(unsigned int device_sector,
                        const std::optional<Json::Value>& luks_sector)
{
   Json::Value root;
   Json::CharReaderBuilder reader;
   std::string errs;
   std::istringstream in(kDoc);
   REQUIRE(Json::parseFromStream(reader, in, &root, &errs));

   root["IGmeta"]["IGconf_device_sector_size"] = device_sector;
   if (luks_sector)
      root["layout"]["provisionmap"][2]["encrypted"]["luks2"]["sector_size"] = *luks_sector;

   Json::StreamWriterBuilder writer;
   return Json::writeString(writer, root);
}

// Load a description, returning the sector size of its encrypted container.
std::optional<std::size_t> containerSectorSize(const std::string& json, std::string& error)
{
   IDPparser parser;
   if (!parser.loadJSON(json, error))
      return std::nullopt;

   for (const auto& part : parser.getPartitions())
      if (part.isCryptContainer())
         return part.luks->sector_size;

   error = "no encrypted container";
   return std::nullopt;
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


TEST_CASE("IDP luks2 sector_size", "[idp][parser][sector_size]") {
   std::string error;

   SECTION("absent: follows the device sector size") {
      for (unsigned int device : {512u, 4096u}) {
         INFO("device sector size " << device);
         auto size = containerSectorSize(description(device, std::nullopt), error);
         INFO(error);
         REQUIRE(size);
         CHECK(*size == device);
      }
   }

   SECTION("set: used instead of the device sector size") {
      for (unsigned int luks : {512u, 1024u, 2048u, 4096u}) {
         INFO("luks2.sector_size " << luks);
         auto size = containerSectorSize(description(512, Json::Value(luks)), error);
         INFO(error);
         REQUIRE(size);
         CHECK(*size == luks);
      }

      auto size = containerSectorSize(description(4096, Json::Value(4096u)), error);
      INFO(error);
      REQUIRE(size);
      CHECK(*size == 4096);
   }

   SECTION("smaller than the device sector size is refused") {
      for (unsigned int luks : {512u, 1024u, 2048u}) {
         INFO("luks2.sector_size " << luks);
         CHECK_FALSE(containerSectorSize(description(4096, Json::Value(luks)), error));
         CHECK(error.find("smaller than IGconf_device_sector_size") != std::string::npos);
      }
   }

   SECTION("unsupported values are refused") {
      const std::vector<Json::Value> bad = {
         Json::Value(0), Json::Value(256), Json::Value(3000), Json::Value(8192),
         Json::Value(-512), Json::Value("4096"), Json::Value(4096.5),
      };
      for (const auto& value : bad) {
         INFO("luks2.sector_size " << value.toStyledString());
         CHECK_FALSE(containerSectorSize(description(512, value), error));
      }
   }
}
