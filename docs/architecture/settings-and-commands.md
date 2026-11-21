# Settings and commands

Covers: `src/core/settings_visitor`, `src/engine/settings_file`, `src/engine/debug_ui`, `src/render/gpu/quality_preset`, `src/render/gpu/typical_gpu`, `src/render/vulkan/vulkan_overlay`, `src/render/gpu/overlay_composite`, `assets/shaders/overlay.glsl`, `assets/shaders/overlay.frag`, `src/core/command_registry`, `src/engine/engine_commands`, `src/engine/application_commands.cpp`, `src/platform/command_socket`, `src/app/game_registry`, `src/app/main.cpp`

Every tunable value described once, and the text interface that drives a running game. The settings structs themselves: `engine-core.md` (Settings structs). Driving a game: the `drive-game` skill.

## The visitor

One `visit(settings, SettingsVisitor&)` per settings struct names each field once; F5, `settings.cfg`, the commands and the defaults' tests all walk it, so a new field needs only that line.

- `SettingsVisitor` (`core/settings_visitor.h`): public `field`, `choice`, `group`, `needingRayQueries`; a visitor implements the protected `visitFloat`, `visitInt`, `visitBool`, `visitChoice`, `visitGroup` (and optionally `visitNeedingRayQueries`).
- Every other type is converted in the base, once, so visitors see four kinds:

| Field type | Seen as | Note |
|---|---|---|
| `float`, `int`, `bool` | itself | |
| `double` | float | written back only if changed, so looking never rounds it |
| `std::uint32_t`, `std::uint64_t` | int | limits must fit an int; negatives become 0 |
| enum (`choice`) | index | values 0, 1, 2... matching a `std::array` of names |
| `group` | label + callback | a visitor may skip the contents (closed tree node) |

