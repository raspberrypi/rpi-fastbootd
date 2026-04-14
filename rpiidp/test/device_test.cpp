#include <iostream>
#include <fstream>
#include <string>
#include <cassert>
#include "idpdevice.h"

#define ERR(msg) std::cerr << "ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl
#define MSG(msg) std::cout << "" << (msg) << std::endl


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


std::vector<char> readFile(const std::string& filename) {
   if (!io_check(filename))
         throw std::runtime_error("Cannot open file");

   std::ifstream file(filename, std::ios::binary | std::ios::ate);
   if (!file) throw std::runtime_error("Cannot open file");

   std::streamsize size = file.tellg();
   file.seekg(0, std::ios::beg);

   std::vector<char> buffer(size);
   if (!file.read(buffer.data(), size)) throw std::runtime_error("Read error");
   return buffer;
}


int main(int argc, char* argv[]) {
   if (argc != 2) {
      ERR("Usage: " << argv[0] << " <file>");
      return 1;
   }

   std::string filePath = argv[1];
   std::vector<char> buffer;

   buffer = readFile(filePath);

   IDPdevice dev(std::filesystem::path("/tmp/my.keyfile"));

   if (!dev.Initialise(buffer.data(), buffer.size()))
      return -1;

   std::string reason;
   bool ready = dev.canProvision(reason);
   MSG(ready ? "Ready for provisioning" : "Not ready for provisioning: " + reason);
   if (!ready)
      return -1;

   std::string provision_reason;
   if (dev.startProvision(provision_reason)) {
      auto cookie = dev.createCookie();
      while (auto bd = dev.getNextBlockDevice(*cookie)) {
         std::cout << "blkdev: " << bd->blockDev << " wants simg: " << bd->simg << std::endl;
      }
   }

   dev.endProvision();
   return 0;
}
