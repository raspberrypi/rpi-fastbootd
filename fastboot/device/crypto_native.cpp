// Native LUKS operations using libcryptsetup
#include "crypto_native.h"

#ifdef HAVE_LIBCRYPTSETUP
#include <libcryptsetup.h>
#include <cstring>
#include <errno.h>

bool CryptInitNative(const std::string& device_path, const std::string& label,
                    const std::string& cipher_spec, const std::string& key_data,
                    std::string* error_msg) {
    struct crypt_device *cd = NULL;
    struct crypt_params_luks2 params = {};
    
    // Parse cipher specification (e.g., "aes-xts-plain64" -> cipher="aes", mode="xts-plain64")
    std::string cipher = "aes";
    std::string mode = "xts-plain64";
    
    size_t dash_pos = cipher_spec.find('-');
    if (dash_pos != std::string::npos) {
        cipher = cipher_spec.substr(0, dash_pos);
        mode = cipher_spec.substr(dash_pos + 1);
    }
    
    int r = crypt_init(&cd, device_path.c_str());
    if (r < 0) {
        *error_msg = "Failed to initialize crypt device: " + std::string(strerror(-r));
        return false;
    }
    
    params.label = label.c_str();
    
    r = crypt_format(cd, CRYPT_LUKS2, cipher.c_str(), mode.c_str(),
                    NULL, NULL, 256 / 8, &params);
    
    if (r < 0) {
        *error_msg = "Failed to format LUKS device: " + std::string(strerror(-r));
        crypt_free(cd);
        return false;
    }
    
    r = crypt_keyslot_add_by_volume_key(cd, 0, NULL, 0,
                                       key_data.c_str(), key_data.size());
    
    if (r < 0) {
        *error_msg = "Failed to add key to keyslot: " + std::string(strerror(-r));
        crypt_free(cd);
        return false;
    }
    
    crypt_free(cd);
    return true;
}

bool CryptOpenNative(const std::string& device_path, const std::string& mapped_name,
                    const std::string& key_data, std::string* error_msg) {
    struct crypt_device *cd = NULL;
    
    int r = crypt_init(&cd, device_path.c_str());
    if (r < 0) {
        *error_msg = "Failed to initialize crypt device: " + std::string(strerror(-r));
        return false;
    }
    
    r = crypt_load(cd, CRYPT_LUKS, NULL);
    if (r < 0) {
        *error_msg = "Failed to load LUKS header: " + std::string(strerror(-r));
        crypt_free(cd);
        return false;
    }
    
    r = crypt_activate_by_passphrase(cd, mapped_name.c_str(), CRYPT_ANY_SLOT,
                                     key_data.c_str(), key_data.size(), 0);
    
    if (r < 0) {
        *error_msg = "Failed to activate device: " + std::string(strerror(-r));
        crypt_free(cd);
        return false;
    }
    
    crypt_free(cd);
    return true;
}

bool CryptSetPasswordNative(const std::string& device_path, const std::string& hw_key,
                           const std::string& user_passphrase, bool remove_passphrase,
                           std::string* error_msg) {
    struct crypt_device *cd = NULL;
    
    int r = crypt_init(&cd, device_path.c_str());
    if (r < 0) {
        *error_msg = "Failed to initialize crypt device: " + std::string(strerror(-r));
        return false;
    }
    
    r = crypt_load(cd, CRYPT_LUKS, NULL);
    if (r < 0) {
        *error_msg = "Failed to load LUKS header: " + std::string(strerror(-r));
        crypt_free(cd);
        return false;
    }
    
    if (remove_passphrase) {
        r = crypt_keyslot_destroy(cd, 1);
        if (r < 0 && r != -ENOENT) {
            *error_msg = "Failed to remove passphrase: " + std::string(strerror(-r));
            crypt_free(cd);
            return false;
        }
    } else {
        crypt_keyslot_destroy(cd, 1);  // Remove existing slot 1
        
        r = crypt_keyslot_add_by_passphrase(cd, 1,
                                           hw_key.c_str(), hw_key.size(),
                                           user_passphrase.c_str(), user_passphrase.size());
        
        if (r < 0) {
            *error_msg = "Failed to add passphrase: " + std::string(strerror(-r));
            crypt_free(cd);
            return false;
        }
    }
    
    crypt_free(cd);
    return true;
}

#endif // HAVE_LIBCRYPTSETUP

