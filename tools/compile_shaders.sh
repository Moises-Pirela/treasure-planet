#!/bin/sh
# Commits the SPIR-V the Vulkan backend embeds. The build makes the table itself
# when it finds glslc (the Vulkan SDK, or `brew install shaderc`): this configures
# and builds it, then copies it to src/render/shaders/spirv/table.inl, with the
# hash of the shader sources it came from, which the backend checks at startup.
set -eu
cd "$(dirname "$0")/.."
dir=build/shaders
cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release -DTP_BUILD_TESTS=OFF
cmake --build "$dir" --target treasure_planet --parallel
cp "$dir/generated/spirv/table.inl" src/render/shaders/spirv/table.inl
echo "committed the SPIR-V table from $dir"
