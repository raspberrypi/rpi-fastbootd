#include <iostream>
#include <fstream>
#include <string>
#include "idpparser.h"

#define ERR(msg) std::cerr << "ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl
#define MSG_INDENT(indent, msg) std::cout << std::string(indent, ' ') << msg << std::endl
#define MSG(msg) MSG_INDENT(0, msg)


bool io_check(const std::string& filePath) {
   std::ifstream file(filePath, std::ios::binary | std::ios::ate);
   if (!file.is_open()) {
      ERR("Error: Cannot open file: " << filePath);
      return false;
   }

   std::streamsize size = file.tellg();
   if (size <= 0) {
      ERR("Error: File is empty or unreadable: " << filePath);
      return false;
   }

   return true;
}


void printp(const IDPpartition* p, const std::vector<IDPpartition>& all, unsigned int indent) {
    if (!p)
        return;
    MSG("");

    if (p->isCryptContainer()) {
       MSG_INDENT(indent, "Partition: [encrypted container]");
       MSG_INDENT(indent, " " << p->luks->summary());
    }
    else {
       if (p->isEncrypted(all)) {
          MSG_INDENT(indent, "Partition: [encrypted]");
       }
       else {
          MSG_INDENT(indent, "Partition: [clear]");
       }
       MSG_INDENT(indent, " num " << p->num);
       if (p->typecode)
          MSG_INDENT(indent, " part code " << *p->typecode);
       if (p->img)
          MSG_INDENT(indent, " img " << *p->img);
       if (p->simg)
          MSG_INDENT(indent, " simg " << *p->simg);
       MSG_INDENT(indent, " sz " << p->size);
       MSG_INDENT(indent, " aligned sz " << p->aligned_size);
       if (p->gptlabel)
          MSG_INDENT(indent, " label(gpt) " << *p->gptlabel);
       if (p->gptuuid)
          MSG_INDENT(indent, " uuid(gpt) " << *p->gptuuid);

    }

    if (p->hasChildren(all)) {
          p->foreachChild(all, [&](const IDPpartition& child) {
          printp(&child, all, indent+4);
      });
    }
}


int main(int argc, char* argv[]) {
   if (argc != 2) {
      ERR("Usage: " << argv[0] << " <file>");
      return 1;
   }

   std::string filePath = argv[1];

   if (!io_check(filePath))
      return 1;

   IDPparser parser;

   if (!parser.loadFile(filePath)) {
      ERR("Load failed");
      return 1;
   }

   MSG("Load successful");

   std::vector<IDPpartition> parts = parser.getPartitions();

   for (const auto& part : parts)
      if (part.getParentIndex() == -1)
         printp(&part, parts, 0);

   return 0;
}
