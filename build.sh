#!/bin/sh
# Builds with CMake on Linux and macOS, then runs the tests.
#   ./build.sh               debug
#   ./build.sh --release     optimised
#   ./build.sh --headless    only the ECS, the simulation and the tests: no window or GPU libraries
#   ./build.sh --no-tests    skip running the tests
set -eu
cd "$(dirname "$0")"

config=Debug
client=ON
tests=1
for arg in "$@"; do
    case "$arg" in
        --release) config=Release ;;
        --headless) client=OFF ;;
        --no-tests) tests=0 ;;
        *) sed -n '2,6p' "$0" >&2; exit 2 ;;
    esac
done

dir="build/$(echo "$config" | tr '[:upper:]' '[:lower:]')"
cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE="$config" -DTP_BUILD_CLIENT="$client"
cmake --build "$dir" --parallel
if [ "$tests" = 1 ]; then
    ctest --test-dir "$dir" --output-on-failure
fi
