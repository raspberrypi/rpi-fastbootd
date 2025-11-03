# IDP JSON Examples

This directory contains example IDP (Interactive Device Provisioning) JSON files demonstrating various device configurations with dm-verity and LUKS encryption.

---

## Available Examples

### 1. `ab_system_verity_luks.json` 
**Complete A/B system with dm-verity and LUKS**

- A/B slotted partitions (boot, system, vendor)
- dm-verity appended mode for all slots
- LUKS2 encrypted userdata (shared between slots)
- Production-ready configuration

**Use case:** Android-style device with seamless updates and verified boot

**Provision:**
```bash
fastboot stage ab_system_verity_luks.json
fastboot oem idpinit
fastboot oem idpwrite

# Flash both slots
for slot in a b; do
    fastboot flash boot_${slot} images/boot.img
    fastboot oem verityappend boot_${slot} 260046848
    
    fastboot flash system_${slot} images/system.img
    fastboot oem verityappend system_${slot} 2040109056
    
    fastboot flash vendor_${slot} images/vendor.img
    fastboot oem verityappend vendor_${slot} 1020054528
done

fastboot oem idpdone
```

---

### 2. `simple_verified_system.json`
**Simple non-A/B verified system**

- Single boot partition (UEFI)
- Single rootfs partition with dm-verity appended
- LUKS2 encrypted userdata
- Minimal configuration

**Use case:** Embedded Linux device with verified boot, no OTA updates

**Provision:**
```bash
fastboot stage simple_verified_system.json
fastboot oem idpinit
fastboot oem idpwrite

fastboot flash rootfs images/rootfs.img
fastboot oem verityappend rootfs 2040109056

fastboot oem idpdone
```

---

### 3. `separate_hash_example.json`
**System with separate dm-verity hash storage**

- Hash trees stored in `/persistent/` directory
- Useful for retrofitting verification
- NVMe storage configuration
- Mix of verified and unverified partitions

**Use case:** Adding verification to existing system without repartitioning

**Provision:**
```bash
fastboot stage separate_hash_example.json
fastboot oem idpinit
fastboot oem idpwrite

fastboot flash system images/system.img
fastboot oem veritysetup system  # Separate mode

fastboot flash config images/config.img
fastboot oem veritysetup config  # Separate mode

fastboot oem idpdone
```

---

### 4. `android_style_complete.json`
**Complete Android-style device**

- A/B slots for boot, system, vendor, product
- All system partitions dm-verity verified
- Misc partition for boot control metadata
- Metadata partition for encryption keys
- Cache partition for OTA updates
- LUKS2 encrypted userdata
- Comprehensive metadata

**Use case:** Full production Android/embedded device

**Provision:**
```bash
fastboot stage android_style_complete.json
fastboot oem idpinit
fastboot oem idpwrite

# Use automated provisioning script
./provision_android_ab.sh
```

---

## JSON Schema Reference

### Device Section

```json
{
  "device": {
    "class": "rpi5",           // Device class
    "variant": "8GB",          // RAM/storage variant
    "storage": {
      "type": "emmc",          // sd, emmc, nvme
      "sector_size": 512,      // Sector size in bytes
      "capacity": 32           // Total capacity in GB
    }
  }
}
```

### Partition Table Section

```json
{
  "layout": {
    "partitiontable": {
      "label": "gpt",                  // dos or gpt
      "id": "uuid-here",               // GPT disk UUID
      "alignment": 4194304,            // 4MB alignment
      "slots": {                       // A/B configuration (optional)
        "enabled": true,
        "count": 2,
        "suffix": ["_a", "_b"],
        "default_slot": "a"
      }
    }
  }
}
```

### Partition Definition

#### Basic Partition
```json
{
  "num": 1,                    // Partition number
  "size": 268435456,           // Size in bytes (or "expand_to_fit": true)
  "typecode": "linux-guid",    // GPT type GUID or name
  "gptlabel": "boot",          // Partition label
  "simg": "boot.simg"          // Sparse image file
}
```

#### Slotted Partition (A/B)
```json
{
  "name": "system",            // Base name (creates system_a, system_b)
  "slotted": true,
  "size": 2147483648,
  "typecode": "linux",
  "simg": "system.simg"
}
```

