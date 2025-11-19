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
                    NULL, NULL, 512 / 8, &params);
    
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

bool VeritySetupNative(const std::string& data_device, const std::string& hash_device,
                      std::string* root_hash, std::string* error_msg) {
    struct crypt_device *cd = NULL;
    struct crypt_params_verity params = {};
    
    // Set up dm-verity parameters
    params.hash_name = "sha256";           // Hash algorithm
    params.data_device = data_device.c_str();
    params.hash_device = hash_device.c_str();  // Separate hash device
    params.fec_device = NULL;              // No FEC (Forward Error Correction)
    params.salt = NULL;                    // Auto-generate salt
    params.salt_size = 32;                 // 32-byte salt
    params.hash_type = 1;                  // Format type 1 (normal)
    params.data_block_size = 4096;         // 4K blocks
    params.hash_block_size = 4096;         // 4K hash blocks
    params.data_size = 0;                  // Auto-detect from device
    params.hash_area_offset = 0;           // Start at beginning of hash device
    params.fec_area_offset = 0;
    params.fec_roots = 0;
    params.flags = CRYPT_VERITY_CREATE_HASH;  // Generate hash tree
    
    // Initialize using the hash device (where the hash tree will be stored)
    int r = crypt_init(&cd, hash_device.c_str());
    if (r < 0) {
        *error_msg = "Failed to initialize verity device: " + std::string(strerror(-r));
        return false;
    }
    
    // Format with VERITY (creates the hash tree in the hash_device)
    r = crypt_format(cd, CRYPT_VERITY, NULL, NULL, NULL, NULL, 0, &params);
    if (r < 0) {
        *error_msg = "Failed to format verity device: " + std::string(strerror(-r));
        crypt_free(cd);
        return false;
    }
    
    // Get the volume key (which is the root hash for verity)
    size_t key_size = crypt_get_volume_key_size(cd);
    if (key_size == 0) {
        *error_msg = "Failed to get root hash size";
        crypt_free(cd);
        return false;
    }
    
    char* key_buffer = new char[key_size];
    r = crypt_volume_key_get(cd, CRYPT_ANY_SLOT, key_buffer, &key_size, NULL, 0);
    if (r < 0) {
        *error_msg = "Failed to get root hash: " + std::string(strerror(-r));
        delete[] key_buffer;
        crypt_free(cd);
        return false;
    }
    
    // Convert root hash to hex string
    root_hash->clear();
    root_hash->reserve(key_size * 2);
    for (size_t i = 0; i < key_size; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (unsigned char)key_buffer[i]);
        root_hash->append(hex);
    }
    
    delete[] key_buffer;
    crypt_free(cd);
    return true;
}

bool VeritySetupAppendedNative(const std::string& device, uint64_t data_size,
                               std::string* root_hash, std::string* error_msg) {
    struct crypt_device *cd = NULL;
    struct crypt_params_verity params = {};
    
    // Calculate hash tree size
    // For dm-verity with 4K blocks and SHA-256:
    // - Each hash block contains 4096 / 32 = 128 hashes
    // - Hash tree is logarithmic in data size
    uint64_t block_size = 4096;
    uint64_t hash_size = 32;  // SHA-256
    uint64_t hashes_per_block = block_size / hash_size;  // 128
    
    // Calculate number of data blocks
    uint64_t data_blocks = (data_size + block_size - 1) / block_size;
    
    // Calculate hash tree size (simplified - actual calculation is more complex)
    // Level 0: data_blocks hashes
    // Level 1: (data_blocks + hashes_per_block - 1) / hashes_per_block hashes
    // etc.
    uint64_t hash_blocks = 0;
    uint64_t level_blocks = data_blocks;
    while (level_blocks > 1) {
        level_blocks = (level_blocks + hashes_per_block - 1) / hashes_per_block;
        hash_blocks += level_blocks;
    }
    
    uint64_t hash_area_offset = data_blocks * block_size;
    
    // Set up dm-verity parameters for appended hash
    params.hash_name = "sha256";
    params.data_device = device.c_str();
    params.hash_device = device.c_str();  // Same device!
    params.fec_device = NULL;
    params.salt = NULL;
    params.salt_size = 32;
    params.hash_type = 1;
    params.data_block_size = block_size;
    params.hash_block_size = block_size;
    params.data_size = data_blocks;  // Limit data area
    params.hash_area_offset = hash_area_offset;  // Hash starts after data
    params.fec_area_offset = 0;
    params.fec_roots = 0;
    params.flags = CRYPT_VERITY_CREATE_HASH;
    
    // Initialize with the device
    int r = crypt_init(&cd, device.c_str());
    if (r < 0) {
        *error_msg = "Failed to initialize verity device: " + std::string(strerror(-r));
        return false;
    }
    
    // Format with VERITY (creates the hash tree appended to data)
    r = crypt_format(cd, CRYPT_VERITY, NULL, NULL, NULL, NULL, 0, &params);
    if (r < 0) {
        *error_msg = "Failed to format verity device: " + std::string(strerror(-r));
        crypt_free(cd);
        return false;
    }
    
    // Get the volume key (root hash)
    size_t key_size = crypt_get_volume_key_size(cd);
    if (key_size == 0) {
        *error_msg = "Failed to get root hash size";
        crypt_free(cd);
        return false;
    }
    
    char* key_buffer = new char[key_size];
    r = crypt_volume_key_get(cd, CRYPT_ANY_SLOT, key_buffer, &key_size, NULL, 0);
    if (r < 0) {
        *error_msg = "Failed to get root hash: " + std::string(strerror(-r));
        delete[] key_buffer;
        crypt_free(cd);
        return false;
    }
    
    // Convert root hash to hex string
    root_hash->clear();
    root_hash->reserve(key_size * 2);
    for (size_t i = 0; i < key_size; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (unsigned char)key_buffer[i]);
        root_hash->append(hex);
    }
    
    delete[] key_buffer;
    crypt_free(cd);
    return true;
}

#endif // HAVE_LIBCRYPTSETUP

