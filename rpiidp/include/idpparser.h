#ifndef IDP_PARSER_H
#define IDP_PARSER_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <json/json.h>

#include "idpversion.h"
#include "idpcrypt.h"
#include "idpstorage.h"

#include "nav.h"

class IDPdeviceWriter;

struct IDPpartition {
    std::uint32_t num;
    std::optional<std::string> img;
    std::optional<std::string> simg;
    std::uint64_t size;
    std::uint64_t aligned_size;
    std::optional<std::string> pcode;
    std::optional<IDPluks> luks;

    bool isCryptContainer() const { return luks.has_value(); }
    bool isEncrypted(const std::vector<IDPpartition>& all) const;
    bool hasChildren(const std::vector<IDPpartition>& all) const;
    void foreachChild(const std::vector<IDPpartition>& all, const std::function<void(const IDPpartition&)>& func) const;
    void setBlockDev(std::string dev){blkdev = dev;}
    std::string getBlockDev() const {return blkdev.value_or("");}

    int getIndex() const { return index; }
    int getParentIndex() const { return parent_index; }
    std::uint64_t getSize(bool aligned, const std::optional<std::reference_wrapper<const std::vector<IDPpartition>>>& all) const;

private:
    friend class IDPdeviceWriter;
    friend class IDPparser;

    int index = -1;
    int parent_index = -1;
    std::optional<std::string> blkdev;
};


class IDPparser {
   public:
      IDPparser();
      ~IDPparser();

      bool loadJSON(const std::string& json_string);
      bool loadFile(const std::string& filePath);
      bool loadData(const char* data, size_t length);
      bool getImage(IDPimage& out) const;
      std::vector<IDPpartition> getPartitions() const;

   private:
      using ParserFunc = std::function<bool(const Json::Value&, std::string&)>;

      struct VersionParser;
      class VersionedParser;

      std::unique_ptr<VersionedParser> IGparser_;
      std::unique_ptr<VersionedParser> PMAPparser_;

      Json::Value root_;

      bool parseIGv2(const Json::Value& json, const IDPversion& version, std::string& error);
      bool parsePMAPv1(const Json::Value& json, const IDPversion& version, std::string& error);

      bool IGvalidate();
      bool jvalid_;

      const Json::Value& getJSON(const std::vector<std::string>& keys) const;

      bool walkTree();

      Json::StyledWriter writer_;
      IDPversion IGversion_;
      IDPversion PMAPversion_;
      IDPimage image_;
      std::shared_ptr<PartitionNavigator> pnav_;
      std::vector<IDPpartition> partitions_;
};

#endif // IDP_PARSER_H
