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

This fastbootd implementation is designed to work with the **rpi-sb-provisioner** host tool, which provides:

- High-level provisioning workflows
- JSON-based device configuration
- Automated partition layout and flashing
- Encryption and verification setup
- A/B system support for OTA updates

### Typical Workflow

**Prerequisites:**
- `fastbootd` running on the Raspberry Pi (built from this repo)
- `fastboot` client installed on your host computer (from OS package)
- USB or network connection between host and device

**Example provisioning session:**

```bash
# On host computer (Linux/Mac/Windows):
# rpi-sb-provisioner wraps the fastboot client

rpi-sb-provisioner --config rpi5-secure.json --device /dev/ttyUSB0

# Behind the scenes, rpi-sb-provisioner uses the fastboot client:
fastboot stage device_config.json       # Upload JSON
fastboot oem idpinit                     # Initialize IDP
fastboot oem idpwrite                    # Create partitions
fastboot flash system system.img         # Flash images
fastboot oem verityappend system ...     # Setup dm-verity
fastboot oem cryptinit userdata luks2    # Setup encryption
fastboot oem idpdone                     # Finalize
```

**Where each component runs:**
- `rpi-sb-provisioner` → Host computer
- `fastboot` client → Host computer (communicates over USB/network)
- `fastbootd` daemon → Raspberry Pi (receives and executes commands)

**See:** [IDP_VERITY_INTEGRATION.md](IDP_VERITY_INTEGRATION.md) for full workflow details

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

**See:** Documentation in this directory for JSON schema details

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

**See:** [DM_VERITY_SETUP.md](DM_VERITY_SETUP.md) and [DM_VERITY_APPENDED_MODE.md](DM_VERITY_APPENDED_MODE.md)

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

> **Raspberry Pi OS only.** This project depends on `librpifwcrypto`, a library that interfaces with Raspberry Pi firmware cryptography services. That library is only available in the Raspberry Pi OS apt repository and is not packaged for generic Debian/Ubuntu. The build must run on a Raspberry Pi running Raspberry Pi OS (Trixie or later, arm64).

### Prerequisites

**On the Raspberry Pi:**

```bash
# Raspberry Pi OS (arm64)
sudo apt-get install \
    build-essential cmake git \
    android-liblog-dev android-libbase-dev android-libcutils-dev \
    android-libsparse-dev libfdisk-dev liburing-dev libsystemd-dev \
    zlib1g-dev libgpiod-dev libcryptsetup-dev libssl-dev \
    librpifwcrypto-dev
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

Run on the Raspberry Pi:

```bash
git clone https://github.com/raspberrypi/rpi-fastbootd
cd rpi-fastbootd
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

## CPack / Packaging

This project uses CPack to generate `.deb` packages for easy deployment.

### Creating Packages

```bash
# Build and package
cmake -B build -S .
cmake --build build
cd build
cpack
```

**Output:** `dist/rpi-fastbootd_14.0.0-<revision>_arm64.deb`

### Package Naming Convention

```
rpi-fastbootd_<VERSION>-<REVISION>_<ARCH>.deb
```

Where:
- **rpi-fastbootd** - Package name prefix (from `CPACK_PACKAGE_NAME`)
- **VERSION** - `14.0.0` (from `PROJECT_VERSION` in CMakeLists.txt)
- **REVISION** - Git commit count since Android 14 baseline (auto-calculated)
- **ARCH** - `arm64` (from `CPACK_DEBIAN_PACKAGE_ARCHITECTURE`)

**Example:** `rpi-fastbootd_14.0.0-247_arm64.deb`

### Customizing Package Metadata

Edit `cmake/Packaging.cmake`:

```cmake
set(CPACK_PACKAGE_NAME "rpi-${PROJECT_NAME}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Raspberry Pi Fastbootd")
set(CPACK_PACKAGE_VENDOR "Raspberry Pi Ltd")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Raspberry Pi Signed Boot Team")
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

**See:** [device/test/README.md](device/test/README.md) for full testing documentation

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

- [VERITY_MODES_COMPARISON.md](VERITY_MODES_COMPARISON.md) - dm-verity modes comparison
- [PROTOCOL.md](PROTOCOL.md) - Fastboot protocol specification
- [device/test/CATCH2_UPDATE.md](device/test/CATCH2_UPDATE.md) - Catch2 v3.11.0 upgrade notes

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

### Default License (Without libcryptsetup)

```
Apache License 2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

**Full license:** http://www.apache.org/licenses/LICENSE-2.0

### With libcryptsetup

When built with libcryptsetup, this binary must be licensed under **GPLv3**:

```
This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
```

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

