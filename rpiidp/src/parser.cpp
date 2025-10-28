#include <fstream>
#include <iostream>
#include <cstring>
#include <cassert>
#include "idpparser.h"
#include "idpversion.h"
#include "idpstorage.h"
#include "nav.h"


#define ERR(msg) std::cerr << "ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl
#define MSG(msg) std::cout << "" << msg << std::endl


namespace {
   template<typename T>
      bool JCHK(const Json::Value& root, const char* key, T& out, std::string& error)
      {
         if (!root.isMember(key)) {
            error = std::string("Missing required field: ") + key;
            return false;
         }

         try {
            if constexpr (std::is_same_v<T, std::string>) {
               if (!root[key].isString()) {
                  error = std::string("Not a string: ") + key;
                  return false;
               }
               out = root[key].asString();
               if (out.empty()) {
                  error = std::string("Empty string not allowed for: ") + key;
                  return false;
               }
            }
            else if constexpr (std::is_same_v<T, int>) {
               if (!root[key].isInt()) {
                  error = std::string("Not an integer: ") + key;
                  return false;
               }
               out = root[key].asInt();
            }
            else if constexpr (std::is_same_v<T, int64_t>) {
               if (!root[key].isInt64()) {
                  error = std::string("Not a 64-bit integer: ") + key;
                  return false;
               }
               out = root[key].asInt64();
            }
            else if constexpr (std::is_same_v<T, size_t>) {
               if (root[key].isUInt() || root[key].isUInt64()) {
                  out = static_cast<size_t>(root[key].asUInt64());
               } else if (root[key].isInt() || root[key].isInt64()) {
                  int64_t val = root[key].asInt64();
                  if (val < 0) {
                     error = std::string("Negative value for size_t: ") + key;
                     return false;
                  }
                  out = static_cast<size_t>(val);
               } else {
                  error = std::string("Not an unsigned integer: ") + key;
                  return false;
               }
            }
            else if constexpr (std::is_same_v<T, Json::Value>) {
               if (!root[key].isObject() && !root[key].isArray()) {
                  error = std::string("Not an object or array: ") + key;
                  return false;
               }
               out = root[key];
            }
            else if constexpr (std::is_same_v<T, std::optional<std::string>>) {
               if (!root[key].isString()) { error = std::string("Not a string: ") + key; return false; }
               auto s = root[key].asString();
               if (s.empty()) { error = std::string("Empty string not allowed for: ") + key; return false; }
               out = std::move(s);
            }
            // Add more types as needed
            return true;
         }
         catch (const Json::Exception& e) {
            error = std::string("JSON error for ") + key + ": " + e.what();
            return false;
         }
      }


   template<typename T>
      bool JCHK_ARRAY(const Json::Value& root, const char* key, T& out, std::string& error)
      {
         if (!root.isMember(key)) {
            error = std::string("Missing required field: ") + key;
            return false;
         }
         if (!root[key].isArray()) {
            error = std::string("Not an array: ") + key;
            return false;
         }
         out = root[key];
         return true;
      }


   template<typename T>
      bool JCHK_NESTED_IN_ARRAY(
            const Json::Value& array,
            const char* nested_key,   // e.g., "attributes"
            const char* target_key,   // e.g., "PMAPversion"
            T& out,
            std::string& error
            )
      {
         if (!array.isArray()) {
            error = "Value is not an array";
            return false;
         }
         for (const auto& entry : array) {
            if (entry.isObject() && entry.isMember(nested_key)) {
               const Json::Value& nested = entry[nested_key];
               if (nested.isMember(target_key)) {
                  // Use your existing JCHK to check type and retrieve value
                  return JCHK(nested, target_key, out, error);
               }
            }
         }
         error = std::string("Key '") + target_key + "' not found in any '" + nested_key + "' object in array";
         return false;
      }

   template<>
      bool JCHK<IDPversion>(const Json::Value& root, const char* key, IDPversion& out, std::string& error)
      {
         std::string version_str;
         if (!JCHK(root, key, version_str, error)) {
            return false;
         }
         return parseVersion(version_str, out, error);
      }



