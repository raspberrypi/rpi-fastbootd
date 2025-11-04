# Raspberry Pi Fastbootd

**Version:** 14.0.0  
**Based on:** Android 14 AOSP Fastboot  
**License:** Apache 2.0 (default), GPLv3 (when built with libcryptsetup)

---

## Overview

This is a heavily modified version of the Android Open Source Project (AOSP) fastboot daemon (`fastbootd`), customized for Raspberry Pi secure boot and device provisioning workflows. While maintaining compatibility with the standard fastboot protocol, it adds extensive Raspberry Pi-specific functionality for partition management, encryption, dm-verity, GPIO control, and Image Description Provisioning (IDP).

### Client vs Device Components

⚠️ **Important:** This repository contains both client and device-side software from AOSP, but **we only build the device-side components**:

| Component | Description | Build Status | Where to Get |
|-----------|-------------|--------------|--------------|
| **fastbootd** (device) | Daemon running on Raspberry Pi | ✅ **Built by this repo** | This project |
| **fastboot** (client) | Command-line tool on host PC | ❌ Not built | Use OS-provided package |

**What this means:**
- This project builds `fastbootd` to run **on your Raspberry Pi**
- You need a separate `fastboot` client installed **on your host computer** (Linux/Mac/Windows)
- The client and device communicate over USB or TCP/IP using the fastboot protocol

**Getting the fastboot client:**
```bash
# Debian/Ubuntu
sudo apt-get install fastboot

# macOS (Homebrew)
brew install android-platform-tools

# Windows
# Download Android SDK Platform Tools from:
# https://developer.android.com/tools/releases/platform-tools
```

**Typical setup:**
```
┌─────────────────────┐          USB/Network          ┌─────────────────────┐
│   Host Computer     │ ◄─────────────────────────► │   Raspberry Pi      │
│                     │                               │                     │
│  fastboot (client)  │  fastboot protocol commands   │  fastbootd (daemon) │
│  [OS-provided]      │  ───────────────────────────► │  [This project]     │
│                     │                               │                     │
└─────────────────────┘                               └─────────────────────┘
     Linux/Mac/Windows                                    arm64 Device
```

### Key Modifications

- **Image Description Provisioning (IDP)** - JSON-driven partition layout and provisioning
- **Native LUKS2 encryption** - Full disk encryption via libcryptsetup
- **dm-verity support** - Read-only partition integrity verification
- **GPIO control** - Hardware control via libgpiod
- **File transfer** - Direct file upload/download to/from device
- **Raspberry Pi OTP integration** - Hardware key management
- **Network optimization** - Native syscalls instead of process spawning
- **Catch2 v3.11.0 test suite** - Comprehensive unit and integration tests

---

## Integration with rpi-sb-provisioner

This fastbootd implementation is designed to work with the **rpi-sb-provisioner** host tool for high-level provisioning workflows.

### Typical Provisioning Workflow

**Prerequisites:**
- `fastbootd` running on the Raspberry Pi (built from this repo)
- `fastboot` client installed on your host computer (from OS package)
- USB or network connection between host and device

**Image Description Provisioning (IDP) example:**

```bash
# 1. Upload device configuration JSON
fastboot stage device_config.json
fastboot oem idpinit

# 2. Create partitions from JSON
fastboot oem idpwrite

# 3. Flash images and setup verification
fastboot flash system system.img
fastboot oem verityappend system 2040109056

fastboot flash boot boot.img
fastboot oem verityappend boot 260046848

# 4. Setup encryption on userdata
fastboot oem cryptinit /dev/mmcblk0p3 userdata aes-xts-plain64

# 5. Finalize provisioning
fastboot oem idpdone

# 6. Download verification hashes
fastboot oem upload-file /persistent/idp_verity_hashes.json
fastboot upload hashes.json
```

**Where each component runs:**
- `fastboot` client → Host computer (communicates over USB/network)
- `fastbootd` daemon → Raspberry Pi (receives and executes commands)

---

## New OEM Commands

This fastbootd adds the following OEM commands beyond standard AOSP:

### Image Description Provisioning (IDP)

| Command | Description | Example |
|---------|-------------|---------|
| `oem idpinit` | Initialize IDP from staged JSON | `fastboot stage config.json && fastboot oem idpinit` |
| `oem idpwrite` | Create all partitions from IDP config | `fastboot oem idpwrite` |
| `oem idpgetblk` | Get next partition to flash | `fastboot oem idpgetblk` |
| `oem idpdone` | Finalize IDP and save metadata | `fastboot oem idpdone` |

