# Treasure Planet

A first-person multiplayer MMORPG in C-style C++. Players gather in a hub world and physically board ships: each planet has its own ship, and you get there by walking onto its deck and riding it through space. Eventually players build their own ships, fly them anywhere, carry each other and run whole economies. [ROADMAP.md](ROADMAP.md) has what is planned.

**This is the foundation.** You can walk around the hub, board any of the three ships, ride to its planet, walk around the landing pad and ride back, alone or on a server with other players, who appear as capsules. The server is authoritative: clients send inputs and receive the world.

## Build and run

Needs Visual Studio with the C++ tools and its CMake component. Everything else is in the repository, and the Vulkan SDK is optional. The server and the tests also build on Linux with CMake and GCC or Clang.

```powershell
.\build.ps1            # debug build, then the tests
.\build.ps1 -Release
build\debug\treasure_planet.exe                 # alone: Vulkan, falling back to OpenGL
build\debug\treasure_planet.exe --renderer gl
```

Multiplayer is a server and any number of clients:

```powershell
build\debug\treasure_server.exe                           # listens on UDP 27500
build\debug\treasure_planet.exe --connect 127.0.0.1        # one window per player
build\debug\treasure_planet.exe --connect 127.0.0.1        # ...so start two
build\debug\treasure_bots.exe --connect 127.0.0.1 --count 50    # headless players, for load
```

`treasure_server` takes `--port N`, `--max-clients N` (default 64) and `--interest METRES` (only send players within that distance of each other; the default sends everyone). The window title shows the connection state, the player count and the round trip.

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
src/net/        the multiplayer stack: UDP transport, wire protocol, zone server, replica client
src/server/     the headless server executable
src/tools/      the bots executable
src/render/     the renderer API and its two backends
src/platform/   window, input and GL context (Windows only for now)
src/client/     the game loop: input in, ticks or packets, draws out
tests/          headless tests of the ECS, the game, and the whole network stack over loopback
tools/          shader compilation, capture comparison
third_party/    GL and Vulkan headers, volk
```

The dependencies point one way: `client -> net -> game -> ecs`, and `client -> render -> platform`. The game and the network stack cannot see the renderer or the window, so the server links `tp_net` alone and runs anywhere.

## Style

C-style C++20: plain structs and free functions. No classes with behaviour, templates, exceptions, RTTI or standard library containers, and the build enforces the exceptions and RTTI parts (`/GR- /EHs-c-`, plus `/W4 /WX`). C++ is used for the small conveniences it offers over C, like struct names without `typedef`.

## The ECS

`src/ecs` is the core of [clockwork-c](https://github.com/Moises-Pirela/clockwork-c) (commit `4602476`) ported to C++, keeping its `cw_` API and behaviour: sparse component pools with stable addresses, cached queries, change observers, command buffers, snapshots, ordered systems with host/client authority, and a fixed-step clock. Its header documents the API and guarantees in detail. Reflection, scenes, assets and game modules were left behind, because they exist to serve clockwork's editor.

## The simulation

All game state lives in one `cw_world`, and everything shares one coordinate space. **Nothing teleports and no level is loaded:** a planet is a place, and a ship is a thing that really flies there.

A ship is a timetable plus a volume. It waits at a dock, then flies a rise, cruise and descend path to the other end of its route. Whatever is inside its volume when it moves gets moved with it. That one rule is the whole of boarding: walk onto the deck and you travel, with no prompt or state change, and any number of players can ride together. Decks are flush with piers and pads, so getting on and off is plain walking.

Adding a planet with its ship is one row in `PLANETS` in [universe.cpp](src/game/universe.cpp).

## Multiplayer

The server is the only simulation. A client never moves anything: it sends its input and draws a replica of the server's world. The same `game` code serves both, in two roles: a `CW_HOST` game runs every system, a `CW_CLIENT` game is a replica the ECS refuses to simulate (systems marked `CW_HOST_ONLY` are skipped), leaving only derived state such as ship parts following their hull.

Everything goes over one UDP socket per peer, in packets of at most 1200 bytes ([net.h](src/net/net.h)). A connection carries two channels: **unreliable** for anything superseded by the next packet, and **reliable, ordered** (cumulative acks, retransmission, a 64-message window with a backlog behind it) for anything that must arrive. The wire format is explicit, little-endian, field by field ([protocol.h](src/net/protocol.h)); no struct is ever copied raw, so a Windows client and a Linux server agree.

Joining: the client repeats `CONNECT` until the server answers with a reliable `WELCOME` (the session id, the client's player entity), then streams the whole world as `SPAWN` records and ends with `BASELINE_END`. Entity ids are the server's, mirrored exactly, so nothing needs translating. Playing: the client sends its input 60 times a second (each message repeats the last three, so a lost packet drops nothing; a jump pressed in any of them still counts), the server applies the newest before each tick, and 20 times a second sends every client a `SNAPSHOT` of the dynamic entities it is interested in: ships always, other players within the interest radius. Joins and leaves arrive as reliable `SPAWN`/`DESPAWN`.

The client keeps a short history of samples per moving entity and renders 100 ms behind the server's clock, interpolating between the two samples around that time. Remote players are smooth at 20 Hz and a lost snapshot costs nothing. The local player is rendered the same way, so their own movement shows one round trip plus the delay late; client side prediction is the next step and this layout (authoritative samples kept apart from the rendered `Transform`) is where it plugs in.

**Scaling.** The unit is the zone server ([server.h](src/net/server.h)): one authoritative game, one socket, one client table, single-threaded, nothing global. Within a zone, per-client cost is bounded by the interest radius rather than the population; across zones, sharding is running more of them, each owning a region of the universe (the hub, a planet) and handing players over with `REDIRECT` as they cross a boundary, which ships flying between regions make a natural event. Entity ids are 64-bit so each shard can own a range and a client can hold entities from several at once. `REDIRECT` is reserved in the protocol and not yet implemented; one zone serves the whole universe today.

[tests/test_net.cpp](tests/test_net.cpp) runs the whole stack in one process over loopback: a server and two clients that see each other as capsules, movement arriving on the other side, a player leaving, a third joining, all of it again through 25-30% simulated packet loss, and the rejections (full server, wrong version, no server).

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
