#include <cstdio>
#include "idpversion.h"

bool parseVersion(const std::string& version_str, IDPversion& version, std::string& error) {
   int count = sscanf(version_str.c_str(), "%d.%d.%d",
         &version.major, &version.minor, &version.patch);
   if (count != 3) {  // major.minor.patch required
      error = "Invalid version format: " + version_str + " (expected X.Y.Z)";
      return false;
   }
   return true;
}
