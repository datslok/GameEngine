---
name: drive-game
description: Use when you need to look at or control the running game (screenshots, camera, settings, stats), either the user's copy or a hidden copy of your own.
---

# Drive the game by commands

Commands reach a running game over 127.0.0.1 (port 47613 by default): every setting by its `settings.cfg` key (`set`, `get`, `list`, `reset`), `preset`, `typical-gpu`, `pause`/`resume`/`step`, `camera [x y z [tx ty tz]]`, `screenshot <file.png> [width height]`, `stats`, `wait [frames]`, `quit`, a game's own (`help` lists them).

## The user's copy

Only when asked: `build/release/game.exe --send <command...>`. Never quit it.

## A hidden copy of your own

From PowerShell, in one step from start to cleanup:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:PATH
Start-Process build/release/game.exe -ArgumentList "--game forest --hidden --port 47620" -WindowStyle Hidden
build/release/game.exe --port 47620 --send wait 120
build/release/game.exe --port 47620 --send camera 0 2 10 0 2 0
build/release/game.exe --port 47620 --send screenshot build/look.png
build/release/game.exe --port 47620 --send quit
Get-Process game -ErrorAction SilentlyContinue
```

- **IMPORTANT:** the same step ends with `quit` and a `Get-Process game` check; `Stop-Process` any of yours left. A leftover copy locks `game.exe` and breaks the user's build. Never end a turn, ask a question or hand over with one running.
- Yours always carry `--hidden --port 47620`; never touch any other. To tell copies apart: `Get-CimInstance Win32_Process -Filter "name = 'game.exe'"` shows each command line.
- Then read the PNG.
- Launch options: `--game <name>`, `--set key=value`, `--run <script>`, `--typical-gpu`.