**See:** IDP documentation in the fastboot directory for JSON schema details

### LUKS Encryption

| Command | Description | Example |
|---------|-------------|---------|
| `oem cryptinit <dev> <label> [cipher]` | Format partition with LUKS2 | `fastboot oem cryptinit /dev/mmcblk0p3 userdata` |
| `oem cryptopen <dev> <name>` | Open encrypted partition | `fastboot oem cryptopen /dev/mmcblk0p3 userdata_crypt` |
| `oem cryptsetpassword <dev> <pass>` | Change LUKS passphrase | `fastboot oem cryptsetpassword /dev/mmcblk0p3 newpass` |

**Default cipher:** `aes-xts-plain64` (256-bit)

### dm-verity (Integrity Verification)

| Command | Description | Example |
|---------|-------------|---------|
| `oem veritysetup <device>` | Calculate hash tree (separate file) | `fastboot oem veritysetup system` |
| `oem verityappend <device> <data_size>` | Append hash tree to device | `fastboot oem verityappend system 2040109056` |

**Root hash:** Saved to `/persistent/<device>.verity.roothash` or output in command response

### File Transfer

| Command | Description | Example |
|---------|-------------|---------|
| `oem download-file <dest>` | Write staged data to file | `fastboot stage keys.bin && fastboot oem download-file /persistent/keys.bin` |
| `oem upload-file <path>` | Stage file for host download | `fastboot oem upload-file /persistent/hashes.json && fastboot upload hashes.json` |

### GPIO Control

| Command | Description | Example |
|---------|-------------|---------|
| `oem gpioset <line>=<value>` | Set GPIO line state | `fastboot oem gpioset 23=1` |

**Requires:** libgpiod on device

### Legacy Partitioning

| Command | Description | Example |
|---------|-------------|---------|
| `oem partinit <dev> <label> [id]` | Initialize GPT partition table | `fastboot oem partinit mmcblk0 gpt` |
| `oem partapp <dev> <type> <size>` | Append partition | `fastboot oem partapp mmcblk0 L 2147483648` |

**Note:** IDP commands are preferred for new deployments

### LED Control

| Command | Description | Example |
|---------|-------------|---------|
| `oem led <pattern>` | Control status LED | `fastboot oem led BOOT` |

---

## Building

### Prerequisites

**On the build machine (can be the Raspberry Pi or cross-compile host):**

```bash
# Debian/Ubuntu (arm64)
sudo apt-get install \
    build-essential cmake git \
    android-liblog-dev android-libbase-dev android-libcutils-dev \
    android-libsparse-dev libfdisk-dev liburing-dev libsystemd-dev \
    zlib1g-dev libgpiod-dev libcryptsetup-dev libssl-dev
```

**On your host computer (for running fastboot commands):**

```bash
# Debian/Ubuntu
sudo apt-get install fastboot android-sdk-platform-tools

# macOS
brew install android-platform-tools

# Windows - Download Android SDK Platform Tools
# https://developer.android.com/tools/releases/platform-tools
```

### Quick Build

```bash
git clone https://github.com/raspberrypi/android_platform_system_core
cd android_platform_system_core
cmake -B build -S .
cmake --build build -j$(nproc)
```

**Output:** `build/fastboot/fastbootd` (device daemon only)

**Note:** This builds **only the device-side daemon** (`fastbootd`). The client tool (`fastboot`) should be installed from your OS package manager as shown in Prerequisites.

---

## Licensing

### Default Build (Apache 2.0)

By default, fastbootd is licensed under the **Apache License 2.0**, matching the upstream AOSP fastbootd.

```bash
# Build without libcryptsetup (Apache 2.0 only)
cmake -B build -S . -DCRYPTSETUP_FOUND=OFF
cmake --build build
```

**Result:** Binary licensed under Apache 2.0 ✅

### Building with libcryptsetup (GPLv3)

⚠️ **Important:** If you link against `libcryptsetup`, the entire binary becomes **GPLv3** licensed due to GPLv3's requirements.

```bash
# Build with libcryptsetup (GPLv3)
cmake -B build -S .
cmake --build build
```

**Result:** Binary licensed under GPLv3 due to libcryptsetup linkage ⚠️

### Skipping libcryptsetup (Maintaining Apache 2.0)

To keep Apache 2.0 licensing, disable libcryptsetup at build time:

```bash
# Option 1: CMake flag
cmake -B build -S . -DCRYPTSETUP_FOUND=OFF

# Option 2: Remove libcryptsetup before building
sudo apt-get remove libcryptsetup-dev
cmake -B build -S .
```

