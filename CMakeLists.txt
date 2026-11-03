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

add_library(tp_game STATIC src/game/game.cpp src/game/universe.cpp src/game/dungeon.cpp src/game/items.cpp)
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
    foreach(name test_ecs test_game test_net test_hud)
        add_executable(${name} tests/${name}.cpp)
        tp_strict(${name})
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
    target_link_libraries(test_ecs PRIVATE tp_game)
    target_link_libraries(test_game PRIVATE tp_game)
    target_link_libraries(test_net PRIVATE tp_net)
    target_sources(test_hud PRIVATE src/client/hud.cpp)
    target_link_libraries(test_hud PRIVATE tp_net)
    # The orchestrator test starts real shard servers.
    target_compile_definitions(test_net PRIVATE TP_SERVER_PATH="$<TARGET_FILE:treasure_server>")
    add_dependencies(test_net treasure_server)
endif()

# ---------------------------------------------------------------------------
# The client: window, renderer (OpenGL 4.5 and Vulkan 1.1), game loop
# ---------------------------------------------------------------------------
if(TP_BUILD_CLIENT)
    # The GL backend compiles mesh.glsl at runtime; wrap the file in a raw string.
    set(MESH_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/mesh.glsl)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MESH_GLSL})
    file(READ ${MESH_GLSL} MESH_GLSL_TEXT)
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/mesh.glsl.inl
         CONTENT "R\"GLSL(${MESH_GLSL_TEXT})GLSL\"\n" @ONLY)

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
        src/render/backend_gl.cpp
        src/render/backend_vk.cpp)
    target_include_directories(treasure_planet SYSTEM PRIVATE third_party/gl)
    target_include_directories(treasure_planet PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(treasure_planet PRIVATE tp_net tp_volk glfw)
    tp_strict(treasure_planet)
endif()
