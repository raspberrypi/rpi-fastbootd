#ifndef IDP_VERSION_H
#define IDP_VERSION_H

#include <string>

struct IDPversion {
   int major = 0;
   int minor = 0;
   int patch = 0;

   bool operator>=(const IDPversion& other) const {
      if (major != other.major) return major > other.major;
      if (minor != other.minor) return minor > other.minor;
      return patch >= other.patch;
   }
};


struct VersionRange {
   IDPversion min;
   IDPversion max;

   // A range of {1,0,0} to {2,0,0} will pass 1.x.x but not 2.0.0
   bool contains(const IDPversion& v) const {
      return v >= min && !(v >= max);
   }
};

bool parseVersion(const std::string& version_str, IDPversion& version, std::string& error);

#endif // IDP_VERSION_H
