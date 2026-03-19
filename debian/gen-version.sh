#!/bin/bash
# Generate debian/changelog with version calculated from git
# Creates detailed changelog entries from git commits

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEMPLATE="${SCRIPT_DIR}/changelog.template"
OUTPUT="${SCRIPT_DIR}/changelog"

# Android 14 baseline commit
BASELINE_COMMIT="45cfa6160"

# Package metadata
PACKAGE_NAME="rpi-fastbootd"
DISTRIBUTION="unstable"
MAINTAINER="Raspberry Pi Signed Boot Team <applications@raspberrypi.com>"

# Get total commit count for progress reporting
GIT_COMMITS=$(git rev-list --count ${BASELINE_COMMIT}^..HEAD 2>/dev/null || echo "0")

# Version from HEAD: 14.0.0~git<YYYYMMDD>.<7-char hash>
HEAD_DATE=$(git show -s --format=%cd --date=format:%Y%m%d HEAD)
HEAD_HASH=$(git show -s --format=%h --abbrev=7 HEAD)
VERSION="14.0.0~git${HEAD_DATE}.${HEAD_HASH}"

echo "Generating debian/changelog with version ${VERSION}..."

# Start fresh changelog
> "${OUTPUT}"

# Get all commits after baseline (newest first for Debian changelog format)
mapfile -t COMMITS < <(git rev-list ${BASELINE_COMMIT}^..HEAD 2>/dev/null || true)

# Generate entry for each commit (newest first)
for commit in "${COMMITS[@]}"; do
    COMMIT_DATE_SHORT=$(git show -s --format=%cd --date=format:%Y%m%d "${commit}")
    COMMIT_HASH=$(git show -s --format=%h --abbrev=7 "${commit}")
    COMMIT_VERSION="14.0.0~git${COMMIT_DATE_SHORT}.${COMMIT_HASH}"
    COMMIT_DATE=$(git show -s --format=%aD "${commit}")
    COMMIT_SUBJECT=$(git show -s --format=%s "${commit}")
    COMMIT_BODY=$(git show -s --format=%b "${commit}" | sed 's/^/  /')
    
    # Write changelog entry
    cat >> "${OUTPUT}" << EOF
${PACKAGE_NAME} (${COMMIT_VERSION}) ${DISTRIBUTION}; urgency=medium

  * ${COMMIT_SUBJECT}
EOF
    
    # Add commit body if present (indented and bullet-pointed)
    if [ -n "${COMMIT_BODY}" ]; then
        echo "${COMMIT_BODY}" | while IFS= read -r line; do
            # Skip lines that are only whitespace
            if [ -n "$(echo "$line" | tr -d '[:space:]')" ]; then
                echo "  ${line}" >> "${OUTPUT}"
            fi
        done
    fi
    
    echo "" >> "${OUTPUT}"
    echo " -- ${MAINTAINER}  ${COMMIT_DATE}" >> "${OUTPUT}"
    echo "" >> "${OUTPUT}"
done

# Append template (baseline entry) if it exists
if [ -f "${TEMPLATE}" ]; then
    cat "${TEMPLATE}" >> "${OUTPUT}"
fi

echo "Generated debian/changelog with ${GIT_COMMITS} entries (version ${VERSION})"


