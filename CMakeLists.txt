cmake_minimum_required(VERSION 3.20)
project(treasure_planet VERSION 0.1.0 LANGUAGES C CXX)

option(TP_BUILD_CLIENT "Build the windowed client; off needs no window system or GPU libraries" ON)
option(TP_BUILD_TESTS "Build the headless tests" ON)
option(TP_WAYLAND "Also support native Wayland on Linux (X11 always works, including under XWayland)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C-style C++: no exceptions, no RTTI, no standard library containers.
if(MSVC)
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
function(tp_strict target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /GR- /EHs-c- /utf-8)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS NOMINMAX)
    elseif(WIN32)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers
                               $<$<CXX_COMPILER_ID:GNU>:-Wno-stringop-truncation>
                               -fno-exceptions -fno-rtti)
    else()
        # Partial aggregate initialisation is the C idiom here: the rest is zero by definition.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers
                               $<$<CXX_COMPILER_ID:GNU>:-Wno-stringop-truncation>
                               -fno-exceptions -fno-rtti)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# The ECS and the simulation: portable and headless, what a server will link
# ---------------------------------------------------------------------------
add_library(tp_ecs STATIC
    src/ecs/clockwork.cpp src/ecs/cw_commands.cpp src/ecs/cw_bytes.cpp
    src/ecs/cw_snapshot.cpp src/ecs/cw_file.cpp src/ecs/cw_sim.cpp)
tp_strict(tp_ecs)

