#!/bin/sh
# Builds on macOS: checks for what the build needs, then does what build.sh does.
#   ./build_mac.sh               debug, then the tests
#   ./build_mac.sh --release     optimised
#   ./build_mac.sh --headless    only the ECS, the simulation and the tests: MoltenVK not needed
#   ./build_mac.sh --no-tests    skip running the tests
#   ./build_mac.sh --deps        install whatever is missing with Homebrew first
set -eu
cd "$(dirname "$0")"

if [ "$(uname -s)" != Darwin ]; then
    echo "build_mac.sh is for macOS; use build.sh here" >&2
    exit 1
fi

dir=build/debug
client=1
install=0
build_args=
for arg in "$@"; do
    case "$arg" in
        --release) dir=build/release; build_args="$build_args $arg" ;;
        --headless) client=0; build_args="$build_args $arg" ;;
        --no-tests) build_args="$build_args $arg" ;;
        --deps) install=1 ;;
        *) sed -n '2,7p' "$0" >&2; exit 2 ;;
    esac
done

# A shell that has not sourced Homebrew's profile still finds what it installed.
# Apple Silicon's prefix goes last, so it comes first on the PATH.
for prefix in /usr/local /opt/homebrew; do
    if [ -x "$prefix/bin/brew" ]; then PATH="$prefix/bin:$PATH"; fi
done

if ! xcode-select -p >/dev/null 2>&1; then
    echo "the Xcode command line tools are missing: run 'xcode-select --install', then try again" >&2
    exit 1
fi

missing=
if ! command -v cmake >/dev/null 2>&1; then missing="$missing cmake"; fi
if [ "$client" = 1 ]; then
    # Where the client looks for Vulkan besides the default search path (platform_glfw.cpp).
    found=0
    for lib in /opt/homebrew/lib/libvulkan.1.dylib /opt/homebrew/lib/libMoltenVK.dylib \
               /usr/local/lib/libvulkan.1.dylib /usr/local/lib/libMoltenVK.dylib; do
        if [ -e "$lib" ]; then found=1; fi
    done
    if [ "$found" = 0 ]; then missing="$missing molten-vk"; fi
fi

if [ -n "$missing" ]; then
    if [ "$install" = 0 ]; then
        echo "missing:$missing. Run 'brew install$missing', or this script with --deps" >&2
        exit 1
    fi
    if ! command -v brew >/dev/null 2>&1; then
        echo "missing:$missing, and --deps needs Homebrew: see https://brew.sh" >&2
        exit 1
    fi
    brew install $missing
fi

sh ./build.sh $build_args

if [ "$client" = 1 ]; then
    echo "built: $dir/treasure_planet"
else
    echo "built: $dir/treasure_server"
fi
