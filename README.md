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

`treasure_server` takes `--port N`, `--max-clients N` (default 64), `--interest METRES` (only send players within that distance of each other; the default sends everyone), `--trace` (log every player's position once a second) and, for several servers sharing the universe, `--zones FILE --zone NAME` (see Multiplayer below). The window title shows the connection state, the player count, the input round trip and the number of zone hand-offs so far.

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
src/net/        the multiplayer stack: UDP transport, wire protocol, snapshots, zones and tickets,
                the zone server, the predicting client
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

The server is the only authority. A client sends inputs and receives the world; what it simulates itself, it simulates only to hide latency, and the server's word replaces it whenever they disagree. The same `game` code runs in both roles: a `CW_HOST` game owns the truth, a `CW_CLIENT` game is a replica that predicts what it can.

**Transport.** Everything goes over one UDP socket per peer, in packets of at most 1200 bytes ([net.h](src/net/net.h)). A connection carries two channels: **unreliable** for anything superseded by the next packet, and **reliable, ordered** (cumulative acks, retransmission, a 64-message window with a backlog behind it) for anything that must arrive. The wire format is explicit, little-endian, field by field ([protocol.h](src/net/protocol.h)); no struct is ever copied raw, so a Windows client and a Linux server agree.

**Joining.** The client repeats `CONNECT` until the server answers with a reliable `WELCOME` (the session, the client's player entity, the universe clock), then streams the whole world as `SPAWN` records and ends with `BASELINE_END`. Entity ids are the server's, mirrored exactly. Joins and leaves arrive later as reliable `SPAWN`/`DESPAWN`.

**Universe time and ships.** Ships follow a timetable read off *universe time*, the wall clock shared by every server and client, rather than integrating their motion. So a ship is a pure function of time: two servers that have never spoken put it in the same place, a client predicts it exactly, and snapshots never mention ships at all. The server's tick is driven by that clock rather than by its own loop: after a stall it catches up (to a second's worth, then skips the rest: ships land where they should, players simply did not move), and after a real clock jump universe time moves with it, so no zone ever drifts from another. `treasure_server --universe-epoch` shifts the clock, for tests; every zone must be given the same value.

**Prediction.** The client runs the real movement systems on its own player, one tick ahead of the server, numbering every input and remembering the position each one led to. The server queues inputs and consumes exactly one per tick (holding two as a jitter buffer), and each snapshot says which input it had applied and how many are still waiting. The client then sets its player to the server's state at that tick, replays the inputs the server has not seen yet with `game_replay_tick`, and compares. Usually the result is identical to the millimetre; when it is not (a collision the server saw first, a late input the server had to repeat), the simulation is corrected at once and the picture catches up through a view offset that fades in a tenth of a second. The replay also anchors the client's tick counter to the server's, which is what keeps the predicted ships where the server has them, passengers included, and the queue depth steers the client's tick rate (by up to a tenth) so that its clock follows the server's rather than its own. Players a client does not own are not simulated: they arrive in snapshots and are rendered 100 ms behind the server's clock (estimated from the fastest snapshots of the last two seconds), interpolated between samples, in the frame of the ship they stand on.

**Snapshots** ([snapshot.h](src/net/snapshot.h)) are quantised (millimetres, 16-bit angles) and delta-coded against the last snapshot the client acknowledged: a player standing still costs two bytes, a moving one six or so, and a snapshot that gets lost costs nothing but the next one being coded against an older base. Players are prioritised by distance within the packet budget, and an interest radius (`--interest`) leaves distant players out altogether, so a client's traffic is bounded by how crowded its surroundings are, not by the population. Four players walking together is about 1 KB/s each.

**Zones** ([zones.h](src/net/zones.h), [server.h](src/net/server.h)). The universe is divided by *place*: the hub, each planet's landing pad. A zone server owns some places and every player standing in them; it simulates the whole static universe and every ship regardless, since those cost nothing and agree everywhere. When a player walks into a place another zone owns, their server issues a *ticket* (their state, signed with the shared secret by SipHash), redirects the client, and freezes the player: inputs are ignored, physics carries on, so the ticket stays true. The client freezes its prediction the same way, connects to the new zone with the ticket, builds the new world alongside the old one, and switches only once the new zone has shown it where it stands, bridging the two predictions with the view offset; then it leaves the old zone, which despawns the frozen body. If the new zone refuses or never answers, the client says so and the old zone thaws the player and tries again later; a client that vanishes mid-way is thawed after four seconds and dropped after five. A ticket is good once (a zone remembers the ones it accepted), and a player arriving by ticket replaces any stale connection of the same account. Servers never talk to each other. A ship flight from the hub to a planet is therefore a hand-off between processes, taking a few tens of milliseconds on a LAN, and [zones.example.txt](zones.example.txt) is everything needed to run two of them. Today a zone is one process and one core; the next step up is several zones per process and a directory service in place of the static file.

```powershell
build\debug\treasure_server.exe --zones zones.example.txt --zone hub
build\debug\treasure_server.exe --zones zones.example.txt --zone planets
build\debug\treasure_planet.exe --connect 127.0.0.1:27500
build\debug\treasure_bots.exe --connect 127.0.0.1:27500 --count 3 --ride     # ride the ships between zones
```

[tests/test_net.cpp](tests/test_net.cpp) runs all of it in one process over loopback: the reliable channel through 30% loss, the snapshot codec, ticket signatures (against the SipHash reference vector), two clients seeing each other as capsules with prediction agreeing with the server to the millimetre and their tick rates holding the server's input queue at its target, a player leaving and another joining, the same again through 25% loss, the rejections, two zones handing a player over (a replayed ticket and a forged one refused), a hand-off to a zone that never answers (frozen, then thawed, nothing lost), and the server's tick following the wall clock through a stall, a long stall and a clock jump.

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