add_library(tp_game STATIC src/game/game.cpp src/game/universe.cpp src/game/planet.cpp src/game/dungeon.cpp src/game/items.cpp)
target_link_libraries(tp_game PUBLIC tp_ecs)
tp_strict(tp_game)
if(NOT WIN32)
    target_link_libraries(tp_ecs PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# Networking: transport, protocol, the zone server and the replica client. All
# headless, so the server runs on Linux and the tests run the whole stack in-process.
# ---------------------------------------------------------------------------
add_library(tp_net STATIC
    src/net/net_socket.cpp src/net/net_connection.cpp src/net/crypto.cpp src/net/accounts.cpp src/net/snapshot.cpp
    src/net/zones.cpp src/net/social.cpp src/net/directory.cpp src/net/protocol.cpp src/net/server.cpp src/net/client.cpp
    src/net/process.cpp src/net/orchestrator.cpp)
target_link_libraries(tp_net PUBLIC tp_game)
if(WIN32)
    target_link_libraries(tp_net PUBLIC ws2_32 bcrypt winmm)
endif()
tp_strict(tp_net)

add_executable(treasure_server src/server/main.cpp)
target_link_libraries(treasure_server PRIVATE tp_net)
tp_strict(treasure_server)

add_executable(treasure_directory src/directory/main.cpp)
target_link_libraries(treasure_directory PRIVATE tp_net)
tp_strict(treasure_directory)

add_executable(treasure_orchestrator src/orchestrator/main.cpp)
target_link_libraries(treasure_orchestrator PRIVATE tp_net)
tp_strict(treasure_orchestrator)

add_executable(treasure_bots src/tools/bots.cpp)
target_link_libraries(treasure_bots PRIVATE tp_net)
tp_strict(treasure_bots)

if(TP_BUILD_TESTS)
    enable_testing()
    foreach(name test_ecs test_game test_net test_hud test_render)
        add_executable(${name} tests/${name}.cpp)
        tp_strict(${name})
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
    target_link_libraries(test_ecs PRIVATE tp_game)
    target_link_libraries(test_game PRIVATE tp_game)
    target_link_libraries(test_net PRIVATE tp_net)
    target_sources(test_hud PRIVATE src/client/hud.cpp)
    target_link_libraries(test_hud PRIVATE tp_net)
    # Preparing a frame is pure arithmetic: the test links it without a backend.
    target_sources(test_render PRIVATE src/render/frame.cpp src/render/obj.cpp)
    target_link_libraries(test_render PRIVATE tp_game)
    # The orchestrator test starts real shard servers.
    target_compile_definitions(test_net PRIVATE TP_SERVER_PATH="$<TARGET_FILE:treasure_server>")
    add_dependencies(test_net treasure_server)
endif()

# ---------------------------------------------------------------------------
# The client: window, renderer (OpenGL 4.5 and Vulkan 1.1), game loop
# ---------------------------------------------------------------------------
if(TP_BUILD_CLIENT)
    # The shaders: every file in src/render/shaders (the shared pieces, the stage
    # programs, and a material shader per file in materials/). Each is embedded as
    # a byte array (a string literal would hit MSVC's 16 KB limit), 16 bytes a line,
    # in a table the renderer reads: OpenGL compiles from it at runtime, expanding
    # the #includes itself. Adding a file is adding a shader, and the build sees it.
    set(TP_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders)
    file(GLOB_RECURSE TP_SHADER_FILES CONFIGURE_DEPENDS
         ${TP_SHADER_DIR}/*.glsl ${TP_SHADER_DIR}/*.vert ${TP_SHADER_DIR}/*.frag)
    list(SORT TP_SHADER_FILES)
    # Editing one reconfigures too: the sources are read here, into the table.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TP_SHADER_FILES})
    set(TP_SHADER_SOURCES "")
    set(TP_SHADER_TABLE "static const r_shader_file SHADER_FILES[] = {\n")
    set(TP_SHADER_TEXT "")
    set(index 0)
    string(REPEAT "0x[0-9a-f][0-9a-f]," 16 TP_BYTE_LINE)
    foreach(file ${TP_SHADER_FILES})
        file(RELATIVE_PATH name ${TP_SHADER_DIR} ${file})
        file(READ ${file} text)
        string(REPLACE "\r" "" text "${text}")
        string(APPEND TP_SHADER_TEXT "${name}\n${text}\n")
        file(READ ${file} hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
        string(REGEX REPLACE "(${TP_BYTE_LINE})" "\\1\n" bytes "${bytes}")
        string(APPEND TP_SHADER_SOURCES "static const unsigned char SHADER_SRC_${index}[] = {\n${bytes}0x00};\n")
        string(APPEND TP_SHADER_TABLE "    {\"${name}\", SHADER_SRC_${index}, sizeof(SHADER_SRC_${index}) - 1},\n")
        math(EXPR index "${index} + 1")
    endforeach()
    string(APPEND TP_SHADER_TABLE "};\n")
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/shader_sources.inl
         CONTENT "${TP_SHADER_SOURCES}${TP_SHADER_TABLE}" @ONLY)
    string(SHA256 TP_SHADERS_SHA256 "${TP_SHADER_TEXT}")

    # The Vulkan backend embeds SPIR-V: one blob per program, which is every
    # .vert and .frag at the top of the directory, and two per material shader
    # (the lit pass and the shadow pass). With glslc at hand (the Vulkan SDK) the
    # build compiles them into a table in the build tree; without, the table
    # committed in src/render/shaders/spirv is used, and the backend checks at
    # startup that it was built from these very sources (by their hash, line
    # endings aside) rather than run stale shaders against the wrong layouts.
    find_program(TP_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    set(TP_SPIRV_SOURCES)
    if(TP_GLSLC)
        set(spirv_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/spirv)
        file(MAKE_DIRECTORY ${spirv_dir})
        set(spirv_names)
        set(spirv_files)
        foreach(file ${TP_SHADER_FILES})
            file(RELATIVE_PATH name ${TP_SHADER_DIR} ${file})
            get_filename_component(suffix ${file} LAST_EXT)
            string(SUBSTRING ${suffix} 1 -1 stage)
            if(NOT stage STREQUAL "vert" AND NOT stage STREQUAL "frag")
                continue()
            endif()
            set(variants "")
            if(name MATCHES "^materials/")
                set(variants "lit;shadow")
            else()
                set(variants "plain")
            endif()
            foreach(variant ${variants})
                set(blob ${name})
                set(define "")
                if(variant STREQUAL "lit")
                    set(blob "${name}:lit")
                    set(define -DLIT_PASS)
                elseif(variant STREQUAL "shadow")
                    set(blob "${name}:shadow")
                    set(define -DSHADOW_PASS)
                endif()
                string(MAKE_C_IDENTIFIER ${blob} safe)
                set(out ${spirv_dir}/${safe}.spv)
                add_custom_command(OUTPUT ${out}
                    COMMAND ${TP_GLSLC} -fshader-stage=${stage} -std=450core --target-env=vulkan1.1
                            -I ${TP_SHADER_DIR} ${define} -o ${out} ${file}
                    DEPENDS ${TP_SHADER_FILES} COMMENT "glslc ${blob}" VERBATIM)
                list(APPEND spirv_names ${blob})
                list(APPEND spirv_files ${out})
            endforeach()
        endforeach()
        add_custom_command(OUTPUT ${spirv_dir}/table.inl
            COMMAND ${CMAKE_COMMAND} -DOUT=${spirv_dir}/table.inl -DHASH=${TP_SHADERS_SHA256}
                    "-DNAMES=${spirv_names}" "-DFILES=${spirv_files}"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv_table.cmake
            DEPENDS ${spirv_files} ${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv_table.cmake
            COMMENT "the SPIR-V table" VERBATIM)
        set(TP_SPIRV_SOURCES ${spirv_dir}/table.inl)
        set(TP_SPIRV_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    else()
        set(TP_SPIRV_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders)
        file(READ src/render/shaders/spirv/table.inl committed)
        if(NOT committed MATCHES "SPIRV_SOURCE_SHA256\\[\\] = \"${TP_SHADERS_SHA256}\"")
            message(WARNING "The committed SPIR-V in src/render/shaders/spirv was not built from the current "
                            "shaders, so the Vulkan backend will decline to start and the client will fall "
                            "back to OpenGL. Install the Vulkan SDK (glslc) and reconfigure, or run tools/compile_shaders.")
        endif()
    endif()

    add_library(tp_volk STATIC third_party/volk/volk.c)
    target_include_directories(tp_volk PUBLIC third_party/volk third_party/vulkan)
    target_compile_definitions(tp_volk PUBLIC VK_NO_PROTOTYPES)
    target_link_libraries(tp_volk PUBLIC ${CMAKE_DL_LIBS})

    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ${TP_WAYLAND} CACHE BOOL "" FORCE)
    add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)

    add_executable(treasure_planet
        src/client/main.cpp
        src/client/hud.cpp
        src/platform/platform_glfw.cpp
        src/render/renderer.cpp
        src/render/frame.cpp
        src/render/obj.cpp
        src/render/shaders.cpp
        src/render/backend_gl.cpp
        src/render/backend_vk.cpp
        ${TP_SPIRV_SOURCES})
    target_include_directories(treasure_planet SYSTEM PRIVATE third_party/gl)
    target_include_directories(treasure_planet PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${TP_SPIRV_INCLUDE})
    target_compile_definitions(treasure_planet PRIVATE TP_SHADERS_SHA256="${TP_SHADERS_SHA256}")
    target_link_libraries(treasure_planet PRIVATE tp_net tp_volk glfw)
    tp_strict(treasure_planet)
endif()
