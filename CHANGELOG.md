# Changelog

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