#### dm-verity Appended
```json
{
  "verity": {
    "enabled": true,
    "mode": "appended",
    "data_size": 2040109056,   // Size of data area (rest is hash)
    "hash_algorithm": "sha256",
    "block_size": 4096         // Optional, default 4096
  }
}
```

#### dm-verity Separate
```json
{
  "verity": {
    "enabled": true,
    "mode": "separate",
    "hash_path": "/persistent/system.verity",
    "hash_algorithm": "sha256"
  }
}
```

#### LUKS Encryption
```json
{
  "luks": {
    "version": 2,
    "cipher": "aes-xts-plain64",
    "key_size": 256,
    "hash": "sha256",
    "sector_size": 512,
    "mname": "userdata_crypt",
    "label": "encrypted"
  }
}
```

---

## Partition Type Codes (GPT)

Common GPT type GUIDs:

| Type | GUID | Usage |
|------|------|-------|
| UEFI System | `c12a7328-f81f-11d2-ba4b-00a0c93ec93b` | EFI boot |
| Linux filesystem | `0fc63daf-8483-4772-8e79-3d69d8477de4` | General Linux |
| Linux swap | `0657fd6d-a4ab-43c4-84e5-0933c84b4f4f` | Swap |
| Linux /home | `933ac7e1-2eb4-4f13-b844-0e14e2aef915` | Home dir |
| Linux LUKS | `ca7d7ccb-63ed-4c53-861c-1742536059cc` | LUKS encrypted |

Or use shorthand names (IDP resolves these):
- `uefi`, `esp` → UEFI System
- `linux`, `L` → Linux filesystem
- `swap`, `S` → Linux swap
- `home`, `H` → Linux /home

---

## Calculating Data Size for Appended dm-verity

When using `mode: "appended"`, you need to specify how much space is for data vs hash tree.

**Formula:**
```
data_size = total_partition_size - hash_tree_size
hash_tree_size ≈ total_partition_size * 0.01  (1% overhead)

Therefore:
data_size ≈ total_partition_size * 0.99
```

**Examples:**

| Partition Size | Data Size (99%) | Hash Size (~1%) |
|----------------|-----------------|-----------------|
| 256 MB | 253,493,248 | ~2,506,752 |
| 512 MB | 506,986,496 | ~5,013,504 |
| 1 GB | 1,020,054,528 | ~10,737,920 |
| 2 GB | 2,040,109,056 | ~21,475,840 |
| 4 GB | 4,080,218,112 | ~42,951,680 |

**Calculate precisely:**
```bash
#!/bin/bash
PARTITION_SIZE=2147483648  # 2GB
HASH_OVERHEAD=0.01
DATA_SIZE=$(echo "$PARTITION_SIZE * (1 - $HASH_OVERHEAD)" | bc)
echo "Partition: $PARTITION_SIZE bytes"
echo "Data size: ${DATA_SIZE%.*} bytes"
```

---

## Provisioning Workflow

### Standard Flow

```bash
# 1. Upload JSON description
fastboot stage device_config.json

# 2. Initialize IDP
fastboot oem idpinit

# 3. Create partitions
fastboot oem idpwrite

# 4. Flash images
while true; do
    INFO=$(fastboot oem idpgetblk 2>&1)
    echo "$INFO" | grep -q "IDP:done" && break
    
    DEVICE=$(echo "$INFO" | grep "INFO" | cut -d: -f2 | awk '{print $1}')
    SIMG=$(echo "$INFO" | grep "INFO" | cut -d: -f3 | awk '{print $1}')
    
    fastboot flash $DEVICE images/$SIMG
    fastboot oem idpverity  # Setup dm-verity if configured
done

# 5. Finalize
fastboot oem idpdone

# 6. Download root hashes
fastboot oem upload-file /persistent/idp_verity_hashes.json
fastboot upload hashes.json
```

### A/B System Flow

```bash
# Same as above, but flash both slots
fastboot stage ab_config.json
fastboot oem idpinit
fastboot oem idpwrite

# Flash slot A
fastboot flash boot_a images/boot.img
fastboot oem verityappend boot_a 260046848

fastboot flash system_a images/system.img
fastboot oem verityappend system_a 2040109056

# Flash slot B (initially same images)
fastboot flash boot_b images/boot.img
fastboot oem verityappend boot_b 260046848

fastboot flash system_b images/system.img
fastboot oem verityappend system_b 2040109056

fastboot oem idpdone
fastboot --set-active=a
```

