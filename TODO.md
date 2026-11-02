# TerrainBelts: planned work

Do these once Follow Terrain on belts is confirmed working in game.

## Same draping logic for other splines (written 2026-11-02, not yet tested in game)
- Pipes and rails now share `DrapeSpline` with belts; per-type step, clearance, incline and
  smoothing are constants at the top of `TerrainBeltsModule.cpp`.
- Rails have no build modes of their own, so they get a "Default" mode plus Follow Terrain.
- Still to decide for rails: on hilly ground the slope pass only raises the track, so it can
  end up high in the air. Consider cutting into hills, or rejecting the track, instead.

## Auto-place a lift on slopes too steep for a belt (first version written 2026-11-02, untested)
Implemented behind `TERRAINBELTS_ENABLE_AUTO_LIFT`: one lift at the biggest steep stretch,
only when the belt is dragged with the flow (source to destination), lift and second belt
spawned as disabled children of the belt hologram, wired together in ConfigureComponents.
Unknowns left for in-game testing: whether the lift hologram's connection components follow
its top transform (the log warns if not), clearance between the lift and the cliff,
multiplayer construct of the children, and whether the end snap survives the split.

Conveyor chain actors (warning from an experienced modder: they "can bite you, and corrupt
saves"): each belt saves a pointer to its `AFGConveyorChainActor` (`mConveyorChainActor` is
`SaveGame`). `AFGBuildableSubsystem` groups connected belts into tick groups when they begin
play, and gives new groups chain actors later. When a chain changes, it removes the chain
actor and rebuilds it. Auto-lift connects the pieces in `ConfigureComponents`, before the later
piece begins play. That's the same order as vanilla snapping a new belt onto an existing one,
but it hasn't been verified. Before release:
- Build a belt-lift-belt split, run items through it, save, reload: items keep flowing and
  nothing is duplicated or lost.
- Dismantle each piece of a split in turn, save and reload after each.
- Build a split onto an existing, running belt line (end snapped to a belt), then save and reload.
- Search the log for conveyor chain warnings and ensures after each step.
- Repeat with a belt dragged against the flow (from a machine input toward an output): items
  must flow through every lift and belt piece the right way.
- If anything looks off, connect the pieces the way vanilla does when a belt snaps to an
  existing connection (snapped connection on the hologram) instead of calling `SetConnection`
  ourselves.

Original notes:
When the ground climbs or drops faster than the belt's max incline allows, split the belt
there, place a conveyor lift to cover the height difference, and keep going with a second
belt from the top (or bottom) of the lift.

Open questions:
- How to spawn extra child holograms (a lift plus a second belt) from one belt hologram so
  they're built together, charged together and replicated. The belt already spawns pole
  children; check how `SpawnChildren` and the construct message handle that.
- Where to split: the first point where the slope pass would have to lift the belt more
  than some threshold above the ground.
- Lifts are vertical and have a minimum height, so check the space needed at each end.