   bool checkIGCompat(const Json::Value& root, std::string& version_str, std::string& error)
   {
      IDPversion min_supported{2, 0, 0};  // Minimum supported IG version
      IDPversion config_version;
      Json::Value obj, layout;
      std::string str;

      if (!JCHK(root, "IGversion", str, error)) {
         return false;
      }

      if (!parseVersion(str, config_version, error)) {
         return false;
      }

      if (!(config_version >= min_supported)) {
         error = "Unsupported IG version: " + std::to_string(config_version.major) + "." +
            std::to_string(config_version.minor) + "." +
            std::to_string(config_version.patch);
         return false;
      }
      version_str = str;

      if (!JCHK(root, "IGmeta", obj, error)) {
         ERR("" << error);
         return false;
      }

      if (!JCHK(root, "attributes", obj, error)) {
         ERR("" << error);
         return false;
      }

      if (!JCHK(root, "layout", layout, error)) {
         return false;
      }

      if (!layout.isMember("partitionimages")) {
         ERR("Partition images not found in layout");
         return false;
      }

      if (!JCHK(layout, "partitiontable", obj, error)) {
         ERR("layout: partitiontable not found");
         return false;
      }

      std::string label;
      if (!JCHK(obj, "label", label, error)) {
         ERR("partitiontable: label not found");
         return false;
      }

      if (label != "dos" && label != "gpt") {
         error = std::string("Bad partition table label: '") + label + "'. Need dos|gpt" ;
         return false;
      }

      return true;
   }


   bool checkPMAPCompat(const Json::Value& root, std::string& version_str, std::string& error)
   {
      IDPversion min_supported{1, 0, 0};  // Minimum supported PMAP version
      IDPversion pmap_version;
      Json::Value obj, pmap, attr;
      std::string str;

      if (!JCHK(root, "layout", obj, error)) {
         return false;
      }

      if (!JCHK_ARRAY(obj, "provisionmap", pmap, error)) {
         return false;
      }

      if (!JCHK_NESTED_IN_ARRAY(pmap, "attributes", "PMAPversion", str, error)) {
         return false;
      }

      if (!parseVersion(str, pmap_version, error)) {
         return false;
      }

      if (!(pmap_version >= min_supported)) {
         error = "Unsupported PMAP version: " + std::to_string(pmap_version.major) + "." +
            std::to_string(pmap_version.minor) + "." +
            std::to_string(pmap_version.patch);
         return false;
      }
      version_str = str;
      return true;
   }


   void dumpPartitionInfo(const std::shared_ptr<Partition>& partition,
                          const std::shared_ptr<PartitionNavigator>& nav,
                          const IDPversion& pmap_version)
   {
      if (nav->isSlotted() && !nav->getCurrentSlot().empty())
         MSG("\nSlot " << nav->getCurrentSlot() << ":");

      if (!partition)
         return;

      if (partition->isCryptContainer()) {
         MSG("Partition: [encrypted container]");
         if (partition->luks) {
            MSG("" << partition->luks->summary());
         }
      } else {
         if (partition->isEncrypted())
            MSG("Partition [encrypted]: ");
         else
            MSG("Partition [clear]: ");

         if (partition->id)
            MSG(" id: " << *partition->id);

         MSG(" image: " << partition->image);
         MSG(" pcode: " << partition->pcode);
         MSG(" image name: " << partition->img_name);
         MSG(" image size: " << partition->img_size);
         MSG(" simg name: " << partition->simg_name);
         if (partition->plabel)
            MSG(" label: " << *partition->plabel);
         if (partition->uuid)
            MSG(" uuid: " << *partition->puuid);
         if (partition->role)
            MSG(" role: " << *partition->role);
      }
         MSG(" order: " << partition->order);
   }


   // Translate a genimage size unit
   // https://github.com/pengutronix/genimage?tab=readme-ov-file#the-image-section
   size_t fromGIsz(const std::string& str)
   {
      size_t i = 0;

      // Extract numeric part (digits only)
      while (i < str.length() && isdigit(str[i])) ++i;
      std::string numberPart = str.substr(0, i);
      std::string suffix = str.substr(i);

      if (numberPart.empty())
         throw std::invalid_argument("Invalid GI size format: " + str);

      size_t value = std::stoull(numberPart);

      if (suffix == "K" || suffix == "k")
         value *= 1024ULL;
      else if (suffix == "M" || suffix == "m")
         value *= 1024ULL * 1024;
      else if (suffix == "G" || suffix == "g")
         value *= 1024ULL * 1024 * 1024;
      else if (suffix == "s")
         value *= 512ULL;
      else if (!suffix.empty())
         throw std::invalid_argument("Unsupported GI size suffix: " + suffix);

      return value;
   }
}


//
// Private
//

struct IDPparser::VersionParser {
   struct VersionRange {
      IDPversion min;
      IDPversion max;

      bool contains(const IDPversion& v) const {
         return v >= min && !(v >= max);
      }
   };

   VersionRange range;
   ParserFunc parser;
};


class IDPparser::VersionedParser {
   private:
      std::vector<VersionParser> parsers_;