---

## Validation

### Validate JSON Schema

```bash
# Check JSON syntax
jq empty device_config.json && echo "Valid JSON" || echo "Invalid JSON"

# Validate IDP schema (if validator available)
idp-validator device_config.json
```

### Calculate Total Size

```bash
#!/bin/bash
# Calculate total partition size from JSON

jq '.layout.partitions[] | 
    select(.expand_to_fit != true) | 
    .size' device_config.json | 
    awk '{sum+=$1} END {print "Total:", sum, "bytes", "(" sum/1024/1024/1024 "GB)"}'
```

---

## Security Notes

### Root Hash Storage

After provisioning, root hashes are saved to `/persistent/idp_verity_hashes.json`. These MUST be stored securely:

**Good:**
- Bootloader environment (read-only)
- TPM / Secure Element
- Signed configuration file
- OTP memory

**Bad:**
- Unprotected filesystem ❌
- Same partition as data ❌

### Boot Integration

The bootloader must:
1. Read active slot (A or B)
2. Load root hashes for active slot
3. Pass to kernel via cmdline

Example bootloader integration:
```bash
# In bootloader (U-Boot, GRUB, etc.)
SLOT=$(cat /persistent/ab_metadata | jq -r .current_slot)
ROOT_HASH=$(cat /persistent/idp_verity_hashes.json | 
            jq -r ".partitions[] | 
            select(.name == \"system_${SLOT}\") | 
            .root_hash")

# Add to kernel cmdline
setenv bootargs "... verity.system.root_hash=${ROOT_HASH}"
```

---

## Troubleshooting

### "Invalid partition size"
- Check that sizes are multiples of alignment (default 4MB)
- Ensure total size doesn't exceed device capacity

### "Data size exceeds partition size"
- For appended verity, data_size must be less than partition size
- Leave ~1-2% for hash tree

### "LUKS format failed"
- Ensure partition is large enough (minimum ~32MB for LUKS2)
- Check that device is not mounted

### "IDP parser error"
- Validate JSON syntax with `jq`
- Check all required fields are present
- Ensure typecodes are valid GUIDs

---

## Creating Your Own

### Template

```json
{
  "description": "My Device Configuration",
  "version": "1.0.0",
  
  "device": {
    "class": "YOUR_DEVICE",
    "variant": "VARIANT",
    "storage": {
      "type": "emmc",
      "capacity": 32
    }
  },
  
  "layout": {
    "partitiontable": {
      "label": "gpt",
      "alignment": 4194304
    },
    
    "partitions": [
      {
        "num": 1,
        "size": SIZE_IN_BYTES,
        "typecode": "TYPE_GUID",
        "gptlabel": "NAME",
        "simg": "image.simg"
      }
    ]
  }
}
```

### Checklist

- [ ] Device class and storage type correct
- [ ] Partition sizes calculated (including alignment)
- [ ] Total size fits within device capacity
- [ ] Type codes are valid GUIDs
- [ ] Sparse images (.simg) exist
- [ ] dm-verity data_size leaves room for hash (~1%)
- [ ] LUKS partitions have sufficient size
- [ ] Slotted partitions have "slotted": true
- [ ] Non-slotted partitions have "slotted": false
- [ ] Last partition can use "expand_to_fit": true

---

## Additional Resources

- [IDP_VERITY_INTEGRATION.md](../IDP_VERITY_INTEGRATION.md) - Integration guide
- [IDP_VERITY_AB_SYSTEM.md](../IDP_VERITY_AB_SYSTEM.md) - A/B system design
- [DM_VERITY_SETUP.md](../DM_VERITY_SETUP.md) - dm-verity documentation
- [VERITY_MODES_COMPARISON.md](../VERITY_MODES_COMPARISON.md) - Appended vs separate modes

---

## Quick Reference

### Provision A/B System
```bash
fastboot stage examples/ab_system_verity_luks.json
fastboot oem idpinit && fastboot oem idpwrite
./scripts/flash_ab_system.sh
fastboot oem idpdone
```

### Provision Simple System
```bash
fastboot stage examples/simple_verified_system.json
fastboot oem idpinit && fastboot oem idpwrite
./scripts/flash_simple_system.sh
fastboot oem idpdone
```

### View Current Config
```bash
fastboot getvar partition-table
fastboot getvar slot-count
fastboot getvar current-slot
```