**Fallback behavior:** Fastbootd will use the `cryptsetup` command-line tool instead of native library calls. This requires:
- `cryptsetup-bin` package installed on the device
- Slightly slower performance (~10-15% overhead)
- Still fully functional for all crypto operations

**Licensing summary:**
```
Without libcryptsetup → Apache 2.0 ✅
With libcryptsetup    → GPLv3 ⚠️
```

---

## Building Debian Packages

This project uses Debian devscripts (`dpkg-buildpackage`) to generate `.deb` packages.

### Creating Packages

```bash
# Standard build (includes libcryptsetup - GPL-2+/GPL-3+)
./build-package.sh

# Apache-only build (no libcryptsetup)
DEB_BUILD_OPTIONS="nocryptsetup" ./build-package.sh
```

**Output:** `../rpi-fastbootd_14.0.0-<revision>_arm64.deb`

### Package Naming Convention

```
rpi-fastbootd_<VERSION>-<REVISION>_<ARCH>.deb
```

Where:
- **rpi-fastbootd** - Package name (from `debian/control`)
- **VERSION** - `14.0.0` (Android 14 base version)
- **REVISION** - Git commit count since Android 14 baseline (auto-calculated by `debian/gen-version.sh`)
- **ARCH** - `arm64` (target architecture)

**Example:** `rpi-fastbootd_14.0.0-85_arm64.deb`

### Build Options

| Option | Effect | License | Use Case |
|--------|--------|---------|----------|
| Default | Links libcryptsetup, libgpiod | GPL-2+ or GPL-3+ | Full functionality with native crypto |
| `DEB_BUILD_OPTIONS="nocryptsetup"` | Uses command fallbacks | Apache 2.0 | Apache-compatible distribution |

**See:** [BUILDING_LICENSING.md](BUILDING_LICENSING.md) for detailed licensing information

### Customizing Package Metadata

Edit `debian/control`:

```
Source: rpi-fastbootd
Maintainer: Raspberry Pi Signed Boot Team <applications@raspberrypi.com>
Section: admin
Priority: optional
```

### Installation

```bash
# Install package
sudo dpkg -i dist/rpi-fastbootd_14.0.0-247_arm64.deb

# Or during development
sudo cmake --build build --target install
```

**Install location:** `/usr/bin/fastbootd`

---

## Testing

This project includes a comprehensive test suite using Catch2 v3.11.0.

### Build Tests

```bash
cmake -B build -S . -DBUILD_TESTING=ON
cmake --build build --target kernel_log_test gpio_test gateway_test crypto_test verity_test
```

### Run Tests

```bash
cd build/fastboot/device/test

# Run all tests (requires root for some tests)
sudo ./verity_test
sudo ./crypto_test

# Run specific test suites
./gateway_test
./gpio_test
./kernel_log_test

# Run with verbose output
./verity_test -s

# List available tests
./verity_test --list-tests
```

Tests include kernel log, GPIO, gateway, crypto (LUKS), and dm-verity operations.

---

## Using Fastbootd

### Running Fastbootd on the Device

Once built and installed on your Raspberry Pi, fastbootd can run in two modes:

#### USB Mode (Default)

USB mode requires USB gadget support (USB OTG/Device mode):

```bash
# Manual start (requires USB gadget configured)
sudo fastbootd

# Or via systemd (recommended - handles USB setup)
sudo systemctl start fastbootd.service
```

**Connecting from host:**
```bash
# Host computer - USB connection
fastboot devices
fastboot getvar product
```

#### TCP/IP Mode

