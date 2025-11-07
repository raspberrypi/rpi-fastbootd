# dm-verity Modes: Separate vs Appended

Quick reference for choosing the right dm-verity mode.

---

## Two Commands Available

### 1. oem veritysetup (Separate Hash Storage)

```bash
fastboot oem veritysetup mmcblk0p2
```

**Storage:**
```
/dev/mmcblk0p2              → Data device (full size used)
/persistent/mmcblk0p2.verity → Hash tree (separate file)
```

**Use when:**
- Need flexible hash storage location
- Want to update hash tree independently
- Using external storage for hashes
- Retrofitting verification to existing system

---

### 2. oem verityappend (Appended Hash Storage) ⭐ ANDROID-STYLE

```bash
fastboot oem verityappend mmcblk0p2 943718400
```

**Storage:**
```
/dev/mmcblk0p2: [====data (943MB)====][hash (~7MB)]
```

**Use when:**
- Building Android-style system partitions
- Want self-contained verified storage
- Need atomic updates (data + hash together)
- Following embedded Linux best practices

---

## Quick Comparison

| Feature | Separate | Appended |
|---------|----------|----------|
| **Command** | `oem veritysetup <device>` | `oem verityappend <device> <size>` |
| **Arguments** | 1 (device name) | 2 (device + data size) |
| **Storage** | 2 locations | 1 location |
| **Device size** | Full device for data | Data + hash (~1% extra) |
| **Flexibility** | High | Medium |
| **Android standard** | No | Yes ✓ |
| **Self-contained** | No | Yes ✓ |
| **Update complexity** | Must sync 2 files | Single atomic update |

---

## Examples

### Separate Mode

```bash
# Simple - no size calculation needed
fastboot oem veritysetup mmcblk0p2

# Outputs:
# Root hash: abc123...
# Hash stored in: /persistent/mmcblk0p2.verity

# Activate:
veritysetup open /dev/mmcblk0p2 verified \
  /persistent/mmcblk0p2.verity <roothash>
```

### Appended Mode

```bash
# Need to specify data size (rest is for hash tree)
fastboot oem verityappend mmcblk0p2 943718400

# Outputs:
# Device size: 1073741824 bytes (1GB total)
# Data size: 943718400 bytes (900MB data)
# Hash tree appended at offset: 943718400
# Root hash: abc123...

# Activate (note: same device for data and hash):
veritysetup open /dev/mmcblk0p2 verified \
  /dev/mmcblk0p2 <roothash> --hash-offset=943718400
```

---

## Planning Your Partition

### Separate Mode
```bash
# Device size = data size (full device used)
DEVICE_SIZE=1073741824  # 1GB
fastboot oem partapp mmcblk0 linux $DEVICE_SIZE
fastboot oem veritysetup mmcblk0p2
# Hash goes to /persistent/mmcblk0p2.verity
```

### Appended Mode
```bash
# Device size = data size + ~1% for hash
DATA_SIZE=943718400      # 900MB
DEVICE_SIZE=1073741824   # 1GB (900MB + 100MB buffer)

fastboot oem partapp mmcblk0 linux $DEVICE_SIZE
fastboot oem verityappend mmcblk0p2 $DATA_SIZE
# Hash stored in device after byte 943718400
```

---

## Which Should I Use?

### Choose **Separate** (`veritysetup`) if:
- ✓ Adding verification to existing partitions
- ✓ Don't want to resize partitions
- ✓ Need hash tree on different storage
- ✓ Want to update hash tree independently
- ✓ Building custom Linux distribution

### Choose **Appended** (`verityappend`) if:
- ✓ Building Android-compatible system
- ✓ Creating new verified partition from scratch
- ✓ Want self-contained storage
- ✓ Need atomic updates
- ✓ Following industry standards
- ✓ Building embedded firmware

---

## Common Workflow

### Typical Android System Partition (Appended)

```bash
#!/bin/bash
# 1. Calculate sizes
DATA_SIZE=$((2048 * 1024 * 1024))      # 2GB data
HASH_SIZE=$((DATA_SIZE / 50))          # ~2% overhead
TOTAL_SIZE=$((DATA_SIZE + HASH_SIZE))  # 2.04GB total

# 2. Create partition
fastboot oem partapp mmcblk0 linux $TOTAL_SIZE

# 3. Flash system image
fastboot flash mmcblk0p2 system.img

# 4. Setup dm-verity (hash appended)
fastboot oem verityappend mmcblk0p2 $DATA_SIZE

# 5. Store root hash in bootloader
# (secure storage - outside scope)
```

### Typical Custom Linux (Separate)

```bash
#!/bin/bash
# 1. Use existing partition
DEVICE="mmcblk0p2"

# 2. Flash rootfs
fastboot flash $DEVICE rootfs.img

# 3. Setup dm-verity (hash in /persistent)
fastboot oem veritysetup $DEVICE

# 4. Hash stored in /persistent/mmcblk0p2.verity
# Can be backed up, moved, etc.
```

---

## Implementation Notes

Both modes use the same underlying library (`libcryptsetup`) with different parameters:

**Separate:**
```cpp
params.data_device = "/dev/mmcblk0p2";
params.hash_device = "/persistent/mmcblk0p2.verity";  // Different
params.hash_area_offset = 0;
```

**Appended:**
```cpp
params.data_device = "/dev/mmcblk0p2";
params.hash_device = "/dev/mmcblk0p2";  // Same!
params.hash_area_offset = data_size;    // Offset within same device
params.data_size = data_blocks;         // Limit data area
```

---

## Files Created

| Mode | Data | Hash Tree |
|------|------|-----------|
| Separate | `/dev/mmcblk0p2` (full device) | `/persistent/mmcblk0p2.verity` (file) |
| Appended | `/dev/mmcblk0p2` (first N bytes) | `/dev/mmcblk0p2` (appended after N bytes) |

---

## Summary

- **Most users (Android/embedded)**: Use `oem verityappend`
- **Custom Linux distributions**: Use `oem veritysetup`
- **Both are equally secure** - just different storage layouts
- **Both use the same verification algorithm** (SHA-256, 4KB blocks)

Choose based on your use case and preferred storage layout! 🎯


