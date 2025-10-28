#ifndef IDP_NAV_H
#define IDP_NAV_H

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <json/json.h>
#include "idpversion.h"
#include "idpcrypt.h"
#include <iostream>
#include <sstream>

class PartitionNavigator;

struct Partition {
    std::string image; // tag to look up image object
    std::optional<std::string> id;
    std::optional<std::string> uuid;
    std::optional<std::string> role;
    std::string img_name;
    std::uint64_t img_size;
    std::string simg_name;
    unsigned int order;
    std::string pcode;
    std::optional<std::string> plabel;
    std::optional<std::string> puuid;
    std::optional<IDPluks> luks;
    std::shared_ptr<const PartitionNavigator> navigator;
    std::shared_ptr<const Partition> parent;

    bool isEncrypted() const;
    bool isCryptContainer() const;
};


struct NavResult {
    int errors = 0;
    std::vector<std::string> messages;

    void error(const std::string& msg) {
        ++errors;
        messages.push_back(msg);
    }

    bool iserror() {
        return (errors != 0);
    }

    // Overload operator+= so nested calls can assign and increment
    NavResult& operator+=(const NavResult& other) {
       errors += other.errors;
       messages.insert(messages.end(), other.messages.begin(), other.messages.end());
       return *this;
    }
};


class PartitionNavigator : public std::enable_shared_from_this<PartitionNavigator> {
public:
    struct NavFrame {
        Json::Value node; // The current node
        std::vector<std::string> keys; // Ordered keys for this node
        size_t childIdx = 0; // Which child are we at?
        std::string slotKey; // If in a slot, the slot name
    };

    static std::shared_ptr<PartitionNavigator> create(IDPversion version, const Json::Value& obj);
    static std::shared_ptr<PartitionNavigator> createAtPartition(IDPversion version, const Json::Value& obj, const std::string& partitionName);

    // Returns all partitions from the current navigation point
    std::optional<std::vector<std::shared_ptr<Partition>>> getPartitions(unsigned int& order) const;

    // Hierarchical navigation
    bool stepIn();   // Descend into the next child block (partitions, encrypted, slots, etc.)
    bool stepOut();  // Ascend to the parent block

    // Helpers
    bool isSlotted() const;
    std::string getCurrentSlot() const;

    // Public accessor for nav_stack_
    const std::vector<NavFrame>& getNavStack() const { return nav_stack_; }

private:
    explicit PartitionNavigator(IDPversion version, const Json::Value& obj);

    NavResult processRegularPartitions(
          const Json::Value& partitions,
          std::vector<std::shared_ptr<Partition>>& result,
          unsigned int& num,
          std::shared_ptr<const Partition> parentPartition) const;

    std::shared_ptr<Partition> createEncryptedPartition(
          const Json::Value& encryptedNode,
          unsigned int& num) const;

    NavResult processEncryptedNode(
          const Json::Value& encryptedNode,
          std::vector<std::shared_ptr<Partition>>& result,
          unsigned int& num,
          std::shared_ptr<const Partition> parentPartition) const;

    NavResult processSlots(
          const Json::Value& slots,
          std::vector<std::shared_ptr<Partition>>& result,
          unsigned int& num,
          std::shared_ptr<const Partition> parentPartition,
          bool processEncrypted) const;

    NavResult extractPartitions(
          const Json::Value& node,
          std::vector<std::shared_ptr<Partition>>& result,
          unsigned int& num,
          std::shared_ptr<const Partition> parentPartition = nullptr,
          bool processEncrypted = true) const;

    // Navigation error collection and reporting
    bool processNav(const NavResult& result) const
    {
       for (const auto& msg : result.messages) {
          std::cerr << "Nav Error: " << msg << std::endl;
       }
       if (result.errors != 0) {
          std::cerr << "ERROR: Problems encountered during navigation parsing." << std::endl;
       }
       return (result.errors == 0);
    }

    mutable std::vector<NavFrame> nav_stack_;

    Json::Value jroot_;
    IDPversion pmap_version_;
};


// Templated utils


// This is a recursive template function that traverses all partitions in a
// tree structure managed by PartitionNavigator. For each partition, it
// calls a user-supplied function (func). It also recursively steps into child
// nodes (partitions) using stepIn/stepOut.
//
// Calls nav->getPartitions(order) to get the the list of partitions at the
// current navigation level.
// Iterates over each partition calling func(partition, nav, error).
// If func returns false, bail out.
// After processing all partitions at the current level, step into child nodes.
// For each successful stepIn(), recursively calls itself to process the
// (child) partitions.
// After recursion, calls nav->stepOut() to return to the previous level.
// If any recursive call returns false, bail out.
// Return true only if all partitions from the navigation point are parsed
// successfully.
template<typename Func>
bool foreachPartitionImpl(const std::shared_ptr<PartitionNavigator>& nav, Func&& func, std::string& error, unsigned int& order) {
    auto p = nav->getPartitions(order);
    if (!p) {
        return false;
    }
    const auto& partitions = *p;

    for (const auto& partition : partitions) {
        if (!func(partition, nav, error)) {
            return false;
        }
    }
    // Recursively step into each child using stepIn/stepOut
    while (nav->stepIn()) {
        bool ok = foreachPartitionImpl(nav, func, error, order);
        nav->stepOut();
        if (!ok) return false;
    }
    return true;
}

template<typename Func>
bool foreachPartition(const std::shared_ptr<PartitionNavigator>& pnav, Func&& func, std::string& error) {
    unsigned int order = 0;
    return foreachPartitionImpl(pnav, std::forward<Func>(func), error, order);
}


#endif // IDP_NAV_H