- **Limits** (`FloatLimits`, `IntLimits`): `min`/`max` are hard (the file clamps to them; tests check defaults inside); `sliderMin`/`sliderMax` the useful range, defaulting to the hard limits, so a field without finite hard limits must give them; `unit`; `applies` (`FieldApplies::EveryFrame` or `AtStartup`).
- `needingRayQueries(contents)`: tags fields that work only with hardware ray queries (ray tracing, its debug view, ray-traced reflections, the mirrored lights' search, the light probes). F5 greys them and `set` warns on GPUs without; other visitors just walk.
- `SettingsWalk` = `std::function<void(SettingsVisitor&)>`.
- **Keys** (`settingKey`): the label's letter-and-digit runs in lower camel case, the first run lowered whole ("GPU validation" → `gpuValidation`, "Target FPS" → `targetFPS`); groups join with dots (`engine.render.bloom.strength`). Name labels after their members.
- Hard limits the renderer also enforces share one constant (`maxBloomStrength`, `BLOOM_LEVEL_LIMIT`); a test checks the ends pass `validateBloom` and `validateToneMapping`.

## The settings file

`engine/settings_file`, one `key = value` line per changed field. Saving only differences lets an improved default reach everyone who never touched it; deleting a line restores the default.

| Function | Does |
|---|---|
| `captureSettings(walk)` | every field as `SettingEntry {key, value}`; floats as the shortest exact text (`std::to_chars`), bools `true`/`false`, enums by name |
| `applySettings(entries, walk, StartupFields)` | sets named fields; clamps to hard limits; skips unreadable values and unknown keys, a warning each; `Keep` leaves start-up fields alone |
| `formatSettingsText(current, defaults, kept)` | a two-line `#` header, each value whose text differs from its default, then `kept` verbatim |
| `parseSettingsText` | `key = value` lines; blanks and `#` lines skipped; `\r\n` accepted; malformed lines warned by number |
| `separateSections(entries, walk)` | `own` (first key part is a section the walk has, or no dot, so strays still warn) and `others` (another game's section) |
| `engineAndGameSettings(settings, game)` | the whole tree: `engine` (`engineSettings(settings)`), then a group named by `Game::title()` (`demo.grenade.throwSpeed`, `lightLab.sun.solarHours`) |
| `engineSettings(settings)` | the `engine` part alone (`--send` reads the port with it, no game needed) |
| `describeSettings(walk)` | `SettingDescription {key, value, kind, atStartup, needsRayQueries}` in capture order; `kind` for people: "number 0 to 1 (slider 0 to 0.2) nits", "whole number 1 to 8", "true or false", "one of Vsync, Mailbox, Immediate"; a missing limit gives "from 0" or "up to 5" |
| `setSetting(walk, key, value)` | one field live, as the file would, returning `SettingChange {outcome, value, message}`; outcomes `Set`, `Clamped`, `Refused`, `Unknown`, `AtStartupOnly`; the same `ApplyingVisitor` as the file, counting refusals apart from clamps |
| `readSettingsFile`, `writeSettingsFile` | nothing when absent; throw `std::runtime_error` when unreadable or unwritable |

**Load, save, reset** (`Application`):

- File: `savedSettingsPath` = `settings.cfg` in the working directory, git-ignored, shared by every game.
- `captureDefaultsAndLoad` (in the constructor's initialiser, before anything reads a start-up value): capture defaults; apply the file's `own` entries with start-up fields; then the `--set` entries. File problems are `SDL_Log`ged, never fatal; a bad `--set` throws `std::invalid_argument` (whoever typed it is there). `SavedSettings::Ignore` (golden captures) skips the file.
- `saveSettings` (F5's Save, `save-settings`): changed values plus the other games' sections, re-read from the file at save time so nothing another game saved meanwhile is lost.
- Reset: the captured defaults with `StartupFields::Keep`, since the running engine was built from the start-up values.

## Debug UI (F5)

`DebugUi` (`engine/debug_ui`) owns the ImGui context and the SDL3 backend (`ImGui_ImplSDL3_InitForVulkan`), and starts the renderer's half (`VulkanRenderer::startDebugUi`, ImGui's Vulkan backend). Declared after the window and renderer; its cleanup calls `stopDebugUi` first, which waits for the GPU before freeing the UI's buffers.

- No `imgui.ini`; keyboard navigation off, so the game keeps its keys while tuning; style sizes and font scaled by `SDL_GetWindowDisplayScale`.
- F5 toggles it (`Application::updateDebugUi`, before `game.onInput`); open, the window gets the mouse and ImGui eats the events it wants (`Window::setDebugUiOpen`).
- `build(walk, render, typicalGpu, requests)`: `nullptr` while closed, nothing built. Open:
  1. `beginDebugUiFrame`, the SDL3 and ImGui `NewFrame`s.
  2. Window "Settings (F5)": the typical GPU checkbox (`drawTypicalGpu`), the quality preset combo (`Custom` when `matchingQualityPreset` finds none), the upscaling mode combo (sets `renderScale`; `Custom` when `matchingUpscalingMode` finds none).
  3. Save and "Reset to defaults" buttons, reported in `DebugUiRequests` and carried out by `Application`, which sets the status line under them.
  4. The walk through `ImGuiSettingsVisitor`.
- **Controls:** top-level groups are collapsing headers (open), deeper ones tree nodes, each in its own ID scope; floats and ints are sliders (`%.4g` plus unit), clamped to the hard limits after Ctrl+click typing; bools checkboxes; choices combos. Start-up fields disabled with a tooltip; so are `needingRayQueries` fields when `VulkanRenderer::supportsRayQueries` is false, the tooltip saying why.

### Drawing it

`VulkanRenderer::render(frame, settings, ui)`, `ui` an `ImDrawData*` or null. `VulkanOverlay` (`render/vulkan/vulkan_overlay`; its composite pipeline is rebuilt with the screen pipelines when the swapchain format changes):

1. `drawUi` (`GpuPass::DebugUi`), early in `drawScene` while no pass is open: uploads ImGui's changed textures and runs `ImGui_ImplVulkan_RenderDrawData` into the overlay image, `vulkanOverlayFormat` (`R8G8B8A8_UNORM`, frame-sized, cleared transparent), so it ends premultiplied as on an 8-bit screen.
2. `composite` (`GpuPass::Overlay`), after tone mapping and the debug lines: `fullscreen_triangle.vert` + `overlay.frag`, the overlay sampled nearest, `OverlayData` holding the white level, premultiplied blending (`ONE, ONE_MINUS_SRC_ALPHA`). Per texel (`overlayOutput` in `overlay.glsl`): colour / alpha clamped to 1, sRGB decoded (`srgbToLinear`, `dither.glsl`), times alpha and `DisplayOutput::whiteLevel`.
3. Mirrored in `render/gpu/overlay_composite` (`overlayOutput`, `blendOverlay`), checked on the GPU too.

Dear ImGui is vendored unmodified (`external/imgui`, version in its `VERSION.txt`); build flags: `build.md`.

## Quality presets

`render/gpu/quality_preset`: `QualityPreset` Low, Medium, High, Ultra (`qualityPresetNames`). One private table (`QualityValues`) feeds `applyQualityPreset` and `matchingQualityPreset`, so they cannot disagree. Taste (tone mapping, exposure, bloom strength, HDR, camera finish, upscaling) is never touched, nor `RenderSettings::rayTracing` (its own choice).

- **Ultra** = `RenderSettings{}`, the default: read from the defaults, so retuning a default retunes Ultra.
- **High** = Ultra without ray-traced reflections and mirrored lights, probe sharing 0.1, reflection probe 128, ambient occlusion 2 slices and 4 steps.

| Value | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Shadowed point / spot caps | 8 / 4 | 24 / 8 | 64 / 8 | 64 / 8 |
| Texel targets (point / spot) | 0.1 / 0.05 | 0.06 / 0.03 | 0.04 / 0.02 | 0.04 / 0.02 |
| Sun shadow distance, cascades | 60 m, 3 | 120 m, 3 | 200 m, 4 | 200 m, 4 |
| Cascade resolution | 2048 | 2048 | 4096 | 4096 |
| Bloom levels | 4 | 5 | 6 | 6 |
| Probe rays / fast rays, sharing | 32 / 32, 0.5 | 64 / 64, 0.3 | 256 / 256, 0.1 | 256 / 256, 0 |
| Probe cascades, densest spacing | 4, 1 m | 5, 0.5 m | 6, 0.5 m | 6, 0.5 m |
| Densest / other cascades (across x up) | 19 x 11 / 15 x 9 | 25 x 15 / 17 x 11 | 31 x 17 / 21 x 13 | 31 x 17 / 21 x 13 |
| Reflections: steps, probe size | off | 32, 64 | 64, 128 | 64, 256 |
| Ambient occlusion: slices, steps | off | 2, 3 | 2, 4 | 3, 6 |
| Contact shadows, sky visibility | off | on | on | on |
| Volumetric fog grid | off | 128 x 72 x 48 | 160 x 90 x 64 | 160 x 90 x 64 |
| Ray-traced reflections, mirrored lights | off | off | off | on (only while ray tracing is on) |

### The typical GPU setup

`render/gpu/typical_gpu`: the engine as a mid-range PC draws it, to judge features as most players see them.

- `applyTypicalGpu(settings, support)`: the High preset, ray tracing off, render scale `upscalingModeScale(UpscalingMode::Quality)`, upscaler `typicalUpscaler(support)`: DLSS where it runs, else FSR, else TAA. Nothing else changes.
- `TypicalGpuView`: `turnOn` saves the settings it found (first time only) and applies the setup (again, if already on, in case another upscaler became available); `turnOff` restores them exactly; off while off does nothing.
- `Application` owns one; used by F5's checkbox, `typical-gpu [on|off]` and `--typical-gpu`.

## Commands

The engine's text interface: scripts, tools and AIs drive a running game by name and see the effect in its window. Editor operations are meant to join the same registry, so a script can do whatever the editor's buttons do.

### The registry

`CommandRegistry` (`core/command_registry`):

- `add(name, usage, description, handler)`: names are lower-case letters, digits and dashes, unique (`std::invalid_argument` otherwise); kept in name order. Built with `help` (`help` lists every usage and description, `help <name>` one); `help` points at its registry, so registries are not copyable.
- `execute(line)`: `splitCommandWords`, then the first word's handler on the rest (`CommandArguments`).
- **Splitting:** spaces and tabs; double quotes keep spaces and, inside them, a backslash escapes a quote or backslash; outside quotes a backslash is ordinary (Windows paths need no quoting); quotes join onto the surrounding word, as in a shell. `joinCommandWords` is the inverse, quoting only words that need it.
- **Errors are replies, never exceptions:** empty line; unknown name (up to three suggestions from `closestNames`: candidates containing the text ignoring case, then those within edit distance of a third of its letters (at least two), against the whole candidate or its last dotted part); unclosed quote; any `std::exception` a handler throws (`floatArgument`, `intArgument` throw `std::invalid_argument` naming what was wanted; no infinities or NaN, no trailing junk).
- `CommandReply {ok, text}` from `commandOk` / `commandError`. Wire format (`encodeReply` / `decodeReply`): `OK` or `ERROR`, newline, text; anything else decodes as an error holding what came.

### Engine commands

`engine/engine_commands`: the ones needing no window or GPU, so tests run them headless (`engine_commands_test`).

| Command | Registered by | Does |
|---|---|---|
| `set <key> <value>`, `set <key>=<value>` | `addSettingsCommands(registry, walk, defaults, rayQueries)` | `setSetting`; replies the new value, or the clamp's reason; a `needsRayQueries` field on a GPU without them is set but the reply says it has no effect; unknown key → nearest keys; start-up field → refused, with the `--set` form to use or "save it" |
| `get [key or group]` | same | values |
| `list [key or group]` | same | values with `describeSettings`' kind and "read only at start-up" |
| `reset <key, group or all>` | same | defaults with `StartupFields::Keep`, says how many start-up fields it left |
| `pause`, `resume` | `addSimulationCommands(registry, control, runTick)` | `SimulationControl::paused`; `ticksThisFrame` gives 0 while paused and the time is dropped |
| `step [ticks]` | same | pauses, runs 1 to `maxStepTicks` (100,000) ticks at once through `runTick`; reaches the very state of ticking (tested with `hashSimulationState`) |
| `camera [x y z [tx ty tz]]` | `addCameraCommands(registry, game, world)` | shows `cameraText`, or poses through `Game::poseCamera` (three numbers keep the direction); straight up or down, or at its own place, refused (world up is camera up); a game returning false refuses by its title |
| `debug-view [on\|off]` | `addDebugViewCommand(registry, game)` | `Game::wantsDebugView` / `setDebugView` (the F4 view: the readout, and the boxes and colliders with `debugBoundingBoxes` on); a game without one refuses |
| `preset [Low\|Medium\|High\|Ultra]` | `addRenderCommands(registry, render, typicalGpu, support)` | the matching preset (Custom when none) or applies one, name in any case |
| `typical-gpu [on\|off]` | same | through `TypicalGpuView`; reports the upscaler chosen from `support` (`VulkanRenderer::upscalerSupport`) |

- Key or group: a group's name must end at a dot (`engine.render.bloom` matches, `engine.render.bloo` does not); `all` or nothing means every field.
- All four games implement `poseCamera` (via `PlayerControls::poseFreeCamera`, so the free camera takes the pose and holds it) and `setDebugView`.

### Application commands

`Application::registerCommands` (`engine/application_commands.cpp`) adds the engine's sets above, then `Game::addCommands(registry, world)` (optional; for actions that are not a setting: the forest's `forest-view`, `forest-climber`, `forest-epiphyte`), then:

| Command | Does |
|---|---|
| `screenshot <file.png> [width height]` | default the window's pixel size; 16 to 8192 a side; makes the folder. Draws the next frame (`describeFrame` at the frame's alpha, 1 while paused) through `VulkanRenderer::capture` for an SDR `DisplayOutput{}`, saves `final` with `saveRawImagePng`: no debug UI or lines. Settles TAA on that frame first and resets the live history (a frame or two of blur after) |
| `stats` | `renderStatsLines` (the F4 readout's lines), the camera, the CPU, scene and render profilers' lines, the GPU profiler's, the cloth's while it has timings, then `PAUSED` or `RUNNING AT TICK <n>` |
| `wait [frames]` | holds the queue for 1 to 100,000 frames; its reply goes when done, so a client knows lighting and anti-aliasing have settled |
| `sky-points <x y z>` | `VulkanRenderer::describeSkyPoints`: the sky visibility grid's queues and each cascade's eight points round the place (state, waiting or quick, open and unsure shares, bounce). A screenshot bakes everything first, so this is the only live view |
| `cull-compare`, `placement-compare`, `cluster-compare` | one helper (`addCompareCommand`): the next frame's GPU lists (`cpuReferenceLists`, `gpu-driven.md`), planting cells (`planting.md`) or mesh clusters (`mesh-clusters.md`) compared with the CPU's; returns the last report |
| `save-settings` | `Application::saveSettings`, as F5's Save |
| `quit` | ends the loop after the replies are sent |

### Where they run

`Application::runCommands`, first thing each frame:

- Its duration is added to `previousFrameStart`, so a slow screenshot or long step is not game time and causes no catch-up burst.
- Takes the server's new requests into `pendingCommands`, behind any script lines `queueCommands` added (`--run`; blank and `#` lines skipped).
- Runs them in order until empty, `quit`, or a `wait` holds the queue (`waitFrames`, the held reply in `waiting`).
- Replies go to the connection; a script's to the standard output as `> <line>` then the reply (`ERROR: ` before failures).
- Commands land between frames like key presses, so ticks stay deterministic; `step` uses `Application::runTick`, the loop's own tick.

### The socket

`platform/command_socket` (Winsock, `-lws2_32`). One command per connection: a line in, the reply until close, so replies have no length limit.

- `CommandServer(port)`: 127.0.0.1 only (unreachable from other machines), non-blocking, `SO_EXCLUSIVEADDRUSE` so a second game cannot share the port; taken port → `std::runtime_error`, which `Application` logs and runs without commands. Port 0 picks a free one (tests).
- `poll()`: accepts and reads without waiting; hands over each connection's first line (to the newline, a trailing `\r` dropped; a client closing after an unterminated line still sent it). Past `maxCommandLineBytes` (64 KB) without a newline: error reply, closed.
- `reply(id, bytes)`: blocking send with a 10 s limit, then close; a client that left is skipped.
- `sendCommand(port, line)`: the client. Connects, sends the line and a newline, reads to the close within `commandReplySeconds` (300). No listener → `std::runtime_error` naming the port.
- Start-up settings `EngineSettings::commandServer` (on) and `commandPort` (`defaultCommandPort`, 47613, below Windows' ephemeral range). The server starts after the pipelines are built, so `--send` meets a ready game. Golden captures turn it off.

### Launch options

`parseLaunchOptions` (`app/game_registry`) into `LaunchOptions`; `main` acts on them. Any order, each once except `--set`.

| Option | Does |
|---|---|
| `--set key=value` | split at the first `=`, both sides trimmed, neither empty; applied over `settings.cfg` before anything reads them, start-up fields included; a bad key or value stops the start |
| `--run <file>` | the script's lines queued for the first frame |
| `--port <port>` | 1 to 65535; the game's port (passed as `engine.commandPort`) and `--send`'s |
| `--hidden` | `WindowVisibility::Hidden` (`SDL_WINDOW_HIDDEN`): a window that never shows, for scripts and AIs; screenshots still work (they come from the finished picture). A launch option, not a setting, so a saved file can never hide a player's game. From PowerShell add `-WindowStyle Hidden` to `Start-Process`, or the console shows |
| `--typical-gpu` | `typical-gpu on` queued before the script's first line |
| `--send <words...>` | every later word is one command (`joinCommandWords`), sent to `--port`, else `settings.cfg`'s `engine.commandPort` (read with `engineSettings`), else the default; prints the reply (errors to standard error), exits 0 or 1, opens no window. Only `--port` may precede it |

- `--set`, `--run`, `--hidden` and `--typical-gpu` are refused with `--golden-save`, `--golden-check` and the cloth yardstick, which measure the defaults in their own window.
- Unknown options or games throw `std::invalid_argument` listing the valid ones.

## Tests

`settings_file_test` (every walk's defaults within limits, `findLimitProblems`), `engine_settings_test`, `command_registry_test`, `engine_commands_test`, `command_socket_test` (round trip, long replies, two clients, taken port, no listener), `game_registry_test` (launch options), `quality_preset_test`, `typical_gpu_test`, `overlay_composite_test`.
