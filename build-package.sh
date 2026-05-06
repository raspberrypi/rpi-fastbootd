#!/bin/bash
# Build rpi-fastbootd Debian package using devscripts

set -e

echo "========================================"
echo "Raspberry Pi Fastbootd Package Builder"
echo "========================================"
echo

# Check if we're in the right directory
if [ ! -f "debian/control" ]; then
    echo "Error: Must run from repository root (where debian/ directory is located)"
    exit 1
fi

# Ensure git submodules (e.g. third_party/rpi-verity-verifier) are populated.
# Skipped when not in a git checkout — sbuild unpacks a self-contained source
# tarball produced by `gbp buildpackage` (gbp.conf has submodules=True), so
# the submodule content is already on disk in that case.
if [ -d .git ] || git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Updating git submodules..."
    git submodule update --init --recursive
fi

# Parse options
BUILD_TYPE="default"
UNSIGNED="-uc -us"

while [[ $# -gt 0 ]]; do
    case $1 in
        --no-cryptsetup)
            BUILD_TYPE="nocryptsetup"
            echo "Building WITHOUT libcryptsetup (Apache 2.0 license)"
            export DEB_BUILD_OPTIONS="nocryptsetup"
            ;;
        --signed)
            UNSIGNED=""
            echo "Building signed package"
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo
            echo "Options:"
            echo "  --no-cryptsetup   Build without libcryptsetup (Apache 2.0 license)"
            echo "  --signed          Sign the package with GPG"
            echo "  --help, -h        Show this help message"
            echo
            echo "Default: Build with libcryptsetup (GPLv3 license)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
    shift
done

# Check for required tools
DS_PKG="devscripts"
echo "Checking $DS_PKG is installed..."
if ! dpkg-query -W "$DS_PKG" | grep -qE "^$DS_PKG	.+"; then
    echo "Error: $DS_PKG is required but not installed"
    echo "Install with: sudo apt-get install $DS_PKG"
    exit 1
fi

echo "Generating and installing build dependencies..."
mk-build-deps \
  --install \
  --root-cmd "sudo --non-interactive" \
  --tool "apt-get --no-install-recommends --assume-yes --quiet"

# Generate version and changelog
echo "Generating version information..."
./debian/gen-version.sh

# Get the generated version for display
VERSION=$(head -n1 debian/changelog | sed 's/.*(\(.*\)).*/\1/')
echo "Package version: ${VERSION}"
echo

# Clean previous builds
echo "Cleaning previous build artifacts..."
if [ -d "debian/tmp" ]; then
    rm -rf debian/tmp
fi
if [ -d "debian/.debhelper" ]; then
    rm -rf debian/.debhelper
fi
if [ -d "debian/rpi-fastbootd" ]; then
    rm -rf debian/rpi-fastbootd
fi

# Build the package
echo
echo "========================================"
echo "Building package..."
echo "========================================"
echo "Build is hermetic — jsoncpp from libjsoncpp-dev (apt), other"
echo "third-party deps as git submodules under third_party/"
echo "(rpi-verity-verifier, Catch2). No network at configure/compile."
echo
dpkg-buildpackage -b ${UNSIGNED} -j$(nproc)

# Remove build dependency metapackage installed by mk-build-deps
echo "Removing build dependency metapackage..."
sudo --non-interactive dpkg --purge rpi-fastbootd-build-deps 2>/dev/null || true

# Show results
echo
echo "========================================"
echo "Build complete!"
echo "========================================"
echo
BUILT_DEB="../rpi-fastbootd_${VERSION}_arm64.deb"
echo "Package location: ${BUILT_DEB}"
ls -lh "${BUILT_DEB}" 2>/dev/null || echo "Warning: Package file not found"
echo
echo "Install with:"
echo "  sudo dpkg -i ${BUILT_DEB}"
echo
if [ -f "obj-aarch64-linux-gnu/LICENSE_INFO" ]; then
    LICENSE_TYPE=$(cat obj-aarch64-linux-gnu/LICENSE_INFO)
    case "$LICENSE_TYPE" in
        "Apache-2.0")
            LICENSE_INFO="Apache 2.0 (without libcryptsetup)"
            ;;
        "GPL-2+")
            LICENSE_INFO="GPL-2+/GPL-3+ (includes libcryptsetup)"
            ;;
        *)
            LICENSE_INFO="$LICENSE_TYPE"
            ;;
    esac
else
    if [ "$BUILD_TYPE" = "nocryptsetup" ]; then
        LICENSE_INFO="Apache 2.0 (without libcryptsetup)"
    else
        LICENSE_INFO="GPL-2+/GPL-3+ (includes libcryptsetup)"
    fi
fi

echo "License: ${LICENSE_INFO}"
echo "${LICENSE_INFO}" > LICENSE_BUILD_INFO

