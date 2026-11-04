# Commits the SPIR-V the Vulkan backend embeds. The build makes the table itself
# when it finds glslc (the Vulkan SDK): this configures and builds it, then copies
# it to src/render/shaders/spirv/table.inl, with the hash of the shader sources it
# came from, which the backend checks at startup.
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..'
$dir = Join-Path $root 'build\shaders'
& cmake -S $root -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release -DTP_BUILD_TESTS=OFF
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& cmake --build $dir --target treasure_planet
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Copy-Item (Join-Path $dir 'generated\spirv\table.inl') (Join-Path $root 'src\render\shaders\spirv\table.inl')
Write-Host "committed the SPIR-V table from $dir"
