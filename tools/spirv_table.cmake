# Turns compiled SPIR-V files into one C table the Vulkan backend embeds:
#   cmake -DOUT=<table.inl> -DHASH=<sha256> -DNAMES=<a;b;..> -DFILES=<x.spv;y.spv;..> -P spirv_table.cmake
# Each blob is named after its program ("mesh.vert", "materials/planet.frag:lit").
# The build runs this when it has glslc; tools/compile_shaders commits the result.
set(content "// Generated: the SPIR-V of every program in src/render/shaders, and the hash of\n// the sources it came from. Do not edit.\n")
set(table "static const r_spirv_blob SPIRV_BLOBS[] = {\n")
list(LENGTH NAMES count)
math(EXPR last "${count} - 1")
foreach(i RANGE ${last})
    list(GET NAMES ${i} name)
    list(GET FILES ${i} file)
    file(READ ${file} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])" "0x\\4\\3\\2\\1," words "${hex}")
    string(REPEAT "0x[0-9a-f]+," 8 line)
    string(REGEX REPLACE "(${line})" "\\1\n" words "${words}")
    string(APPEND content "static const uint32_t SPV_${i}[] = {\n${words}};\n")
    string(APPEND table "    {\"${name}\", SPV_${i}, sizeof(SPV_${i})},\n")
endforeach()
string(APPEND content "${table}};\nstatic const char SPIRV_SOURCE_SHA256[] = \"${HASH}\";\n")
file(WRITE ${OUT} "${content}")
