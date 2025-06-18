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

   bool waitBlockDev(const std::string& dev, int timeout_sec)
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
