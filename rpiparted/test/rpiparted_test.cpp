#include <iostream>
#include <cstring>
#include "rpiparted.h"

#define ALIGN (8 * 1024)

// GPT Partition type UUID
constexpr const char * EFI = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";
constexpr const char * LINUX = "0fc63daf-8483-4772-8e79-3d69d8477de4";
constexpr const char * LVM = "e6d6d379-f507-44c2-a23c-238f2a3df928";
constexpr const char * FAT32 = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7";

static PartitionAttributes make_attrs(uint64_t size_bytes, const std::string& type_id) {
    PartitionAttributes a{};
    a.size_bytes = size_bytes;
    a.type_id    = type_id;
    return a;
}

static int append_dos(RPIparted * disk)
{
   std::cout << "Append DOS partitions..." << std::endl;

   if (!disk->appendPartition(make_attrs(5 * 1024 * 1024, "0xc"))) {
      std::cerr << "Failed to append p1" << std::endl;
      return 1;
   }

   if (!disk->appendPartition(make_attrs(10 * 1024 * 1024, "c"))) {
      std::cerr << "Failed to append p2" << std::endl;
      return 1;
   }

   if (!disk->appendPartition(make_attrs(15 * 1024 * 1024, "0c"))) {
      std::cerr << "Failed to append p3" << std::endl;
      return 1;
   }

   if (!disk->appendPartition(make_attrs(20 * 1024 * 1024, "83"))) {
      std::cerr << "Failed to append p4" << std::endl;
      return 1;
   }

   // This should fail
   if (disk->appendPartition(make_attrs(25 * 1024 * 1024, "0x83"))) {
      std::cerr << "Append p5 should have failed" << std::endl;
      return 1;
   }

   // Remove last primary
   if (!disk->removePartition(4)) {
      std::cerr << "Failed to remove p4" << std::endl;
      return 1;
   }

   // New extended part spans remaining
   if (!disk->appendPartition(make_attrs(0, "5"))) {
      std::cerr << "Failed to append p4" << std::endl;
      return 1;
   }

   // New logical spans remaining
   if (!disk->appendPartition(make_attrs(0, "0x83"))) {
      std::cerr << "Failed to append p5" << std::endl;
      return 1;
   }
   return 0;
}


static int append_gpt(RPIparted * disk)
{
   std::cout << "Append GPT partitions..." << std::endl;

   if (!disk->appendPartition(make_attrs(5 * 1024 * 1024, EFI))) {
      std::cerr << "Failed to append p1" << std::endl;
      return 1;
   }

   if (!disk->appendPartition(make_attrs(10 * 1024 * 1024, LINUX))) {
      std::cerr << "Failed to append p2" << std::endl;
      return 1;
   }

   if (!disk->appendPartition(make_attrs(15 * 1024 * 1024, LVM))) {
      std::cerr << "Failed to append p3" << std::endl;
      return 1;
   }

   if (!disk->appendPartition(make_attrs(20 * 1024 * 1024, FAT32))) {
      std::cerr << "Failed to append p4" << std::endl;
      return 1;
   }

   // Bogus UUID should fail
   if (disk->appendPartition(make_attrs(25 * 1024 * 1024, "12345678-05fc-4d3b-a006-743f0f84911e"))) {
      std::cerr << "Append p5 should have failed" << std::endl;
      return 1;
   }
   return 0;
}


static int test01(RPIparted& disk, std::string ptype, std::string device)
{
   int ret = -1;

   std::cout << "[01]..." << std::endl;

   std::cout << "Creating new partition table without label ID..." << std::endl;
   if (!disk.createPartitionTable(ptype, std::nullopt)) {
      std::cerr << "Failed to create partition table of type " << ptype  <<  std::endl;
      return -1;
   }

   if (ptype == "gpt") {
      std::cout << "Creating new GPT table with label ID..." << std::endl;
      if (!disk.createPartitionTable(ptype, "3f671466-0e3f-4d4e-9c39-2cefc7be395c")) {
         std::cerr << "Failed to create partition table." << std::endl;
         return -1;
      }
      ret = append_gpt(&disk);
   }

   if (ptype == "dos" || ptype == "mbr") {
      std::cout << "Creating new MBR table with label ID..." << std::endl;
      if (!disk.createPartitionTable(ptype, "0x12345678")) {
         std::cerr << "Failed to create partition table." << std::endl;
         return -1;
      }
      ret = append_dos(&disk);
   }

   // Write table to disk
   if (ret == 0)
      ret = disk.commit() ? 0 : -1;

   return ret;
}


