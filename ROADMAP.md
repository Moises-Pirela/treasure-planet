# Roadmap

Roughly in order, since each step leans on the ones before it.

## 1. Multiplayer

- A headless server executable linking `tp_game`, and a transport.
- The client stops ticking the world itself: it sends `PlayerInput` and receives state. `cw_observer` already reports what changed per component, and `cw_sim` already separates host-only systems from shared ones. Clockwork's original Odin version has inbox, transport, prediction and interpolation helpers worth studying.
- Client-side prediction, including for a player standing on a moving ship.

## 2. Persistence

- World snapshots work and are tested (`test_save_and_restore`), but they are all-or-nothing images in a host-specific raw layout. A live world needs codecs that survive component changes, and accounts and characters that outlive a world.

## 3. A world big enough

These are simplifications that will stop working, listed so that they are not mistaken for decisions:

- **Coordinates are 32-bit floats** in one space. Precision is fine out to the current ~2 km and gets visibly bad past a few tens of km. Interplanetary distances need doubles or sectors.
- **Collision tests every player against every solid**, so it is O(players x solids). It needs a broadphase before it needs anything else.
- **The server should not simulate everything.** Interest management and zones have to be designed along with the coordinates.
- **Entity ids are never reused,** which keeps stale references safe, but the entity table grows with every entity ever created. Turning on `recycle_ids` needs generation counters first, or old references will silently hit new entities.

## 4. Planets as places

- Today each planet is a flat pad on top of a decorative sphere, with gravity pointing down -y everywhere. Real planets need radial gravity, terrain and streaming.
- Levels and content worth travelling for: combat, creatures, and whatever makes this an RPG.

## 5. Ships players build and fly

- **Ships only translate today:** they never rotate, which is what keeps every collision box axis-aligned. A pilotable ship has to turn, so players and hulls need collision in the ship's own frame, and carrying needs to become parenting.
- Piloting, then ship construction, starting from the existing data: a ship is already a volume plus a list of parts.
- Cargo: anything in a ship's volume should ride along, not only players (`system_ships` iterates over players for now).

## 6. Economies

- Resources that differ by planet, cargo that physically has to be carried, and markets and contracts between players. It all rests on persistence and on ships players control.

## Engine work along the way

- **Renderer:** textures, shadows, instancing (each draw is its own call), a real allocator in the Vulkan backend (each buffer is its own allocation), an in-game HUD and text (status lives in the window title).
- **Platform:** Linux, for servers first. The ECS and the simulation are written to be portable, but have only been built with MSVC so far.
- **Tooling:** continuous integration, and running the capture comparison in it. Run the Vulkan backend under the validation layers from the SDK: this hasn't been done yet.