   public:
      void registerParser(const IDPversion& min, const IDPversion& max, ParserFunc parser) {
         parsers_.push_back({VersionParser::VersionRange{min, max}, parser});
      }

      bool parse(const IDPversion& version, const Json::Value& json, std::string& error) {
         for (const auto& parser : parsers_) {
            if (parser.range.contains(version)) {
               return parser.parser(json, error);
            }
         }
         error = "No parser found for version " + std::to_string(version.major) + "." +
            std::to_string(version.minor) + "." + std::to_string(version.patch);
         return false;
      }
};


bool IDPparser::IGvalidate()
{
   std::string str, error;
   IDPversion version;
   Json::Value obj;

   if (!checkIGCompat(root_, str, error)) {
      ERR("IG compat error: " << error);
      return false;
   }

   MSG("Parsing: " << str);

   // Initialise and create IG parser
   if (!parseVersion(str, version, error)) {
      ERR("Invalid version format: " << error);
      return false;
   }

   IGparser_ = std::make_unique<VersionedParser>();
   if (version.major == 2) {
      IGparser_->registerParser(
         IDPversion{2, 0, 0},
         IDPversion{3, 0, 0},
         [this, version](const Json::Value& json, std::string& error) {
            return this->parseIGv2(json, version, error);
         }
      );
   } else {
      ERR("Unsupported IG major version: " << version.major);
      return false;
   }
   IGversion_ = version;

   // Initiate IG parsing
   if (!IGparser_->parse(version, root_, error)) {
      ERR("Parsing error: " << error);
      return false;
   }

   MSG("device class: " << image_.device_class);
   MSG("device variant: " << image_.device_variant);
   MSG("device storage type: " << image_.device_storage.typeString());
   MSG("image name: " << image_.name);
   MSG("image version: " << image_.version);

   return walkTree();
}



