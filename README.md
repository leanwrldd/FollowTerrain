# Follow Terrain

<img src="logo.png" alt="Follow Terrain" width="400">

Adds a **Follow Terrain** toggle to conveyor belts, pipes, hypertubes and railway tracks. Press **Alt + T**
while placing one and drag it across a hill: it drapes over the ground instead of cutting straight
through it.
Also increases the maximum length of belts, pipes, hypertubes and rails, and the maximum height of
conveyor lifts, with a configurable multiplier that defaults to **8×**.
An optional setting also lets you place these lines and their supports on uneven surfaces.

## Features

- **Allow uneven surfaces:** optionally remove the “Surface is too uneven” restriction for belts,
  conveyor lifts, pipes, hypertubes, railway tracks and their supports. Includes standalone and
  automatically placed supports, stackable supports, and belt, pipe and hypertube wall/ceiling supports.
  Works in every build mode.
- **Longer placements:** belts, pipes, hypertubes and rails default to **8×** their normal maximum
  segment length, and conveyor lifts to **8×** their normal maximum height, in every build mode.
  This also applies to automatically placed lifts. Change **Maximum length multiplier** (1–32×) under
  **Mods → Terrain Belts → settings**. The new value applies when you start a new placement;
  existing built segments are unchanged. Use matching settings on all players and the server.
- **Follow Terrain toggle:** press **Alt + T** while placing a belt, pipe, hypertube or track to switch Follow
  Terrain on or off. While it's on, the *Default*, *Straight* and *Curve* build modes follow the ground:
  the route keeps the shape that mode gives it from above (a straight belt stays straight), and only the
  height changes, so it hugs the slope. Pipe shape modes (*Noodle*, *2D*, *Horizontal to Vertical*) stay
  vanilla. The toggle stays on between placements until you press the key again or restart the game.
  The build gun's key hint bar shows *Follow Terrain: On* or *Off* while you place one of these.
  Rebind it under *Options > Controls > Follow Terrain*.
- **Respects slope and curvature limits:** slopes are capped at the belt's or pipe's maximum incline, and
  bumps are smoothed so the result passes the game's own curvature checks. Where the ground is too steep,
  the line is lifted rather than sunk, so it never ends up underground.
- **Automatic conveyor lifts:** when a belt meets a slope too steep for it, a conveyor lift is placed
  there and the belt carries on from the other end. Belt, lift and second belt are built and charged as
  one placement.
- **Pipe steps:** on big steep drops, pipes get a vertical riser instead of a long ramp.
- **Snapping still works:** the ends of the line are never moved, so snapping to machines, poles and
  other belts behaves as it does in vanilla.
- **Falls back to vanilla:** if a draped shape can't be placed, you get the vanilla shape instead of an
  invalid hologram.

Railway tracks have no build modes of their own, so for them the toggle applies to every placement.

## Settings

Open **Mods → Terrain Belts → settings** to configure:

| Setting | Default | Effect |
| --- | --- | --- |
| **Allow uneven surfaces** | Off | Removes the uneven-surface placement restriction immediately, including during an active placement. |
| **Maximum length multiplier** | 8× | Adjusts maximum segment length and conveyor lift height from 1–32×. Applies when you start a new placement. |

Allowing uneven surfaces keeps the other placement checks, including slope, curvature, snapping,
collision and Follow Terrain's pipe ground-clearance checks. It does not change existing buildings.
Settings are shared across your saves. Use matching values on every player and the server in multiplayer.

## Multiplayer

Every player needs the mod. **Multiplayer and dedicated servers have not been tested yet.** If
something misbehaves for clients, please open an issue.

## Known limitations

- Under overhangs and in caves the ground check can land on the ceiling.
- On hilly ground, railway tracks can only rise, so they may end up raised well above the ground.
- Blueprints and the blueprint designer are not supported yet.

Source: https://github.com/leanwrldd/FollowTerrain
