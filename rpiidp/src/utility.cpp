#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <cerrno>
#include <vector>

#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <libudev.h>

#include "utility.h"

// Lifted from RPI modified AOSP

namespace utils {
   int process_spawn_blocking(
         int *r,
         const std::string& bin,
         char * const argv[],
         char * const envp[],
         posix_spawn_file_actions_t *file_actions
         )
   {
      if (!r) return -1;

      int ret;
      pid_t pid;
      int wstatus;

      ret = posix_spawnp(
            &pid,
            bin.c_str(),
            file_actions, // file_actions
            NULL, // spawn_attr
            argv,
            envp
            );

      if (ret) return ret;

      do {
         ret = waitpid(pid, &wstatus, 0);
      } while (ret == -1 && errno == EINTR);

      if ((ret != -1) && WIFEXITED(wstatus)) {
         *r = WEXITSTATUS(wstatus);
         return 0;
      } else {
         if (r) *r = -1;
         return -1;
      }
   }


   bool ReReadPartitionTable(const std::string& dev)
   {
      int fd = open(dev.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
         std::string msg = dev + ": open failed";
         perror(msg.c_str());
         return false;
      }

      int rc = ioctl(fd, BLKRRPART);
      if (rc != 0)
         perror(dev.c_str());
      close(fd);
      return (rc == 0) ? true : false;
   }


   bool WaitReReadPartitionTable(const std::string& dev, int timeout_sec)
   {
      const int interval_ms = 100;
      const int max_attempts = timeout_sec * 1000 / interval_ms;

      for (int i = 0; i < max_attempts; ++i) {
         if (ReReadPartitionTable(dev)) {
            return true;
         }
         if (errno != EBUSY) {
            perror(dev.c_str());
            return false; // Some other error, don't retry
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      }
      return false;
   }


   bool BlockDevReady(const std::string& dev)
   {
      int fd = open(dev.c_str(), O_RDONLY | O_DIRECT);
      if (fd < 0) {
         fd = open(dev.c_str(), O_RDONLY);
         if (fd < 0)
            return false;
      }

      struct stat st;
      if (fstat(fd, &st) < 0) {
         close(fd);
         return false;
      }
      if (!S_ISBLK(st.st_mode)) {
         close(fd);
         return false;
      }

      uint64_t size;
      if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
         close(fd);
         return false;
      }

      close(fd);
      return true;
   }

   bool WaitBlockDev(const std::string& dev, int timeout_sec)
   {
      const int interval_ms = 100;
      const int max_attempts = timeout_sec * 1000 / interval_ms;

      for (int i = 0; i < max_attempts; ++i) {
         if (BlockDevReady(dev)) {
            return true;
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      }
      return false;
   }

   // Wait until udev has finished its processing for a block device
   bool WaitUdevBlockDev(const std::string& target_path, int timeout_ms) {
      struct udev* udev = udev_new();
      if (!udev) {
         return false;
      }

      auto start = std::chrono::steady_clock::now();
      bool ready = false;

      while (true) {
         struct udev_enumerate* enm = udev_enumerate_new(udev);
         udev_enumerate_add_match_subsystem(enm, "block");
         udev_enumerate_scan_devices(enm);

         struct udev_list_entry *devices = udev_enumerate_get_list_entry(enm), *entry;
         udev_list_entry_foreach(entry, devices) {
            const char* syspath = udev_list_entry_get_name(entry);
            struct udev_device* dev = udev_device_new_from_syspath(udev, syspath);
            if (!dev) {
               continue;
            }

            // 1. Check primary node (eg, /dev/mmcblk0p1)
            const char* devnode = udev_device_get_devnode(dev);
            if (devnode && target_path == devnode) {
               ready = udev_device_get_is_initialized(dev);
            }
            // 2. Check all symlinks (eg, /dev/mapper/foo or /dev/disk/...)
            else {
               struct udev_list_entry *links = udev_device_get_devlinks_list_entry(dev), *link;
               udev_list_entry_foreach(link, links) {
                  if (target_path == udev_list_entry_get_name(link)) {
                     ready = udev_device_get_is_initialized(dev);
                     break;
                  }
               }
            }

            udev_device_unref(dev);
            if (ready) {
               break;
            }
         }
         udev_enumerate_unref(enm);

         if (ready) {
            break;
         }

         auto now = std::chrono::steady_clock::now();
         if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout_ms) {
            break;
         }

         std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      udev_unref(udev);
      return ready;
   }

   std::vector<char*> to_execvp_argv(const std::vector<std::string>& args)
   {
      std::vector<char*> argv;
      for (const auto& arg : args) {
         argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr); // execvp expects a null-terminated array
      return argv;
   }
}