For network-based provisioning (useful for debugging or when USB isn't available):

```bash
# On Raspberry Pi - start in TCP mode
sudo fastbootd -i tcp

# Or edit systemd service to use TCP
sudo systemctl edit fastbootd.service
# Add: Environment="FASTBOOT_MODE=tcp"

# Fastbootd listens on port 5554
```

**Connecting from host:**
```bash
# Host computer - connect to device IP
fastboot -s tcp:192.168.1.100:5554 devices
fastboot -s tcp:192.168.1.100:5554 getvar product

# Or for repeated commands, use environment variable
export FASTBOOT_DEVICE=tcp:192.168.1.100:5554
fastboot devices
fastboot oem idpinit
```

### Systemd Service

The systemd service handles all the USB gadget setup automatically.

**Installation:**
```bash
# Install systemd files
sudo cmake --install build

# Or manually
sudo cp systemd/system/*.service /etc/systemd/system/
sudo cp systemd/bin/* /usr/local/bin/
sudo systemctl daemon-reload
```

**Usage:**
```bash
# Enable on boot
sudo systemctl enable fastbootd.service

# Start now
sudo systemctl start fastbootd.service

# Check status
sudo systemctl status fastbootd.service

# View logs
sudo journalctl -u fastbootd.service -f

# Stop service
sudo systemctl stop fastbootd.service
```

**Service files:**
- `fastbootd.service` - Main fastboot daemon
- `configfs-init.service` - Sets up USB gadget configfs
- `dev-usb-ffs-fastboot.mount` - Mounts USB FunctionFS
- `fastboot-udc-attach.service` - Attaches to USB Device Controller

### Connection Methods

| Method | Use Case | Command |
|--------|----------|---------|
| **USB** | Production, secure boot | `fastboot devices` |
| **TCP/IP** | Development, debugging, network provisioning | `fastboot -s tcp:IP:5554 devices` |

### Verifying Connection

```bash
# From host computer

# List connected devices
fastboot devices

# Should show something like:
# 1234567890abcdef    fastboot    (for USB)
# 192.168.1.100:5554  fastboot    (for TCP)

# Get device info
fastboot getvar all

# Test custom command
fastboot oem led BOOT
```

### Troubleshooting

**USB Mode Issues:**

```bash
# Check USB gadget is loaded
lsmod | grep usb_f_fs

# Check configfs
ls /sys/kernel/config/usb_gadget/

# Check fastbootd is running
ps aux | grep fastbootd

# Check USB connection on host
lsusb | grep -i fastboot
```

**TCP Mode Issues:**

```bash
# Check fastbootd is listening
sudo netstat -tlnp | grep 5554

# Test connection
telnet 192.168.1.100 5554

# Check firewall
sudo iptables -L | grep 5554
```

**Common Problems:**

| Problem | Solution |
|---------|----------|
| "No devices found" | Check USB cable, drivers, or network connectivity |
| "Insufficient permissions" | Run `fastboot` with sudo or add udev rules |
| "Device offline" | Restart fastbootd: `sudo systemctl restart fastbootd` |
| Can't connect via TCP | Check firewall, ensure fastbootd started with `-i tcp` |

### Configuration

Fastbootd behavior can be customized:

**Via command line:**
```bash
# USB mode (default)
sudo fastbootd

# TCP mode
sudo fastbootd -i tcp

# Custom interface descriptor
STR_INTERFACE_="my-device" sudo fastbootd
```

**Via systemd service override:**
```bash
sudo systemctl edit fastbootd.service
```

Add:
```ini
[Service]
Environment="FASTBOOT_MODE=tcp"
Environment="STR_INTERFACE_=custom-device"
```

**Persistent storage location:**
The `/persistent` directory is used for:
- Encryption keys
- dm-verity hashes
- IDP metadata
- Device configuration

Ensure it's mounted and writable:
```bash
sudo mkdir -p /persistent
sudo chown root:root /persistent
sudo chmod 755 /persistent
```

---

## Architecture Overview

```
┌─────────────────────────────────────────┐
│         rpi-sb-provisioner              │  (Host tool)
│         (High-level API)                │
└─────────────────┬───────────────────────┘
                  │ fastboot protocol (USB/TCP)
┌─────────────────▼───────────────────────┐
│            fastbootd                    │  (Device daemon)
│  ┌─────────────────────────────────┐   │
│  │   OEM Command Handlers          │   │
│  │  • IDP  • Crypto  • Verity      │   │
│  │  • GPIO • Files   • Partitions  │   │
│  └──────────┬──────────────────────┘   │
│             ▼                           │
│  ┌─────────────────────────────────┐   │
│  │   Native Libraries              │   │
│  │  • libcryptsetup (LUKS/verity)  │   │
│  │  • libgpiod (GPIO control)      │   │
│  │  • libfdisk (Partitioning)      │   │
│  └─────────────────────────────────┘   │
└─────────────────────────────────────────┘
```

---

## Documentation

### Protocol Documentation
- [fastboot/PROTOCOL.md](fastboot/PROTOCOL.md) - Original AOSP fastboot protocol specification

### Configuration Documentation
- [fastboot/VERITY_MODES_COMPARISON.md](fastboot/VERITY_MODES_COMPARISON.md) - dm-verity modes comparison
- [BUILDING_LICENSING.md](BUILDING_LICENSING.md) - Build options and licensing guide
- [DEBIAN_PACKAGING.md](DEBIAN_PACKAGING.md) - Debian package build process

---

## Dependencies

### Required (Core)
- `android-liblog` - Android logging library
- `android-libbase` - Android base utilities
- `android-libcutils` - Android common utilities
- `android-libsparse` - Sparse image support
- `libfdisk1` - Partition management
- `liburing2` - Async I/O for USB
- `libsystemd0` - Systemd integration
- `zlib1g` - Compression support
- `openssl` - Cryptographic functions

### Optional (Native APIs)
- `libgpiod2` - GPIO control (falls back to `gpioset` command)
- `libcryptsetup12` - LUKS/dm-verity (falls back to `cryptsetup` command, **causes GPLv3 licensing**)

### Fallback Commands (Suggested)
- `net-tools` - Network utilities fallback
- `iproute2` - Network routing fallback
- `util-linux` - System utilities fallback
- `gpiod` - GPIO command fallback
- `cryptsetup-bin` - Crypto command fallback (no GPL issues when used as external command)

---

## Differences from AOSP Fastboot

| Feature | AOSP Fastboot | RPi Fastbootd |
|---------|---------------|---------------|
| **IDP Provisioning** | ❌ No | ✅ Full JSON-driven workflow |
| **LUKS Encryption** | ❌ No | ✅ Native libcryptsetup integration |
| **dm-verity** | ⚠️ Basic | ✅ Full setup + appended mode |
| **GPIO Control** | ❌ No | ✅ Native libgpiod |
| **File Transfer** | ⚠️ Limited | ✅ Direct upload/download |
| **Network Calls** | ⚠️ Process spawning | ✅ Native syscalls |
| **Test Suite** | ⚠️ Limited | ✅ Comprehensive Catch2 |
| **A/B Updates** | ✅ Yes | ✅ Yes + verification |
| **OTP Integration** | ❌ No | ✅ Raspberry Pi OTP |

---

## Platform Support

**Tested on:**
- Raspberry Pi 5 (BCM2712)
- Raspberry Pi 4 (BCM2711)
- Raspberry Pi Compute Module 4

**Architecture:** ARM64 only

---

## Contributing

This is a Raspberry Pi Ltd project for secure boot and device provisioning.

**Contact:**
- Raspberry Pi Signed Boot Team
- applications@raspberrypi.com

---

## License

This software is based on the Android 14 fastboot implementation. The **source code** is licensed under **Apache License 2.0**.

### Binary License (Build-Dependent)

The **compiled binary's license** depends on what libraries are linked at build time:

#### Default Build: GPL-2+ / GPL-3+
When built with `libcryptsetup` (default):
- **License**: GPL-2+ (or GPL-3+ depending on libcryptsetup version)
- **Reason**: Dynamic linking with GPL-licensed libraries:
  - `libcryptsetup12` (GPL-2+)
  - `libgpiod` (GPL-2+)
- **Build command**: Normal `./build-package.sh` or `dpkg-buildpackage`

#### Apache-Only Build: Apache 2.0
When built without `libcryptsetup`:
- **License**: Apache 2.0
- **Tradeoff**: Uses command-line fallbacks (`cryptsetup`, `gpioset`) instead of native library integration
- **Build commands**:
  ```bash
  # Debian package:
  DEB_BUILD_OPTIONS="nocryptsetup" ./build-package.sh
  
  # CMake directly:
  cmake -DSKIP_CRYPTSETUP=ON ..
  ```

The build system automatically detects which libraries are linked and generates a `LICENSE_INFO` file indicating the effective license of the compiled binary.

**For detailed licensing information, build instructions, and verification steps, see [`BUILDING_LICENSING.md`](BUILDING_LICENSING.md).**

### License Texts

- **Apache 2.0**: See [`LICENSE`](LICENSE) or http://www.apache.org/licenses/LICENSE-2.0
- **GPL-2+**: https://www.gnu.org/licenses/old-licenses/gpl-2.0.html
- **GPL-3+**: https://www.gnu.org/licenses/gpl-3.0.html

**⚠️ Important:** Always check your build configuration to understand which license applies.

---

## Version History

- **14.0.0** - Initial Raspberry Pi release based on Android 14
  - IDP provisioning workflow
  - LUKS2 and dm-verity support
  - GPIO and file transfer commands
  - Comprehensive test suite (Catch2 v3.11.0)
  - Native library integration (libcryptsetup, libgpiod)

---

## Links

- **Upstream AOSP:** https://android.googlesource.com/platform/system/core/+/refs/heads/android14-release/fastboot/
- **Raspberry Pi Documentation:** https://www.raspberrypi.com/documentation/
- **rpi-sb-provisioner:** (Internal tool - contact Raspberry Pi for access)

---

**Built with ❤️ by Raspberry Pi Ltd**

