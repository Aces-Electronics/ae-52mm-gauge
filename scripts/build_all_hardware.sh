#!/bin/bash
# ae-smart-shunt/scripts/build_all_hardware.sh
# Build firmware for all hardware versions

set -e

VERSION=$1

if [ -z "$VERSION" ]; then
    echo "Usage: ./build_all_hardware.sh <version>"
    echo "Example: ./build_all_hardware.sh 1.2.3"
    echo ""
    echo "This will build firmware for all defined hardware versions."
    exit 1
fi

# Navigate to project root
PROJECT_ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
cd "$PROJECT_ROOT" || exit 1

# Define hardware versions to build
# Modify this array as new hardware revisions are released
HW_VERSIONS=(1 2)

echo "========================================="
echo "Building Firmware for All Hardware Versions"
echo "Version: $VERSION"
echo "Hardware Versions: ${HW_VERSIONS[*]}"
echo "========================================="
echo ""

for hw_ver in "${HW_VERSIONS[@]}"; do
    echo "Building for HW v$hw_ver..."
    ./scripts/build_release.sh "$VERSION" "$hw_ver"
    echo ""
done

echo "========================================="
echo "All builds complete!"
echo "Releases available in: ./releases/v${VERSION}/"
echo "========================================="
ls -lh "./releases/v${VERSION}/"
