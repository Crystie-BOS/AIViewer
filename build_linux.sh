#!/bin/bash
# AI Viewer — Linux build script
# Usage: ./build_linux.sh [configure|build|both]
set -e

ACTION="${1:-both}"

# Path to viewer-build-variables (adjust if your Linux build env differs)
: "${AUTOBUILD_VARIABLES_FILE:=/opt/ai-viewer-build/viewer-build-variables/variables}"

if [ ! -f "$AUTOBUILD_VARIABLES_FILE" ]; then
    echo "ERROR: AUTOBUILD_VARIABLES_FILE not found: $AUTOBUILD_VARIABLES_FILE"
    echo "Set the env var to point to your viewer-build-variables/variables file."
    exit 1
fi

export AUTOBUILD_VARIABLES_FILE

CMAKE_ARGS=(
    -DCMAKE_VERBOSE_MAKEFILE:BOOL=FALSE
    -DLL_TESTS:BOOL=OFF
    -DPACKAGE:BOOL=FALSE
    -DOPENAL:BOOL=TRUE
    -DFMODSTUDIO:BOOL=OFF
    -DUSE_KDU:BOOL=OFF
    -DHAVOK_TPV:BOOL=OFF
    -DHAVOK:BOOL=OFF
    -DRLV_ALWAYS_ON:BOOL=TRUE
)

if [[ "$ACTION" == "configure" || "$ACTION" == "both" ]]; then
    echo "=== Configuring AI Viewer (Linux 64-bit ReleaseOS) ==="
    autobuild configure -A 64 -c ReleaseOS -- "${CMAKE_ARGS[@]}"
fi

if [[ "$ACTION" == "build" || "$ACTION" == "both" ]]; then
    echo "=== Building AI Viewer ==="
    autobuild build -A 64 -c ReleaseOS --no-configure
    echo "=== Build complete ==="
    find . -name "ai-viewer" -type f 2>/dev/null | head -5
fi
