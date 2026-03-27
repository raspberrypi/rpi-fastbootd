#pragma once

#include <string>
#include <cstdint>
#include <optional>

#ifdef HAVE_LIBCRYPTSETUP

// Parameters for LUKS2 container creation, matching IDPluks fields
struct CryptFormatParams {
    std::string cipher;                       // e.g. "aes-xts-plain64"
    std::string hash = "sha256";              // PBKDF hash algorithm
    std::size_t key_size_bits = 512;          // Volume key size in bits
    std::size_t sector_size = 512;            // Encryption sector size
    std::string label;                        // LUKS2 label (optional)
    std::optional<std::string> uuid;          // LUKS2 UUID (optional)
    std::size_t data_alignment_bytes = 0;     // Payload alignment (0 = default)
};

bool CryptInitNative(const std::string& device_path, const CryptFormatParams& params,
                    const std::string& key_data, std::string* error_msg);

// Legacy overload for existing callers (commands.cpp)
bool CryptInitNative(const std::string& device_path, const std::string& label,
                    const std::string& cipher, const std::string& key_data,
                    std::string* error_msg);

bool CryptOpenNative(const std::string& device_path, const std::string& mapped_name,
                    const std::string& key_data, std::string* error_msg);

bool CryptCloseNative(const std::string& mapped_name, std::string* error_msg);

bool CryptSetPasswordNative(const std::string& device_path, const std::string& hw_key,
                           const std::string& user_passphrase, bool remove_passphrase,
                           std::string* error_msg);

bool VeritySetupNative(const std::string& data_device, const std::string& hash_device,
                      std::string* root_hash, std::string* error_msg);

bool VeritySetupAppendedNative(const std::string& device, uint64_t data_size,
                               std::string* root_hash, std::string* error_msg);

#endif // HAVE_LIBCRYPTSETUP

