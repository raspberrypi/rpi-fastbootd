#include <iostream>
#include <string>
#include <vector>
#include "idpcrypt.h"
#include "utility.h"

// Define to redirect cryptsetup stdout to sdterr
#ifdef OUT2ERR
#include <unistd.h>
#endif


#define ERR(msg) std::cerr << "IDP: ERROR [" << __FILE_NAME__ << ":" << __LINE__ << "]: " << msg << std::endl
#define MSG(msg) std::cout << "IDP: " << msg << std::endl


namespace cryptsetup{

   // Translate vector to args and invoke cryptsetup
   bool run(std::vector<std::string> args, std::string str)
   {
      std::vector<char*> argv = utils::to_execvp_argv(args);

#if 0
      // Dump all args
      for (size_t i = 0; i < argv.size() && argv[i] != nullptr; ++i) {
         std::cout << "argv[" << i << "]: " << argv[i] << std::endl;
      }
#endif

      int rc = -1;
#ifdef OUT2ERR
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
      int ret = utils::process_spawn_blocking(&rc, argv[0], argv.data(), nullptr, &actions);
      posix_spawn_file_actions_destroy(&actions);
#else
      int ret = utils::process_spawn_blocking(&rc, argv[0], argv.data(), nullptr, nullptr);
#endif

      if (ret || rc) {
         std::string msg = (std::string(argv[0]) +
               " [" +
               str +
               "] " +
               "exit " +
               std::to_string(rc) +
               " ret " +
               std::to_string(ret)); ERR(msg);
      }

      return (ret == 0) && (rc == 0) ? true : false;
   }
}


bool IDPluks::Create(std::string_view blkdev, std::filesystem::path keyfile) const
{
   std::string msg = ("Creating " +
         summary() +
         " on " +
         std::string(blkdev)); MSG(msg);

   if (version != 2) {
      ERR("Unsupported LUKS version: " << version);
      return false;
   }

   return cryptsetup::run(luks2createcmd(blkdev, keyfile), "create");
}


bool IDPluks::Open(std::string_view blkdev, std::filesystem::path keyfile) const
{
   std::string msg = ("Opening " +
         std::string(mname) +
         " on " +
         summary() +
         " [" + std::string(blkdev) + "]"); MSG(msg);

   return cryptsetup::run(luks2opencmd(blkdev, keyfile), "open");
}


bool IDPluks::Close(std::optional<std::string_view> blkdev) const
{
   std::string_view dev = blkdev.value_or(mname);
   std::string msg = ("Closing " + std::string(dev)); MSG(msg);

   return cryptsetup::run(luks2closecmd(dev), "close");
}


// Private


std::vector<std::string>  IDPluks::luks2createcmd(std::string_view blkdev, std::filesystem::path keyfile) const
{
   std::vector<std::string> args;

   args.push_back("cryptsetup");
   //args.push_back("--debug");
   args.push_back("--batch-mode");
   args.push_back("luksFormat");
   args.push_back("--type");
   args.push_back("luks2");
   args.push_back("--force-password");

   if (key_size)
      args.push_back("--key-size=" + std::to_string(key_size));

   if (!hash.empty())
      args.push_back("--hash=" + hash);

   if (!cipher.empty())
      args.push_back("--cipher=" + cipher);

   if (sector_size)
      args.push_back("--sector-size=" + std::to_string(sector_size));

   args.push_back("--align-payload=" + std::to_string(pAlignmentBytes / sector_size));

   args.push_back("--key-file=" + keyfile.string());

   if (label && !label->empty())
      args.push_back("--label=" + *label);

   if (uuid && !uuid->empty())
      args.push_back("--uuid=" + *uuid);

   args.push_back(std::string(blkdev));

   return args;
}


std::vector<std::string>  IDPluks::luks2opencmd(std::string_view blkdev, std::filesystem::path keyfile) const
{
   std::vector<std::string> args;

   args.push_back("cryptsetup");
   args.push_back("luksOpen");
   args.push_back(std::string(blkdev));
   args.push_back(mname);
   args.push_back("--key-file=" + keyfile.string());

   return args;
}


std::vector<std::string>  IDPluks::luks2closecmd(std::string_view mname) const
{
   std::vector<std::string> args;

   args.push_back("cryptsetup");
   args.push_back("luksClose");
   args.push_back(std::string(mname));

   return args;
}