// IG v2 specific parsing
bool IDPparser::parseIGv2(const Json::Value& json, const IDPversion& version, std::string& error)
{
   IDPversion pmap_version;
   Json::Value obj;
   std::string str;
   size_t sectors;

   // Can operate on version.<member> as needed for compat

   JCHK(json, "IGmeta", obj, error);
   if (!
         (JCHK(obj, "IGconf_device_class", image_.device_class, error) &&
          JCHK(obj, "IGconf_device_storage_type", str, error) &&
          JCHK(obj, "IGconf_device_sector_size", sectors, error) &&
          JCHK(obj, "IGconf_device_variant", image_.device_variant, error) &&
          JCHK(obj, "IGconf_image_version", image_.version, error)
         )) {
      ERR("Error reading image device info: " << error);
      return false;
   }

   // Sane?
   if (sectors % 512 != 0) {
      ERR("Unsupported sector size: " << sectors);
      return false;
   }
   image_.device_storage.sector_size = sectors;

   // Set storage attrs
   const Json::Value& storage = json["IGmeta"]["IGconf_device_storage_type"];
   if (storage.asString() == "sd")
      image_.device_storage.type = IDPstorage::storage_type::SD;
   else if (storage.asString() == "emmc")
      image_.device_storage.type = IDPstorage::storage_type::EMMC;
   else if (storage.asString() == "nvme")
      image_.device_storage.type = IDPstorage::storage_type::NVME;
   else {
      ERR("Invalid device storage type: " << storage.asString());
      return false;
   }

   JCHK(json, "attributes", obj, error);
   if (!
         (JCHK(obj, "image-name", image_.name, error) &&
          JCHK(obj, "image-size", image_.size, error) &&
          JCHK(obj, "image-palign-bytes", str, error)
         )) {
      ERR("Error reading image attributes: " << error);
      return false;
   }

   if (!image_.size) {
      ERR("Image size reported as zero");
      return false;
   }

   size_t align = fromGIsz(str);
   if (align % (1024*1024) != 0) {
      ERR("Partition alignment must be a multiple of 1MB.");
      return false;
   }
   image_.device_storage.ptable_align = align;


   const Json::Value& ptable = json["layout"]["partitiontable"]["label"];

   if (ptable.asString() == "dos")
      image_.device_storage.ptable_type = IDPptable_type::DOS;
   if (ptable.asString() == "gpt")
      image_.device_storage.ptable_type = IDPptable_type::GPT;

   if (image_.device_storage.ptable_type == IDPptable_type::GPT) {
      const Json::Value& id = json["layout"]["partitiontable"]["id"];
      if (id.isString() && !id.asString().empty())
         image_.device_storage.ptable_id = id.asString();
      else
         image_.device_storage.ptable_id.reset();
   }

   if (!checkPMAPCompat(json, str, error)) {
      ERR("PMAP compat check failed: " << error);
      return false;
   }

   // Initialise and create Provisioning Map Parser
   if (!parseVersion(str, pmap_version, error)) {
      ERR("Invalid version format: " << error);
      return false;
   }

   MSG("PMAP: " << str);

   PMAPparser_ = std::make_unique<VersionedParser>();
   if (pmap_version.major == 1) {
      PMAPparser_->registerParser(
         IDPversion{1, 0, 0},
         IDPversion{2, 0, 0},
         [this, pmap_version](const Json::Value& json, std::string& error) {
            return this->parsePMAPv1(json, pmap_version, error);
         }
      );
   } else {
      ERR("Unsupported PMAP major version: " << pmap_version.major);
      return false;
   }
   PMAPversion_ = pmap_version;

   // Initiate PMAP parsing
   if (!PMAPparser_->parse(pmap_version, json, error)) {
      ERR("PMAP parser error: " << error);
      return false;
   }

   // Walk the pmap populating the info from the corresponding partition image.
   // These constitute all the partitions that will be provisioned, with the
   // order being exactly as it's described in the JSON.
   partitions_.clear();
   std::unordered_map<const Partition*, int> p2i;
   std::string perr;
   if (!foreachPartition(pnav_,
                    [this,&p2i](const std::shared_ptr<Partition>& partition,
                    const std::shared_ptr<PartitionNavigator>& nav,
                    std::string& err) -> bool {

      if (!partition) {
         err = "No partition!";
         return false;
      }

      // Encrypted container partition images are not created off-line and
      // are only written on the device, so they have no image.
      if (!partition->isCryptContainer()) {
         const Json::Value& img = getJSON({"layout", "partitionimages", partition->image});
         if (img.isNull()) {
            err = "No image found for " + partition->image;
            return false;
         }
         std::string error;

         // Mandatory
         if (! (JCHK(img, "image", partition->img_name, error) &&
                JCHK(img, "simage", partition->simg_name, error) &&
                JCHK(img, "size", partition->img_size, error)
               )) {
            err = "Attribute collection failed for partition image: " + partition->image;
            return false;
         }
         if (img.isMember("partition-type"))
               JCHK(img, "partition-type", partition->pcode, error);
         else if (img.isMember("partition-type-uuid"))
               JCHK(img, "partition-type-uuid", partition->pcode, error);
         else {
            err = "No partition code found for " + partition->image;
            return false;
         }

         // Optional
         if (img.isMember("partition-label")) {
            JCHK(img, "partition-label", partition->plabel, error);
         }
         if (img.isMember("partition-uuid")) {
            JCHK(img, "partition-uuid", partition->puuid, error);
         }
      }

      // Establishing MBR extended partitions from the genimage config
      // is non-trivial, with the extended partition being able to be created
      // in the disk image without an actual entry in the genimage config.
      // It's simpler and safer to restrict MBR provisioning support to primary
      // partitions only, leaving GPT to cater for anything else.
      if (image_.device_storage.ptable_type == IDPptable_type::DOS &&
            partition->order > 4) {
         err = "MBR support restricted to primary partitions only";
         return false;
      }

      //dumpPartitionInfo(partition, nav, this->PMAPversion_);

      // Populate for API
      IDPpartition p = {};

      // Only partitions containing images can have their sizes statically set.
      // Everything else needs to be computed dynamically.
      if (partition->isCryptContainer()) {
         p.luks = partition->luks;
         p.luks->pAlignmentBytes = image_.device_storage.ptable_align;
         p.luks->sector_size = image_.device_storage.sector_size;
      }
      else {
         p.img = partition->img_name;
         p.simg = partition->simg_name;
         p.size = partition->img_size;
         p.aligned_size = image_.device_storage.alignUp(partition->img_size);
         p.typecode = partition->pcode;
         if (partition->plabel)
            p.gptlabel = partition->plabel;
         if (partition->puuid)
            p.gptuuid = partition->puuid;
      }
      p.index = partitions_.size();

      // Set parent_index
      if (partition->parent) {
         auto it = p2i.find(partition->parent.get());
         p.parent_index = (it != p2i.end()) ? it->second : -1;
      } else {
         p.parent_index = -1;
      }

      // Register this partition's pointer to its index and add
      p2i[partition.get()] = p.index;
      partitions_.push_back(p);

      return true;
   }, perr)) {
      error = "Partition processing failed: " + perr;
      return false;
   }
   return true;
}


