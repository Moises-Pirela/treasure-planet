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

- Each planet has land (a height field) and a flat pad at its top, and pulls crafts towards its centre (`Gravity`), but walkers still fall along -y everywhere, which only agrees with the planet on its top. Walking anywhere on a planet means a walker's way up following gravity, and their collision with it: the same frame trick crafts use (a player aboard moves in the craft's frame) with the planet's surface as the frame. Then streaming.
- Levels and content worth travelling for: combat, creatures, and whatever makes this an RPG.

## 5. Ships players build and fly

Begun: **crafts** (README, *Crafts*) are ships players own and fly, rigid bodies whose mass, inertia, thrust and torque come from their parts, with a flight computer, contacts with the land and the world's solids, and players aboard moving in the craft's own frame, so ships turn. Their pilot's client predicts them to the bit. They are built from **loadouts** (README, *Ships: hulls and fittings*): four hull classes from a one-man sloop to a galleon for eight, each slot holding a fitting from a catalogue, the stats a shipyard would show worked out by the same sums the flight uses. Players buy hulls and fittings at the **shipwright's** stall in the yard (README, *The shipwright*): a screen over their own ship turning on its berth, orders the server checks and applies to the character, which keeps the fleet (hulls owned, the one sailed, each one's fittings, a store) and the purse with the gear, in the backend. What is left:

- **Earning doubloons.** Every purse is bottomless for now (`character_fit_out` grants it). Doubloons want sources (cargo carried between planets, contracts, what dungeons and wrecks give up) and sinks (repairs, docking fees) before the purses can have bottoms; the books already refuse what a purse with a bottom cannot pay.
- **More looks.** Paint, the cut of the sails and the shape of the hull are chosen at the shipwright's; next could be flags and banners (a player's emblem), figureheads to choose, flame colours for the thrusters, names painted on the stern, and patterns (stripes, chequers) rather than one colour to a part. Shapes are trim on the same hull; shapes that change the hull itself (its length and beam, a tumblehome, a raised deck) would change how she flies, and would be fittings rather than looks.
- **Fittings as things.** Fittings come from a catalogue at a fixed price whatever the hull; they could be items (found in dungeons, salvaged, crafted, traded between players, of a rarity like gear), and a galleon's engine could cost what its size is worth. Selling a hull back, naming ships, paint.
- **Crews by size.** A hull says the crew it is meant for and the panel shows it, but nobody is turned away for being one too many; it could limit who may come aboard (with the owner's say over who), and bigger hulls could need more hands to handle (a mast without a hand, a sail that cannot be worked).
- **More to fit.** The hull itself (planking: light, heavy, ironclad, which wants damage to mean anything), thrusters that swivel, cargo holds, named ships, paint. Moving fittings about (slots are where the hull puts them) needs the rules a free design must keep (parts joined up, a helm, thrust enough to lift itself).
- **Navigation and sailing.** The sail braces round and a ship makes headway into the current at four tenths of its speed; the model is a game's, not an aerofoil's (a sailing speed for each angle to the current, scaled by trim). The solar current is switched off for now (`--currents` brings it back) while the flying itself is worked on: with it off, set sails keep a ship's way (she flies on as she was going, with nobody at the helm) and draw nothing. Ships of two and three masts carry a sail on each, each worked by its own hand. Next: sails of other cuts (a jib, fore-and-aft sails that point higher into the current), plotting a course on the chart (a planet, a point) that the helm holds to, and currents worth finding (rivers of fast flow between the planets, slack water) to make the chart a tool. The pilot's client cannot know the sail hand's inputs, so it mispredicts a little while the sails are worked; carrying the hand's inputs in the pilot's snapshots would fix that.
- **Docks and captains.** Ships dock at the hub and by every planet, brought in by their autopilot, and captains of the world's own sail between them. Next: docks that charge (and a harbourmaster's queue when they are full), ships laid up at a dock kept there while their owner is away, captains with cargo and contracts (the economy's carriers), captains who hire players as crew, and a captain's route across zones (a captain only flies in the world of the server that owns the hub).
- **Crews.** An owner can put anyone off (for a minute); next are crews with standing: friends or a party aboard by right, a mate who may take the helm, a ship closed to all but its crew.
- **Keeping ships.** What a player's ships are is saved with their character; the craft itself (where it is, who is aboard) is not: it is built at the yard when called and put away a while after its owner leaves the server.
- **Ships across zones.** A crew stays on its craft's server until it walks off; a craft flying into a place another zone owns should be handed over with everyone aboard, which is a ticket for a vessel, or crafts need a zone of their own.
- **Physics still simplified:** thrusters push along the craft's axes only (any direction needs an allocation of thrust between them, a small optimisation), inertia is taken as principal along the craft's axes, crafts do not collide with each other, crew and cargo weigh nothing, and a craft resting on a moving liner is not carried by it. Planets pull crafts towards their centres and crafts keep to that way up, but walkers still fall along the world's -y (below); drag comes with atmospheres.
- **Liners onto the same footing:** the timetable ships still carry by volume and only translate; they could be crafts flown by their timetable.
- Cargo: anything in a ship's volume should ride along, not only players (`system_ships` iterates over players for now; a craft carries only players `Aboard`).

## 6. Economies

- Resources that differ by planet, cargo that physically has to be carried, and markets and contracts between players. It all rests on persistence and on ships players control.

## Engine work along the way

- **Renderer:** textures, shadows, instancing (each draw is its own call), a real allocator in the Vulkan backend (each buffer is its own allocation), a real UI pass (the overlay, screens like the shipwright's included, is thousands of tiny quads, each a draw: a texture and an orthographic projection would make it a handful).
- **Platform:** packaging, such as bundling MoltenVK with a macOS build. Windows, Linux and macOS share one platform layer on GLFW.
- **Tooling:** running the Vulkan validation layers in CI, which so far builds, tests and compares captures. Core and synchronization validation are clean today. Best-practice validation only flags the per-buffer allocations above and one access mask in the capture path.
