# Treasure Planet

A first-person multiplayer MMORPG in C-style C++. Players gather in a hub world and physically board ships: each planet has its own ship, and you get there by walking onto its deck and riding it through space. Eventually players build their own ships, fly them anywhere, carry each other and run whole economies. [ROADMAP.md](ROADMAP.md) has what is planned.

**This is the foundation, and it is single-player today.** You can walk around the hub, board any of the three ships, ride to its planet, walk around the landing pad and ride back. There is no networking yet, but the simulation is already written the way a server needs it: headless, with any number of players.

## Build and run

Needs Visual Studio with the C++ tools and its CMake component. Everything else is in the repository, and the Vulkan SDK is optional.

```powershell
.\build.ps1            # debug build, then the tests
.\build.ps1 -Release
build\debug\treasure_planet.exe                 # Vulkan, falling back to OpenGL
build\debug\treasure_planet.exe --renderer gl
```

| Input | Action |
| --- | --- |
| Mouse | Look |
| W A S D | Walk |
| Space | Jump |
| Esc / click | Release / recapture the mouse |

Ships keep a timetable, so be aboard before they leave. The window title shows the nearest ship and its countdown. To board, walk down a pier from the north edge of the hub (straight ahead from where you start) and through the gap in the ship's stern.

## Layout

```
src/ecs/        the ECS, ported from clockwork-c
src/game/       the simulation: components, systems and the universe's content. No window, no renderer
src/render/     the renderer API and its two backends
src/platform/   window, input and GL context (Windows only for now)
src/client/     the game loop: input in, ticks, draws out
tests/          headless tests of the ECS and of the game itself
tools/          shader compilation, capture comparison
third_party/    GL and Vulkan headers, volk
```

The dependencies point one way: `client -> game -> ecs`, and `client -> render -> platform`. The game cannot see the renderer, so a server can link `tp_game` alone.

## Style

C-style C++20: plain structs and free functions. No classes with behaviour, templates, exceptions, RTTI or standard library containers, and the build enforces the exceptions and RTTI parts (`/GR- /EHs-c-`, plus `/W4 /WX`). C++ is used for the small conveniences it offers over C, like struct names without `typedef`.

## The ECS

`src/ecs` is the core of [clockwork-c](https://github.com/Moises-Pirela/clockwork-c) (commit `4602476`) ported to C++, keeping its `cw_` API and behaviour: sparse component pools with stable addresses, cached queries, change observers, command buffers, snapshots, ordered systems with host/client authority, and a fixed-step clock. Its header documents the API and guarantees in detail. Reflection, scenes, assets and game modules were left behind, because they exist to serve clockwork's editor.

## The simulation

All game state lives in one `cw_world`, and everything shares one coordinate space. **Nothing teleports and no level is loaded:** a planet is a place, and a ship is a thing that really flies there.

A ship is a timetable plus a volume. It waits at a dock, then flies a rise, cruise and descend path to the other end of its route. Whatever is inside its volume when it moves gets moved with it. That one rule is the whole of boarding: walk onto the deck and you travel, with no prompt or state change, and any number of players can ride together. Decks are flush with piers and pads, so getting on and off is plain walking.

Adding a planet with its ship is one row in `PLANETS` in [universe.cpp](src/game/universe.cpp).

## The renderer

Callers create meshes and submit a frame, which is a camera, a sun and a list of draws. They never see a graphics API. Each backend implements the small table in [backend.h](src/render/backend.h):

- **Vulkan 1.1**, loaded at runtime through volk, so machines without a driver still start.
- **OpenGL 4.5** core with direct state access.

Both use the same shader source ([mesh.glsl](src/render/shaders/mesh.glsl)) and the same conventions: y-up clip space, and reversed depth with an infinite far plane on a 32-bit float buffer, so a deck rail and a distant planet share a depth buffer without fighting.

OpenGL compiles the shader at runtime. Vulkan embeds SPIR-V, which is committed so that building doesn't need the SDK. After editing the shader, run `tools\compile_shaders.ps1`, which does need `glslc` from the SDK.

To check that the backends agree, capture the same scripted scene with each and compare:

```powershell
build\debug\treasure_planet.exe --renderer vulkan --capture vk.bmp --ticks 60
build\debug\treasure_planet.exe --renderer gl --capture gl.bmp --ticks 60
python tools\compare_captures.py vk.bmp gl.bmp
```

`--at X Y Z` and `--look YAW PITCH` place the camera. Expect a handful of differing pixels out of 921,600, along triangle edges: the two APIs break rasterisation ties differently.

## Licences

The ported ECS keeps clockwork-c's MIT licence ([src/ecs/LICENSE](src/ecs/LICENSE)), and [third_party/README.md](third_party/README.md) lists the vendored code. The game itself has no licence chosen yet.
