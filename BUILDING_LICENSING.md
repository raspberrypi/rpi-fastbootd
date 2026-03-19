# Building and Licensing Guide

## License Summary

The **rpi-fastbootd** package has conditional licensing based on build-time library linkage:

| Build Configuration | Binary License | Library Dependencies |
|---------------------|----------------|----------------------|
| **Default** (recommended) | **GPL-2+ / GPL-3+** | libcryptsetup12, libgpiod |
| **Apache-only** | **Apache 2.0** | Command-line fallbacks only |

### Source Code License

All source code in this repository is licensed under **Apache License 2.0**. The binary license change occurs only due to dynamic linking with GPL-licensed libraries at build time.

## Understanding the License

### Why GPL for Default Build?

The default build dynamically links against:
- **libcryptsetup12**: GPL-2+ licensed library for LUKS encryption
- **libgpiod**: GPL-2+ licensed library for GPIO control

Per GPL terms, dynamically linking GPL libraries causes the entire binary to be GPL-licensed, even though the source code remains Apache 2.0.

### When to Use Each Build

#### Use Default (GPL) Build When:
- ✅ You need native, high-performance LUKS operations
- ✅ You need native GPIO control without shelling out
- ✅ GPL-2+/GPL-3+ licensing is acceptable for your use case
- ✅ You're distributing as part of a GPL-compatible system

#### Use Apache-Only Build When:
- ✅ You require Apache 2.0 licensing for compliance
- ✅ You're okay with command-line fallbacks (slower but functional)
- ✅ You don't need LUKS or GPIO features
- ✅ You're distributing in Apache-2.0-only environments

## Building

### Default Build (GPL-2+/GPL-3+)

```bash
cd /path/to/rpi-fastbootd
./build-package.sh
```

This produces a binary that:
- Links `libcryptsetup12` and `libgpiod`
- Uses native C APIs for crypto and GPIO
- Is licensed as **GPL-2+ / GPL-3+**

### Apache-Only Build (Apache 2.0)

```bash
cd /path/to/rpi-fastbootd
DEB_BUILD_OPTIONS="nocryptsetup" ./build-package.sh
```

This produces a binary that:
- Does NOT link `libcryptsetup12`
- Uses `cryptsetup` command-line tool as fallback
- Uses `gpioset` command-line tool as fallback
- Is licensed as **Apache 2.0**

### CMake Direct Build

If not using Debian packaging:

```bash
# GPL build (default)
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make

# Apache-only build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DSKIP_CRYPTSETUP=ON ..
make
```

## Verifying Your Build

### Check Linked Libraries

```bash
ldd /usr/bin/fastbootd | grep -E "(cryptsetup|gpiod)"
```

**GPL build output:**
```
libcryptsetup.so.12 => /lib/aarch64-linux-gnu/libcryptsetup.so.12
libgpiod.so.3 => /lib/aarch64-linux-gnu/libgpiod.so.3
```

**Apache build output:**
```
(no output - libraries not linked)
```

### Check License File

After CMake configuration:
```bash
cat build/LICENSE_INFO
```

- **GPL-2+** = Default build with libcryptsetup
- **Apache-2.0** = Apache-only build without libcryptsetup

### Check Package Dependencies

```bash
dpkg-deb -I rpi-fastbootd_*.deb | grep "Depends:"
```

**GPL build**: Lists `libcryptsetup12`  
**Apache build**: Does NOT list `libcryptsetup12`

## Distribution Considerations

### For GPL Build

- ✅ Can be distributed under GPL-2+ or GPL-3+
- ✅ Can be combined with other GPL software
- ✅ Source code must be made available per GPL terms
- ❌ Cannot be combined with Apache-2.0-only components

### For Apache Build

- ✅ Can be distributed under Apache 2.0
- ✅ Can be combined with Apache-2.0 software
- ✅ More permissive redistribution terms
- ⚠️ Slower cryptsetup operations (command-line fallback)
- ⚠️ Requires `cryptsetup-bin` package at runtime

## Runtime Dependencies

### GPL Build
```
Depends: libcryptsetup12, libgpiod2, cryptsetup-bin (fallback)
```

### Apache Build
```
Depends: cryptsetup-bin (required)
Recommends: gpiod (for gpioset command)
```

## FAQ

### Q: Can I use the GPL build in a commercial product?
**A:** Yes, but you must comply with GPL terms (provide source, allow redistribution, etc.).

### Q: Does the Apache build lose functionality?
**A:** No. It uses command-line tools instead of native libraries. Performance is slightly lower, but all features work.

### Q: Can I change the license later?
**A:** You can rebuild with different options, but once distributed, that binary's license applies to that distribution.

### Q: What about libgpiod?
**A:** Currently, libgpiod is always linked if available (GPL-2+ licensed). A future enhancement could make it optional similar to libcryptsetup.

### Q: Is the test suite affected?
**A:** Tests work with both builds. Native library tests are skipped in Apache-only builds.

## Contact

For licensing questions, contact: Raspberry Pi Signed Boot Team <applications@raspberrypi.com>

