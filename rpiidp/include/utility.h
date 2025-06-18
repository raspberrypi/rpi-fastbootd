#include <string>
#include <spawn.h>

namespace utils {
   int process_spawn_blocking(int *r,
         const std::string& bin,
         char * const argv[],
         char * const envp[] = nullptr,
         posix_spawn_file_actions_t *file_actions = nullptr);

   bool BlockDevReady(const std::string& dev);

   bool waitBlockDev(const std::string& dev, int timeout_sec = 5);

   bool ReReadPartitionTable(const std::string& dev);

   bool WaitReReadPartitionTable(const std::string& dev, int timeout_seconds = 5);

   std::vector<char*> to_execvp_argv(const std::vector<std::string>& args);
}
