#!/bin/bash
# ae-smart-shunt/scripts/build_release.sh
# Automated firmware build script with hardware version support

set -e  # Exit on error

VERSION=$1
HW_VERSION=${2:-1}
ENV=${3:-ae-52mm-gauge}

if [ -z "$VERSION" ]; then
    echo "Usage: ./build_release.sh <version> [hw_version] [environment]"
    echo "Example: ./build_release.sh 1.2.3 2 ae-52mm-gauge"
    echo ""
    echo "Arguments:"
    echo "  version     - Firmware version (e.g., 1.2.3)"
    echo "  hw_version  - Hardware version (default: 1)"
    echo "  environment - PlatformIO environment (default: ae-52mm-gauge)"
    exit 1
fi

# Navigate to project root
PROJECT_ROOT=$(dirname "$(dirname "$(readlink -f "$0")")")
cd "$PROJECT_ROOT" || exit 1

# Check for pio
if ! command -v pio &> /dev/null; then
    if [ -f "$HOME/.platformio/penv/bin/pio" ]; then
        export PATH="$PATH:$HOME/.platformio/penv/bin"
    else
        echo "Error: 'pio' command not found. Please install PlatformIO."
        exit 1
    fi
fi

echo "========================================="
echo "Building Firmware Release"
echo "========================================="
echo "Version:     $VERSION"
echo "HW Version:  $HW_VERSION"
echo "Environment: $ENV"
echo "========================================="

# Set environment variables for version.py
export OTA_VERSION="$VERSION"
export HW_VERSION="$HW_VERSION"

# Run PlatformIO Build
echo "Running build..."
pio run -e "$ENV"

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Build Success!"
    
    # Prepare Release Directory
    RELEASE_DIR="./releases/v${VERSION}"
    mkdir -p "$RELEASE_DIR"
    
    # Generate descriptive filename
    DEVICE_TYPE="gauge"  # Can be parameterized if needed
    OUTPUT_FILENAME="${DEVICE_TYPE}_v${VERSION}_hw${HW_VERSION}.bin"
    
    # Copy firmware with descriptive name
    SRC_BIN="./.pio/build/$ENV/firmware.bin"
    if [ -f "$SRC_BIN" ]; then
        cp "$SRC_BIN" "$RELEASE_DIR/$OUTPUT_FILENAME"
        echo "✓ Firmware copied to: $RELEASE_DIR/$OUTPUT_FILENAME"
        
        # Calculate file size and checksum
        FILE_SIZE=$(stat -f%z "$RELEASE_DIR/$OUTPUT_FILENAME" 2>/dev/null || stat -c%s "$RELEASE_DIR/$OUTPUT_FILENAME")
        FILE_SHA256=$(sha256sum "$RELEASE_DIR/$OUTPUT_FILENAME" | awk '{print $1}')
        
        # Create Metadata
        cat > "$RELEASE_DIR/${OUTPUT_FILENAME%.bin}.json" <<EOF
{
  "version": "$VERSION",
  "hw_version": $HW_VERSION,
  "device_type": "$DEVICE_TYPE",
  "board": "$ENV",
  "filename": "$OUTPUT_FILENAME",
  "size_bytes": $FILE_SIZE,
  "sha256": "$FILE_SHA256",
  "built_at": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "min_hw_version": $HW_VERSION,
  "max_hw_version": $HW_VERSION
}
EOF
        echo "✓ Metadata created: $RELEASE_DIR/${OUTPUT_FILENAME%.bin}.json"
        echo ""
        echo "========================================="
        echo "Release build complete!"
        echo "Output: $RELEASE_DIR/$OUTPUT_FILENAME"
        echo "========================================="
    else
        echo "✗ Error: Output binary not found at $SRC_BIN"
        exit 1
    fi
else
    echo "✗ Build Failed!"
    exit 1
fi
