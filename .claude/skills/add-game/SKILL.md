---
name: add-game
description: Use when adding a new game (a new --game name) to the engine.
---

# Add a game

1. Research and plan first, and put it on `ROADMAP.md` (root `CLAUDE.md` rules).
2. Folder `src/games/<name>/` with a class implementing `Game` (`src/engine/game.h`): `title()`, `onInit`, `onInput`, `onFixedUpdate`, `onUpdate`, `getCamera`, `wantsMouseLook`; optional `visitSettings`, `addCommands`, `goldenViews`, `prepareGoldenView`, `poseCamera`.
3. Never include `platform/`, `render/` or SDL: the game must run headless. Hold a `PlayerControls` for the camera and F1-F3 modes.
4. Settings in a struct with `visit()` (saved under the game's `title()` in `settings.cfg`); actions that are not values as commands.
5. One line in `src/app/game_registry.cpp`'s `games` table.
6. Tests: build it headless with `testJobs` (`add-test` skill).
7. Golden references: ask the user, then `make golden-save GAME=<name>` (`golden` skill).
8. `README.md`: the game under Features, its controls under `## Controls`.