// Test GPT attributes
// Verify with lsblk -o NAME,PARTLABEL,PARTUUID <device>
static int test02(std::string device)
{
   int ret = 0;
   RPIparted disk;
   PartitionAttributes attr{};

   std::cout << "[02]..." << std::endl;

   if (!disk.openDevice(device, ALIGN)) {
      std::cerr << "Failed to open" << device << std::endl;
      return -1;
   }

   std::cout << "Creating new GPT table with ID..." << std::endl;

   if (!disk.createPartitionTable("GPT", "3f671466-0e3f-4d4e-9c39-2cefc7be395c")) {
      std::cerr << "Failed to create partition table." << std::endl;
      return -1;
   }

   std::cout << "Appending GPT partitions..." << std::endl;

   attr.type_id = EFI;
   attr.size_bytes = (50 * 1024 * 1024);
   attr.partlabel = "boot_a";
   attr.partuuid = "616FD89E-6DD3-431A-9E60-3DEB7A1BEFED";

   if (!disk.appendPartition(attr)) {
      std::cerr << "Failed to append p1" << std::endl;
      return 1;
   }

   attr.type_id = LINUX;
   attr.size_bytes = (50 * 1024 * 1024);
   attr.partlabel = "system_a";
   attr.partuuid = "B3C002F5-69EF-4220-80E0-E40A020D3EDE";

   if (!disk.appendPartition(attr)) {
      std::cerr << "Failed to append p2" << std::endl;
      return 1;
   }

   // Duplicate label (API allows)
   attr = PartitionAttributes{};
   attr.type_id = LVM;
   attr.size_bytes = (50 * 1024 * 1024);
   attr.partlabel = "boot_a";

   if (!disk.appendPartition(attr)) {
      std::cerr << "Should have allowed to append dup label" << std::endl;
      return 1;
   }

   // Duplicate UUID (API allows)
   attr = PartitionAttributes{};
   attr.type_id = LVM;
   attr.size_bytes = (50 * 1024 * 1024);
   attr.partuuid = "616FD89E-6DD3-431A-9E60-3DEB7A1BEFED";

   if (!disk.appendPartition(attr)) {
      std::cerr << "Should have allowed to append dup uuid" << std::endl;
      return 1;
   }

   // Long label (37)
   attr = PartitionAttributes{};
   attr.type_id = EFI;
   attr.size_bytes = (50 * 1024 * 1024);
   attr.partlabel = "1234567890abcdefghijklmnopqrstuvwxyzA";

   if (!disk.appendPartition(attr)) {
      std::cerr << "Should have allowed long label (truncated A)" << std::endl;
      return 1;
   }

   // Bad UUID (short by one char)
   attr = PartitionAttributes{};
   attr.type_id = EFI;
   attr.size_bytes = (50 * 1024 * 1024);
   attr.partuuid = "516FD89E-6DD3-431A-9E60-3DEB7A1BEFE";

   if (disk.appendPartition(attr)) {
      std::cerr << "Should have rejected bad UUID" << std::endl;
      return 1;
   }

   // Flush
   if (ret == 0)
      ret = disk.commit() ? 0 : -1;

   disk.closeDevice();
   return ret;
}


int main(int argc, char* argv[]) {
   if (argc != 3) {
      std::cerr << "Usage: " << argv[0] << " <device> <dos|gpt>" << std::endl;
      return 1;
   }

   std::string device = argv[1];
   std::string ptype = argv[2];
   RPIparted disk;
   int ret = -1;

   if (!disk.openDevice(device, ALIGN)) {
      std::cerr << "Failed to open(1) " << device << std::endl;
      return -1;
   }

   ret = test01(disk, ptype, device);
   if (ret)
      std::cerr << "Error " << device << std::endl;

   disk.closeDevice();

   // Repeat
   if (ret == 0) {
      if (!disk.openDevice(device, ALIGN)) {
         std::cerr << "Failed to open(2) " << device << std::endl;
         return -1;
      }

      ret = test01(disk, ptype, device);
      if (ret)
         std::cerr << "Error " << device << std::endl;

      disk.closeDevice();
   }

   // GPT specific
   ret = test02(device);
   if (ret)
      std::cerr << "Error " << device << std::endl;

   std::cout << "Tests complete: result " << ret << std::endl;
   return ret;
}