// PMAP v1 specific parsing
bool IDPparser::parsePMAPv1(const Json::Value& json, const IDPversion& version, std::string& error)
{
   Json::Value layout, pmap;
   std::string type;

   // Can operate on version.<member> as needed for compat

   if (!JCHK(json, "layout", layout, error)) {
      return false;
   }

   if (!JCHK_ARRAY(layout, "provisionmap", pmap, error)) {
      return false;
   }

   if (!JCHK_NESTED_IN_ARRAY(pmap, "attributes", "system_type", type, error)) {
      return false;
   }

   if (type != "flat" && type != "slotted") {
      error = std::string("Bad system type in provisionmap: '") + type + "'. Need flat|slotted" ;
      return false;
   }

   MSG("" << writer_.write(pmap));

   pnav_ = PartitionNavigator::create(version, pmap);
   if (!pnav_) {
      ERR("Failed to bind pmap nav");
      return false;
   }

   // Add more v1 specific parsing here
   return true;
}


const Json::Value& IDPparser::getJSON(const std::vector<std::string>& keys) const
{
    const Json::Value* node = &root_;
    for (const auto& key : keys) {
        if (!node->isObject() || !node->isMember(key))
            return Json::Value::nullSingleton();
        node = &(*node)[key];
    }
    return *node;
}


bool IDPparser::walkTree()
{
   unsigned int root_num = 1;
   for (auto& part : partitions_) {
      if (part.getParentIndex() == -1) {
         part.num = root_num++; // No parent: top/root level partition

         if (part.isCryptContainer()) {
            // Set partition type for the container
            std::string code =
               image_.device_storage.ptable_type == IDPptable_type::DOS ?
               "0x83" : "0fc63daf-8483-4772-8e79-3d69d8477de4"; // linux
            part.typecode = code;
         }

         unsigned int child_num = 1;
         for (auto& child : partitions_) {
            if (child.getParentIndex() == part.getIndex()) {
               child.num = child_num++; // Child partition
            }
         }
      }
   }
   return true;
}


//
// Public
//


IDPparser::IDPparser()
  : jvalid_(false) {}


IDPparser::~IDPparser() = default;


bool IDPparser::loadFile(const std::string& filePath)
{
   std::ifstream file(filePath);
   if (!file.is_open()) {
      std::cerr << "Failed to open file: " << filePath << std::endl;
      return false;
   }

   std::string jsonContent((std::istreambuf_iterator<char>(file)),
         std::istreambuf_iterator<char>());
   return loadJSON(jsonContent);
}


bool IDPparser::loadJSON(const std::string& json_string)
{
   Json::Reader reader;
   if (!reader.parse(json_string, root_)) {
      ERR("Unable to parse JSON: " << reader.getFormattedErrorMessages());
      return false;
   }
   jvalid_ = IGvalidate();
   return jvalid_;
}


bool IDPparser::loadData(const char* data, size_t length)
{
   std::string jsonString(data, length);
   return loadJSON(jsonString);
}


bool IDPparser::getImage(IDPimage& out) const
{
    if (!jvalid_) {
        return false;
    }
    out = image_;
    return true;
}


std::vector<IDPpartition> IDPparser::getPartitions() const
{
    return partitions_;
}


bool IDPpartition::isEncrypted(const std::vector<IDPpartition>& all) const
{
    if (parent_index < 0 || parent_index >= static_cast<int>(all.size())) {
        return false;
    }
    return all[parent_index].isCryptContainer();
}


bool IDPpartition::hasChildren(const std::vector<IDPpartition>& all) const
{
    for (const auto& candidate : all) {
        if (candidate.parent_index == index) {
            return true;
        }
    }
    return false;
}


void IDPpartition::foreachChild(const std::vector<IDPpartition>& all, const std::function<void(const IDPpartition&)>& func) const
{
    for (const auto& candidate : all) {
        if (candidate.parent_index == index) {
            func(candidate);
        }
    }
}


std::uint64_t IDPpartition::getSize(
      bool aligned,
      const std::optional<std::reference_wrapper<const std::vector<IDPpartition>>>& all
      ) const
{
   std::uint64_t total = 0;

   // For LUKS containers, the recursive nature of this function
   // ensures the reported size will encapsulate all children.
   if (isCryptContainer()) {
      total = luks->headerSize();

      if (luks->etype == IDPluks::encap_type::Partitioned) {
         // Partitioned LUKS uses GPT
         total += luks->pAlignmentBytes * 2; // Main GPT + backup
      }
   } else {
      total = aligned ? aligned_size : size;
   }

   if (all) {
      foreachChild(all->get(), [&](const IDPpartition& child) {
         total += child.getSize(aligned, all); //recurse
      });
   }

   return total;
}
