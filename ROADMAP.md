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

Begun: **crafts** (README, *Crafts*) are ships players own and fly, rigid bodies whose mass, inertia, thrust and torque come from their parts, with a flight computer, contacts with the land and the world's solids, and players aboard moving in the craft's own frame, so ships turn. Their pilot's client predicts them to the bit. What is left:

- **Building ships.** A craft is already its parts and refits from them; what is missing is changing them: adding, removing and moving parts at the yard (and a way to show it), with the rules a design must keep (parts joined up, a helm, thrust enough to lift itself), and designs players own rather than the `DESIGNS` table.
- **Keeping ships.** Crafts are not saved: one is handed out at the yard and put away a while after its owner leaves the server. They belong with the character, in the backend (social.h), like gear.
- **Ships across zones.** A crew stays on its craft's server until it walks off; a craft flying into a place another zone owns should be handed over with everyone aboard, which is a ticket for a vessel, or crafts need a zone of their own.
- **Physics still simplified:** thrusters push along the craft's axes only (any direction needs an allocation of thrust between them, a small optimisation), inertia is taken as principal along the craft's axes, crafts do not collide with each other, crew and cargo weigh nothing, a craft resting on a moving liner is not carried by it, and nobody sees anyone else's engines fire (the throttles are not replicated). Drag and radial gravity come with planets as places.
- **Liners onto the same footing:** the timetable ships still carry by volume and only translate; they could be crafts flown by their timetable.
- Cargo: anything in a ship's volume should ride along, not only players (`system_ships` iterates over players for now; a craft carries only players `Aboard`).

## 6. Economies

- Resources that differ by planet, cargo that physically has to be carried, and markets and contracts between players. It all rests on persistence and on ships players control.

## Engine work along the way

- **Renderer:** textures, shadows, instancing (each draw is its own call), a real allocator in the Vulkan backend (each buffer is its own allocation), an in-game HUD and text (status lives in the window title).
- **Platform:** packaging, such as bundling MoltenVK with a macOS build. Windows, Linux and macOS share one platform layer on GLFW.
- **Tooling:** running the Vulkan validation layers in CI, which so far builds, tests and compares captures. Core and synchronization validation are clean today. Best-practice validation only flags the per-buffer allocations above and one access mask in the capture path.
