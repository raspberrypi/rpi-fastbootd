#include <iostream>
#include <sstream>
#include <set>

#include "nav.h"

#define MSG(msg) std::cout << "" << msg << std::endl
#define ERR(msg) std::cerr << "ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl


namespace Validation {
   NavResult validateLUKS(IDPversion pmapver, const Json::Value& node)
   {
      NavResult nr;

      if (!node.isObject()) {
         nr.error("Invalid node object");
         return nr;
      }

      if (!node.isMember("luks2")) {
         nr.error("Node contains no LUKS2 object");
         return nr;
      }

      Json::Value luks = node["luks2"];
      std::vector<std::string> intKeys = {"key_size"};
      std::vector<std::string> stringKeys = {"cipher", "hash", "mname", "etype"};

      for (const auto& key : intKeys) {
         if (!luks.isMember(key) || !luks[key].isInt()) {
            nr.error("LUKS key '" + key + "' is missing or not an int.");
            return nr;
         }
      }

      for (const auto& key : stringKeys) {
         if (!luks.isMember(key) || !luks[key].isString()) {
            nr.error("LUKS key '" + key + "' is missing or not a string.");
            return nr;
         }
         if (luks[key].asString().empty()) {
            nr.error("LUKS key '" + key + "' is invalid.");
            return nr;
         }
      }

      // Extend here for additional encapsulating types (eg lvm) as needed.
      const std::set<std::string> valideTypes = {"raw", "partitioned"};

      if (valideTypes.find(luks["etype"].asString()) == valideTypes.end()) {
         nr.error("Unsupported LUKS encapsulating type (etype).");
         return nr;
      }

      unsigned int keySize = luks["key_size"].asInt();
      if (keySize % 8 != 0) {
         nr.error("LUKS key size must be a multiple of 8.");
         return nr;
      }

      // Optional fields
      if (luks.isMember("label") && !(luks["label"].isString())) {
         nr.error("LUKS key label is not a string.");
         return nr;
      }
      if (luks.isMember("uuid") && !(luks["uuid"].isString())) {
         nr.error("LUKS key uuid is not a string.");
         return nr;
      }

      // These checks are not for device level validation. They're for sanity
      // checking the parsing only. Device validation may reject a provisioning
      // attempt using a particular value, or the on-device LUKS setup may fail
      // depending on the user's settings. We do not try to mitigate against
      // either scenario here.
      // See https://gitlab.com/cryptsetup/cryptsetup/ for details.
      return nr;
   }

   NavResult validateRegularPartition(IDPversion pmapver, const Json::Value& node)
   {
      NavResult nr;

      // Regular partitions must have an image tag
      if (!node.isMember("image") || !node["image"].isString()) {
         nr.error("Partition image tag missing or not a string");
         return nr;
      }

      // Regular partitions can provide static information. IDP currently doesn't
      // use this, so validate with a light touch.
      if (node.isMember("static")) {
         const std::set<std::string> validRoles = {"boot", "system"};

         Json::Value snode = node["static"];

         if (snode.isMember("role")) {
            if (!snode["role"].isString()) {
               nr.error("Partition role must be a string");
               return nr;
            }
         }

         if (validRoles.find(snode["role"].asString()) == validRoles.end()) {
            nr.error("Unsupported partition role.");
            return nr;
         }

         if (snode.isMember("id")) {
            if (!snode["id"].isString()) {
               nr.error("Partition id must be a string");
               return nr;
            }
         }

         if (snode.isMember("uuid")) {
            if (!snode["uuid"].isString()) {
               nr.error("Partition uuid must be a string");
               return nr;
            }
         }
      }

      return nr;
   }
}


// Partition


bool Partition::isCryptContainer() const
{
    return luks.has_value();
}


bool Partition::isEncrypted() const
{
    // Check this partition and all ancestors for a crypt container
    const Partition* p = this;
    while (p) {
        if (p->isCryptContainer()) {
            return true;
        }
        p = p->parent.get();
    }
    return false;
}


