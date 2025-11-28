#ifndef IDP_DEVICE_H
#define IDP_DEVICE_H

#include <variant>

#include "rpiparted.h"
#include "idpparser.h"
#include "idpstorage.h"


class IDPdevice;

class IDPdeviceWriter {
   public:
      IDPdeviceWriter() = delete;
      explicit IDPdeviceWriter(IDPdevice* device);
      ~IDPdeviceWriter();

      bool PrepareStorage();
      bool FinaliseStorage();

   private:
      IDPdevice* device_; // Owner

      bool WritePhysicalPartitions();
      bool InitPhysicalPartitions();
      bool InitCryptPartitions();
      void CloseCryptPartitions();
};


using IDPkeyfile = std::filesystem::path;

struct IDPcookie;

void IDPcookieDeleter(IDPcookie* ptr);

struct IDPblockDevice {
    std::string blockDev;
    std::string simg;
};


class IDPdevice {
   public:
      enum class IDPstate {
         Init,
         Config,
         Ready,
         Writing,
         Partitioned,
         Complete,
         Error
      };

      // Constructor for IDPdevice
      // lukskey: Optional user-provided additional key for second key slot
      // NOTE: This feature is not currently implemented - constructor will throw
      //       exception if a key is provided (Some value)
      IDPdevice(std::optional<IDPkeyfile> lukskey = std::nullopt);
      ~IDPdevice();

      bool Initialise(const char* jdata, size_t length);
      bool canProvision();
      bool startProvision();
      IDPstate getState () const {return state_;}
      bool endProvision();

      using CookiePtr = std::unique_ptr<IDPcookie, void(*)(IDPcookie*)>;

      IDPdevice::CookiePtr createCookie() const;
      std::optional<IDPblockDevice> getNextBlockDevice(IDPcookie& cookie) const;
      void resetCookie(IDPcookie& cookie) const;

   private:
      friend class IDPdeviceWriter;
      IDPparser parser_;
      std::vector<IDPpartition> partitions_;
      IDPimage image_;
      IDPstate state_;

      // Provisioning
      uint64_t session_id_;
      bool setState (IDPstate s);
      std::uint64_t dcap_;
      std::uint64_t isize_;
      IDPdeviceWriter writer;
      // Optional user-provided additional key for second key slot
      // NOTE: This feature is not currently implemented
      std::optional<IDPkeyfile> lukskey_;

      bool validateDeviceIdent() const ;
      bool validateDeviceStorage() ;
      bool validateDeviceReadiness() const;
      bool validateProvisioningIntent() const;
      std::uint64_t computeImageStorageSize() const ;
      bool setPartBlockDev(IDPpartition * part, std::string);
      bool checkFirmwareCryptoStatus() const;
};




#endif // IDP_DEVICE_H
