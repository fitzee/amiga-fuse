#!/bin/bash

# Simple build script wrapper for amiga-fuse
# Uses CMake for cross-platform compatibility

set -e

BUILD_DIR="build"
BUILD_TYPE="Release"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --clean)
            rm -rf "$BUILD_DIR"
            echo "Cleaned build directory"
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [--debug] [--clean] [--help]"
            echo "  --debug  Build in debug mode"
            echo "  --clean  Remove build directory"
            echo "  --help   Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure and build
echo "Configuring build with CMake..."
cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" ..

echo "Building amiga-fuse..."
cmake --build . --config "$BUILD_TYPE"

# Copy binary to parent directory for convenience
if [ -f "amiga-fuse" ]; then
    cp amiga-fuse ../
    echo "Build successful! Binary size: $(ls -lh ../amiga-fuse | awk '{print $5}')"
    echo "Usage: ./amiga-fuse <adf_file> <mount_point>"
else
    echo "Build failed - binary not found"
    exit 1
fi
