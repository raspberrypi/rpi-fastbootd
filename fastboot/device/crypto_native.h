#pragma once

#include <string>

#ifdef HAVE_LIBCRYPTSETUP

bool CryptInitNative(const std::string& device_path, const std::string& label,
                    const std::string& cipher, const std::string& key_data,
                    std::string* error_msg);

bool CryptOpenNative(const std::string& device_path, const std::string& mapped_name,
                    const std::string& key_data, std::string* error_msg);

bool CryptSetPasswordNative(const std::string& device_path, const std::string& hw_key,
                           const std::string& user_passphrase, bool remove_passphrase,
                           std::string* error_msg);

#endif // HAVE_LIBCRYPTSETUP