// Navigation


std::shared_ptr<PartitionNavigator> PartitionNavigator::create(IDPversion version, const Json::Value& obj)
{
   // obj is already the provisionmap array
   if (!obj.isArray()) {
      ERR("provisionmap is not an array");
      return nullptr;
   }

   // Look for the attributes object in the array
   for (const auto& entry : obj) {
      if (entry.isMember("attributes")) {
         const auto& attrs = entry["attributes"];
         if (attrs.isMember("system_type")) {
            const auto& systemType = attrs["system_type"].asString();
            if (systemType != "flat" && systemType != "slotted") {
               ERR("bad system type");
               return nullptr;
            }
            // Create a root object containing the provisionmap array
            Json::Value root;
            root["layout"]["provisionmap"] = obj;
            return std::shared_ptr<PartitionNavigator>(new PartitionNavigator(version, root));
         }
      }
   }

   ERR("system_type not found in provisionmap attributes");
   return nullptr;
}


std::shared_ptr<PartitionNavigator> PartitionNavigator::createAtPartition(
    IDPversion version, const Json::Value& obj, const std::string& partitionName)
{
    // Create a root object containing the provisionmap array
    Json::Value root;
    root["layout"]["provisionmap"] = obj;
    auto navigator = std::shared_ptr<PartitionNavigator>(new PartitionNavigator(version, root));

    // obj is already the provisionmap array
    if (!obj.isArray()) {
        ERR("provisionmap is not an array");
        return nullptr;
    }

    // Find the partition and set up the navigation stack appropriately
    for (Json::ArrayIndex i = 0; i < obj.size(); ++i) {
        const auto& entry = obj[i];

        // Check top-level partitions
        if (entry.isMember("partitions")) {
            const auto& partitions = entry["partitions"];
            for (const auto& p : partitions) {
                if (p["image"].asString() == partitionName) {
                    NavFrame frame;
                    frame.node = entry;
                    frame.keys.push_back("partitions");
                    frame.childIdx = 0;
                    navigator->nav_stack_.push_back(frame);
                    return navigator;
                }
            }
        }

        // Check encrypted partitions
        if (entry.isMember("encrypted")) {
            const auto& encrypted = entry["encrypted"];
            if (encrypted.isMember("partitions")) {
                const auto& encPartitions = encrypted["partitions"];
                for (const auto& p : encPartitions) {
                    if (p["image"].asString() == partitionName) {
                        NavFrame frame;
                        frame.node = encrypted;
                        frame.keys.push_back("partitions");
                        frame.childIdx = 0;
                        navigator->nav_stack_.push_back(frame);
                        return navigator;
                    }
                }
            }
        }

        // Check slots
        if (entry.isMember("slots")) {
            const auto& slots = entry["slots"];
            if (slots.isObject()) {
                for (const auto& slotName : slots.getMemberNames()) {
                    const auto& slot = slots[slotName];
                    // Check regular partitions in slot
                    if (slot.isMember("partitions")) {
                        const auto& partitions = slot["partitions"];
                        for (const auto& p : partitions) {
                            if (p["image"].asString() == partitionName) {
                                NavFrame slotFrame;
                                slotFrame.node = slot;
                                slotFrame.slotKey = slotName;
                                slotFrame.keys.push_back("partitions");
                                slotFrame.childIdx = 0;
                                navigator->nav_stack_.push_back(slotFrame);
                                return navigator;
                            }
                        }
                    }
                    // Check encrypted partitions in slot
                    if (slot.isMember("encrypted")) {
                        const auto& encrypted = slot["encrypted"];
                        if (encrypted.isMember("partitions")) {
                            const auto& encPartitions = encrypted["partitions"];
                            for (const auto& p : encPartitions) {
                                if (p["image"].asString() == partitionName) {
                                    NavFrame slotFrame;
                                    slotFrame.node = slot;
                                    slotFrame.slotKey = slotName;
                                    NavFrame encFrame;
                                    encFrame.node = encrypted;
                                    encFrame.keys.push_back("partitions");
                                    encFrame.childIdx = 0;
                                    navigator->nav_stack_.push_back(slotFrame);
                                    navigator->nav_stack_.push_back(encFrame);
                                    return navigator;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ERR("No partition found: " << partitionName);
    return nullptr;
}


bool PartitionNavigator::isSlotted() const
{
    const Json::Value& provisionmap = jroot_["layout"]["provisionmap"];
    if (!provisionmap.isArray()) {
        return false;
    }
    for (const auto& entry : provisionmap) {
        if (entry.isMember("attributes")) {
            const auto& attrs = entry["attributes"];
            if (attrs.isMember("system_type")) {
                return attrs["system_type"].asString() == "slotted";
            }
        }
    }
    return false;
}

std::string PartitionNavigator::getCurrentSlot() const
{
    for (auto it = nav_stack_.rbegin(); it != nav_stack_.rend(); ++it) {
        if (!it->slotKey.empty()) {
            return it->slotKey;
        }
    }
    return "";
}


PartitionNavigator::PartitionNavigator(IDPversion version, const Json::Value& obj)
    : pmap_version_(version), jroot_(obj)
{
    // Initialise stack with the provisionmap array as the root
    NavFrame rootFrame;
    rootFrame.node = jroot_["layout"]["provisionmap"];
    // For arrays, keys are indices as strings
    if (rootFrame.node.isArray()) {
        for (Json::ArrayIndex i = 0; i < rootFrame.node.size(); ++i) {
            rootFrame.keys.push_back(std::to_string(i));
        }
    }
    rootFrame.childIdx = 0;
    nav_stack_.push_back(rootFrame);
}

NavResult PartitionNavigator::processRegularPartitions(
      const Json::Value& partitions,
      std::vector<std::shared_ptr<Partition>>& result,
      unsigned int& num,
      std::shared_ptr<const Partition> parentPartition) const 
{
   NavResult nr;

   for (const auto& p : partitions) {
      auto partition = std::make_shared<Partition>();

      nr = Validation::validateRegularPartition(pmap_version_, p);
      if (nr.iserror())
         return nr;

      partition->image = p["image"].asString();

      if (p.isMember("static")) {
         Json::Value s = p["static"];
         if (s.isMember("id"))
            partition->id = s["id"].asString();

         if (s.isMember("role"))
            partition->role = s["role"].asString();

         if (s.isMember("uuid"))
            partition->uuid = s["uuid"].asString();
      }

      partition->parent = parentPartition;
      partition->navigator = shared_from_this();
      partition->order = ++num;

      result.push_back(partition);
   }
   return nr;
}

std::shared_ptr<Partition> PartitionNavigator::createEncryptedPartition(
      const Json::Value& encryptedNode,
      unsigned int& num) const 
{
   auto encryptedPartition = std::make_shared<Partition>();
   const auto& luks = encryptedNode["luks2"];

   IDPluks luksConfig;

   luksConfig.key_size = luks["key_size"].asInt();
   luksConfig.cipher = luks["cipher"].asString();
   luksConfig.hash = luks["hash"].asString();
   luksConfig.mname = luks["mname"].asString();

   if (luks["etype"].isString() && luks["etype"].asString() == "raw")
      luksConfig.etype = IDPluks::encap_type::Raw;

   if (luks["etype"].isString() && luks["etype"].asString() == "partitioned")
      luksConfig.etype = IDPluks::encap_type::Partitioned;

   // Optional fields
   if (luks.isMember("label")) {
      luksConfig.label = luks["label"].asString();
   }
   if (luks.isMember("uuid")) {
      luksConfig.uuid = luks["uuid"].asString();
   }

   encryptedPartition->luks = luksConfig;
   encryptedPartition->navigator = shared_from_this();
   encryptedPartition->order = ++num;
   return encryptedPartition;
}

NavResult PartitionNavigator::processEncryptedNode(
      const Json::Value& encryptedNode,
      std::vector<std::shared_ptr<Partition>>& result,
      unsigned int& num,
      std::shared_ptr<const Partition> parentPartition) const 
{
   NavResult nr;

   if (parentPartition && parentPartition->isCryptContainer()) {
      nr.error("Nested encrypted containers are not supported");
      return nr;
   }

   nr = Validation::validateLUKS(pmap_version_, encryptedNode);
   if (nr.iserror()) {
      nr.error("Bad LUKS config in encrypted container");
      return nr;
   }

   std::shared_ptr<Partition> encryptedPartition;
   if (encryptedNode.isMember("slots") || encryptedNode.isMember("partitions")) {
      encryptedPartition = createEncryptedPartition(encryptedNode, num);
      result.push_back(encryptedPartition);
   }
   else {
      nr.error("No partitions or slots found in encrypted container");
      return nr;
   }

   if (encryptedNode.isMember("slots")) {
      nr += processSlots(encryptedNode["slots"], result, num, encryptedPartition, false);
   }

   if (encryptedNode.isMember("partitions")) {
      nr += extractPartitions(encryptedNode, result, num, encryptedPartition, false);
   }

   return nr;
}

NavResult PartitionNavigator::processSlots(
      const Json::Value& slots,
      std::vector<std::shared_ptr<Partition>>& result,
      unsigned int& num,
      std::shared_ptr<const Partition> parentPartition,
      bool processEncrypted) const 
{
   NavResult nr;

   for (auto it = slots.begin(); it != slots.end(); ++it) {
      std::string slotKey = it.key().asString();
      NavFrame slotFrame;
      slotFrame.node = *it;
      slotFrame.slotKey = slotKey;
      nav_stack_.push_back(slotFrame);

      if (it->isMember("partitions")) {
         NavFrame partFrame;
         partFrame.node = (*it)["partitions"];
         partFrame.slotKey = slotKey;
         nav_stack_.push_back(partFrame);
         nr += processRegularPartitions((*it)["partitions"], result, num, parentPartition);
         nav_stack_.pop_back();
      }

      if (processEncrypted && it->isMember("encrypted")) {
         NavFrame encFrame;
         encFrame.node = (*it)["encrypted"];
         encFrame.slotKey = slotKey;
         nav_stack_.push_back(encFrame);
         nr += processEncryptedNode((*it)["encrypted"], result, num, parentPartition);
         nav_stack_.pop_back();
      }

      nav_stack_.pop_back();
   }
   return nr;
}

NavResult PartitionNavigator::extractPartitions(
      const Json::Value& node,
      std::vector<std::shared_ptr<Partition>>& result,
      unsigned int& num,
      std::shared_ptr<const Partition> parentPartition,
      bool processEncrypted) const
{
   NavResult nr;

   // If node is an array, process each element in order
   if (node.isArray()) {
      for (const auto& entry : node) {
         if (entry.isMember("attributes")) {
            continue;  // Skip attributes object
         }

         if (processEncrypted && entry.isMember("encrypted")) {
            nr += processEncryptedNode(entry["encrypted"], result, num, parentPartition);
            continue;
         }

         if (entry.isMember("partitions")) {
            nr += processRegularPartitions(entry["partitions"], result, num, parentPartition);
         }

         if (entry.isMember("slots")) {
            nr += processSlots(entry["slots"], result, num, parentPartition, processEncrypted);
         }
      }
      return nr;
   }

   // Process partitions for non-array nodes if we have a parent
   if (node.isMember("partitions") && parentPartition) {
      nr += processRegularPartitions(node["partitions"], result, num, parentPartition);
   }
   return nr;
}

// stepIn: descend into the next child block (partitions, encrypted, slots, etc.)
bool PartitionNavigator::stepIn()
{
    if (nav_stack_.empty()) return false;
    NavFrame& current = nav_stack_.back();
    if (current.childIdx >= current.keys.size()) return false;

    const Json::Value& node = current.node;
    const std::string& key = current.keys[current.childIdx];
    ++current.childIdx;

    Json::Value child;
    if (node.isArray()) {
        child = node[std::stoi(key)];
    } else {
        child = node[key];
    }

    // For regular objects, step into their blocks in order
    if (child.isObject()) {
        NavFrame frame;
        frame.node = child;
        if (!nav_stack_.empty()) {
            const NavFrame& parent = nav_stack_.back();
            if (parent.node.isObject() && nav_stack_.size() >= 2) {
                const NavFrame& grandparent = nav_stack_[nav_stack_.size() - 2];
                if (grandparent.node.isObject() && grandparent.keys.size() > 0 &&
                    grandparent.keys[grandparent.childIdx - 1] == "slots") {
                    frame.slotKey = key;
                }
            }
            if (frame.slotKey.empty() && !parent.slotKey.empty()) {
                frame.slotKey = parent.slotKey;
            }
        }
        for (const auto& blockKey : child.getMemberNames()) {
            frame.keys.push_back(blockKey);
        }
        frame.childIdx = 0;
        nav_stack_.push_back(frame);
        return true;
    }
    // For arrays, treat each element as a child
    if (child.isArray()) {
        NavFrame frame;
        frame.node = child;
        if (!nav_stack_.empty() && !nav_stack_.back().slotKey.empty()) {
            frame.slotKey = nav_stack_.back().slotKey;
        }
        for (Json::ArrayIndex i = 0; i < child.size(); ++i) {
            frame.keys.push_back(std::to_string(i));
        }
        frame.childIdx = 0;
        nav_stack_.push_back(frame);
        return true;
    }
    return false;
}

// stepOut: ascend to the parent block
bool PartitionNavigator::stepOut()
{
    if (nav_stack_.size() <= 1) return false; // Don't pop the root
    nav_stack_.pop_back();
    return true;
}

// Returns all partitions from the current navigation point
std::optional<std::vector<std::shared_ptr<Partition>>>
PartitionNavigator::getPartitions(unsigned int& order) const
{
    std::vector<std::shared_ptr<Partition>> result;

    if (nav_stack_.empty())
       return result;

    const NavFrame& frame = nav_stack_.back();
    const Json::Value& node = frame.node;
    NavResult nr;

    // Check if this node is under an "encrypted" key
    bool isEncryptedNode = false;
    std::string currentSlot;

    // Look through the stack for slot context
    for (const auto& stackFrame : nav_stack_) {
        if (!stackFrame.slotKey.empty()) {
            currentSlot = stackFrame.slotKey;
            break;
        }
    }

    // Walk up the stack to check if any ancestor is "encrypted"
    for (size_t i = 0; i + 1 < nav_stack_.size(); ++i) {
       const NavFrame& parent = nav_stack_[i];
       if (parent.keys.size() > 0 && parent.childIdx > 0 &&
             parent.keys[parent.childIdx - 1] == "encrypted") {
          isEncryptedNode = true;
          break;
       }
    }

    if (node.isObject()) {
       // currentSlot.empty() ensures we only process the true top level of the pmap.
       // currentSlot is set when traversing inside a slots branch.

       // Top-level partitions (skip when under slot/encrypted)
       if (!isEncryptedNode && currentSlot.empty() && node.isMember("partitions")) {
          nr += processRegularPartitions(node["partitions"], result, order, nullptr);
       }
       // Top-level slots (skip when under encrypted)
       if (!isEncryptedNode && currentSlot.empty() && node.isMember("slots")) {
          nr += processSlots(node["slots"], result, order, nullptr, /*processEncrypted=*/true);
       }
       // Top-level encrypted container
       if (currentSlot.empty() && node.isMember("encrypted")) {
          nr += processEncryptedNode(node["encrypted"], result, order, nullptr);
       }
       if (!processNav(nr)) {
          return {};
       }
    }
    return result;
}
