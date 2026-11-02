# Regenerates the SPIR-V the Vulkan backend embeds, after editing mesh.glsl.
# Needs glslc from the Vulkan SDK. OpenGL compiles mesh.glsl itself at runtime, so a
# stale .inl shows up as the two backends disagreeing: tools/compare_captures.py.
$ErrorActionPreference = 'Stop'
$glslc = if ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK 'Bin\glslc.exe' } else { 'glslc' }
$dir = Join-Path $PSScriptRoot '..\src\render\shaders'
foreach ($stage in @(@('vert', 'VERTEX'), @('frag', 'FRAGMENT'))) {
    $out = Join-Path $dir "mesh.$($stage[0]).spv.inl"
    & $glslc "-fshader-stage=$($stage[0])" -std=450core --target-env=vulkan1.1 "-D$($stage[1])" -mfmt=c -o $out (Join-Path $dir 'mesh.glsl')
    if ($LASTEXITCODE -ne 0) { throw "glslc failed on the $($stage[0]) stage." }
}
