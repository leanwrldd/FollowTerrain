# Changelog

## 1.2.3

### Fixed
- Under a rock arch, cave roof or other overhang, Follow Terrain always went over the top, with
  automatic lifts up and back down, even when both ends were on the open ground underneath. Now
  each overhang is passed under, with any lifts at either end of it, unless going over climbs
  clearly less. Passing under needs at least 4 m of open space, and a belt placed along the top
  of an arch stays on top.
- A short belt between two machine ports bent down towards uneven ground with Follow Terrain on.
  A belt that already runs clear of the ground, at most about 2 m above it, now keeps vanilla's
  straight shape.
- A belt dragged from the ground onto a rounded top, such as a rock arch, got no automatic lift
  and was left too steep. Where the belt can't keep its full clearance over a crest, it now runs
  a little closer to the ground instead of being rejected.

## 1.2.2

### Fixed
- A pipe ending in a vertical riser (for example Straight mode into a floor hole) could get a
  wide, bulging elbow with Follow Terrain on. It now keeps vanilla's tight bend when the terrain
  shape doesn't fit.
- A pipe with no ground under it (over a gap or between floors) came out wavier and higher than
  vanilla's with Follow Terrain on. With nothing to follow, it now keeps vanilla's shape.
- Belts and pipes running alongside a foundation wall were lifted as if the foundation's top were
  ground under them, turning the curve into a stiff climb.
- "Surface is too uneven" could still show on pipes and their supports with Allow uneven surfaces
  on, when the game raised it outside the placement check the mod filtered.
- With Follow Terrain on, a pipe whose route clipped a foundation (for example running from the
  top of a foundation down its side) was blocked with "Surface is too uneven". Vanilla allows it
  with a clearance warning, and now so does the mod. Pipes running into terrain are still blocked.

## 1.2.1

### Fixed
- Lifts placed automatically on steep ground were removed right after building when Construction
  Preferences is installed with its conveyor pole type set to none. It deleted every part built
  alongside a belt, taking them all for poles. The belt pieces stayed, but the lifts were gone.

## 1.2.0

### Changed
- **Allow uneven surfaces is now on by default.**

### Added
- Mod icon, shown in the in-game mod list.

### Fixed
- Fixed a possible memory overrun when filling the build gun key hint bar (Follow Terrain: On/Off row).

## 1.1.0

### Changed
- **Follow Terrain is now a toggle, not a build mode.** Press **Alt + T** while placing a belt, pipe,
  hypertube or railway track to switch it on or off. The old *Follow Terrain* entry in the build mode
  cycle is gone.
- While the toggle is on, the *Default*, *Straight* and *Curve* build modes follow the ground. The route
  keeps the shape that mode gives it from above (a straight belt stays straight) and only the height changes.
- Pipe shape modes (*Noodle*, *2D*, *Horizontal to Vertical*) stay vanilla.
- Railway tracks have no build modes, so for them the toggle applies to every placement.
- The toggle stays on between placements until you press the key again or restart the game.

### Added
- Rebindable key: **Options > Controls > Follow Terrain**.
- Build gun key hint bar shows *Follow Terrain: On* or *Off* (with its key) while placing a supported
  piece, so the state is visible even on flat ground.
- Placement settings, and terrain route fixes (from the earlier commits in this release).

### Technical
- The mod now ships content (input action and mapping context in `Content/Input`), so
  `CanContainContent` is `true`.
- New module dependencies: InputCore, EnhancedInput, UMG, Slate, SlateCore.
- Removed `TerrainBeltsBuildMode.h`.
- Added `Tools/create_input_assets.py` to generate the input assets.

## 1.0.0
- Initial release: Follow Terrain build mode for belts, pipes and tracks, with longer maximum lengths.
