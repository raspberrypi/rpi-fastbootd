#!/bin/bash
# Generate debian/changelog with version calculated from git

set -e

# Calculate version from git commits since Android 14 baseline
BASELINE_COMMIT="2bdf18c51"
GIT_COMMITS=$(git rev-list --count ${BASELINE_COMMIT}^..HEAD 2>/dev/null || echo "0")
VERSION="14.0.0-${GIT_COMMITS}"
TIMESTAMP=$(date -R)

# Generate changelog
cat > debian/changelog << EOF
rpi-fastbootd (${VERSION}) bookworm; urgency=medium

  * Automated build from git revision ${GIT_COMMITS}
  * Based on Android 14 AOSP fastboot
  * See git log for detailed changes

 -- Raspberry Pi Signed Boot Team <applications@raspberrypi.com>  ${TIMESTAMP}
EOF

echo "Generated debian/changelog with version ${VERSION}"


