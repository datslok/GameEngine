# Seeing and steering the engine

The engine's tools for developers: debug drawing and the F4 readout, the F5 settings window, and text commands that let scripts and AIs drive a running game.

## Debug drawing

Much of what the engine knows is invisible: bounding boxes, colliders, where a character means to walk. **Debug drawing** shows it as wireframe lines over the scene.

Anything can add shapes to a `DebugDraw` (lines, boxes, spheres drawn as three circles, capsules, any collision shape), and it is emptied every frame, so a shape lasts only while something keeps adding it. That style is **immediate mode**: instead of creating a "debug box object" and remembering to delete it, you say "draw a box here" on every frame you want it.

The lines are drawn last, after tone mapping, unlit and with no depth test, so they show through walls. That is the point: a collider buried in a wall or a light behind a hill is exactly what you want to see while debugging.

Games add their own shapes in `onDebugDraw`. The engine's debug view (F4, or the `debug-view` command) boxes every mesh the camera draws, outlines every collider, and prints a readout: draws and how many culling skipped, triangles, shadow tiles, lights, video memory, and the CPU and GPU timings of the previous chapters (`gpu.md`). The same lines come back as text from the `stats` command.

The engine has no font yet, so the readout is drawn with lines too, in a **stroke font**: each character is a few straight strokes on a 4 by 6 grid, like a plotter or an old vector arcade game. These lines are placed in window pixels rather than the world, through a flat projection with (0, 0) at the top-left corner. Details: `docs/architecture/game-interface.md`.

## The F5 window: one description, many uses

Every number someone might tune (bloom strength, exposure, how many lights get shadows, mouse sensitivity) lives in a settings struct next to the system that reads it. F5 opens a window with a slider, checkbox or list for each, and a change shows on the next frame.

No setting has UI code of its own. Each settings struct has one `visit()` function listing its fields once, the way a physicist writes down a quantity: a name, the value, the range that makes physical sense and the unit. For example, "bloom strength, from 0 to 1, useful up to 0.2".

A **visitor** is anything that walks that list and does one job with each entry:

| Visitor | Job |
|---|---|
| UI | a slider per number, a checkbox per switch, a list per choice |
| saving and loading | each value to text and back, for `settings.cfg` |
| commands | `set`, `get`, `list`, `reset` by key (below) |
| checking | the tests confirm every default lies inside its stated range |

So the description is written once and every use is derived from it: one line in `visit()` gives a field its slider, its place in the settings file, its command and its test. This is the **visitor pattern**: the walk over the data is written once, and new operations are new visitors.

Each number carries two ranges:

- **hard limits**, what is meaningful at all (a share of light cannot exceed 1); hand-edited files and commands are clamped to them;
- the **slider range**, the useful part, often much narrower, so the slider is not fiddly.

Some settings are read only at start-up (the simulation rate, GPU validation); the window shows them greyed out rather than pretend a change took effect. Settings that need hardware ray tracing are greyed on GPUs without it.

**Save** writes only the values that differ from the defaults to `settings.cfg`, which the engine loads at every start before anything reads a setting. Saving only the differences matters: if a default is improved later, everyone who never touched that setting gets the improvement, and deleting a line returns it to its default. Each game keeps its own settings in its own section of the file.

**Quality presets** set the costly settings together:

| Preset | In short |
|---|---|
| **Ultra** (default) | the engine's defaults, including ray-traced reflections and mirrored lights while ray tracing is on |
| **High** | Ultra without those two ray-traced extras, and a little cheaper ambient occlusion and reflection probe |
| **Medium** | half-resolution sun shadows, fewer shadowed lights, a coarser fog grid |
| **Low** | the fewest shadows; ambient occlusion, reflections, contact shadows and fog off |

Taste (exposure, tone mapping, bloom strength, the camera's finish, the upscaler) is never touched by a preset, and neither is the ray tracing master switch. A **typical GPU** checkbox shows the engine as a mid-range PC draws it: High, no ray tracing, and the best available upscaler at its Quality mode. The full table: `docs/architecture/settings-and-commands.md`.

**Dear ImGui.** The window comes from Dear ImGui, a widely used tools library. It is immediate mode, like debug drawing: every frame the code says "a slider for this value here" and ImGui reports whether it moved. There are no UI objects to create, keep in sync or delete, which is why one visitor can draw the whole window.

**Drawn as an overlay.** The UI is drawn into an image of its own and laid over the finished frame last, for two reasons:

- **HDR.** On an HDR screen the frame is in scRGB, where 1 means a dim 80 nits; ImGui's colours drawn there directly would come out dim. As a separate layer, its colours are decoded from sRGB and scaled to the screen's white level, so the window looks as it would on the desktop.
- **Premultiplied alpha.** Drawn onto a transparent image, each pixel ends up holding colour times coverage, plus the coverage itself. The final blend is then one formula, `ui + screen * (1 - coverage)`, like a sheet of tinted glass passing the share of light it does not block. That blend is mirrored on the CPU and tested, as the other shader maths is.

## Commands: driving the engine from outside

Everything F5 can do, a program can do. A running game listens for short text commands, so a script, a tool or an AI can change it while you watch the window:

```sh
build/game.exe --send set engine.render.bloom.strength 0.1
build/game.exe --send screenshot build/shots/bloom.png
```

**One description pays again.** `set`, `get`, `list` and `reset` are one more visitor over the same `visit()` lists, so every setting is reachable by its key in `settings.cfg`, with its limits enforced, and a setting added later needs nothing more. A mistyped key gets the nearest real keys back.

**Commands, not direct changes.** Each command is a named action with arguments, kept in a registry with a sentence saying what it does (`help` lists them). This is the **command pattern**, the same idea as the input commands that reach the simulation. Beyond settings there are `pause`, `resume`, `step`, `camera`, `screenshot`, `stats`, `wait`, `preset`, `quit`, and games add their own (`Game::addCommands`). It will matter more for the planned level editor: if each button only issues a command, a script can do anything the editor can, and undo is a list of commands played backwards.

**Between frames, like key presses.** Commands arrive whenever they like, but the engine looks at them only at the top of a frame. A tick never sees half a command, and `step 120` runs exactly the 120 ticks playing would have, so the simulation stays deterministic. Time a command takes (a slow screenshot, a long step) is left out of the clock, so the game does not rush to catch up afterwards. `wait` holds its reply for a number of frames, so a script knows when lighting and anti-aliasing have settled.

**A socket on your own computer.** A socket is a numbered mailbox the operating system keeps for a program, its number the **port** (47613 by default). The game listens only on 127.0.0.1, the address meaning "this computer", so nothing on the network can reach it. `--send` opens a connection, writes one line and reads the reply until the game hangs up. Each `--send` is a separate run of the program, so an AI whose shell commands each start afresh can still hold a conversation with the game.

Two launch options round it off: `--run <script>` plays a file of commands at start-up, and `--hidden` with `--port` starts a copy whose window never shows, for scripts that should not disturb the screen.
