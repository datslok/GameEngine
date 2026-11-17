# Games

Covers: `src/app/game_registry`

Every game the program can run, and where each is described. Each game builds on the `Game` interface (`engine/game.h`) and never includes `render/`, `platform/` or SDL, so it runs headless in tests. Adding one: the `add-game` skill.

## The registry

- `GameEntry {name, make}`: `make` is a plain function pointer making a fresh game, since the golden images need one per view; the table is `constexpr`.
- `registeredGames()`, in help order: `lightlab`, `demo`, `clothlab`, `forest`. The names are the golden folders' names (`tests/golden/<GPU>/<game>/`), so they follow the golden view names' rules.
- `defaultGame()` (the first: the light lab), `findGame(name)` (nullptr when unknown), `gameNames()` for help and errors.
- The registry makes every game in `ControlMode::FirstPerson` with mode switching on (F1 first person, F2 MOBA, F3 free camera); the games' own constructors default to other modes for tests.

## Launch options

`parseLaunchOptions(arguments)` → `LaunchOptions`:

| Option | Effect |
|---|---|
| `--game <name>` | the game; none means the default, or every game for the golden images |
| `--golden-save`, `--golden-check` | golden images instead of playing (`GoldenMode`) |
| `--cloth-yardstick`, `-quick`, `-dress` | the cloth yardstick (`YardstickRun`, `app/cloth_yardstick_run`); plays no game, takes no other option |
| `--set key=value` (repeatable), `--run <file>`, `--port`, `--hidden`, `--typical-gpu` | play options: settings before anything reads them, a command script, the command port, a window that never shows, the typical GPU setup |
| `--send <command...>` | talks to a running game and exits; only `--port` may come before it |

- Each option at most once (but `--set`), in any order. Anything else throws `std::invalid_argument` naming the valid choices; play options with golden images or the yardstick throw too.
- How commands run once a game is up: `settings-and-commands.md`.

## The games

| Game | Title | Folder | Reference |
|---|---|---|---|
| `lightlab` | Light lab | `games/lightlab/` | `light-lab.md` |
| `demo` | Demo | `games/demo/` | `gameplay.md` |
| `clothlab` | Cloth lab | `games/clothlab/` | `cloth-lab.md` |
| `forest` | Forest | `games/forest/` | `forest.md` |

The title names each game's section in `settings.cfg` and its F5 group (`lightLab.`, `clothLab.`, `forest.` keys). The sun, moon and stars the labs and the forest place: `sun-moon-stars.md`.

Tests: `game_registry_test`.
