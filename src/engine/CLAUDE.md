# engine/

The loop and everything shared by all games. Never includes `games/`. Design: `docs/ARCHITECTURE.md` (Application, Settings, Commands, Main loop).

- Declare the game before the `Application` so it outlives the engine.
- **Settings:** a field named once in its struct's `visit()` gets F5, `settings.cfg` and the defaults tests for free. `settings.cfg` holds only values changed from the defaults, each game's under its `title()`'s section, other games' kept on save.
- **Commands:** GPU-free ones in `engine_commands`, the `Application`'s (screenshot, stats, wait, quit) in `application_commands.cpp`; a game's own through `Game::addCommands`. They run between frames, before the frame's time is measured. Test each through `CommandRegistry::execute`.
- **Ticks:** 120 Hz `FixedTimestep`; a frame runs its ticks within `maxTickSecondsPerFrame` and drops the rest (slow motion, no spiral). Key and click edges are handled once a frame and reach ticks as commands.
- Moving entities have a `PreviousTransform` and are drawn interpolated (`getRenderTransform`); teleporting ones have none.
- The future editor's operations will be commands in the same `CommandRegistry`, its buttons issuing them.
