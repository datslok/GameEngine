---
name: golden
description: Use only when the user asks to run the golden image tests or the GPU tests, or to save new golden references.
---

# Golden images and GPU tests

Never run these on your own: they take long and the user checks the pictures by hand.

| Command | Does |
|---|---|
| `make golden` | Renders every game's golden views, compares with `tests/golden/<GPU>/<game>/`; captures and difference heatmaps in `build/golden/<GPU>/<game>/` |
| `make golden-save [GAME=<name>]` | Makes the captures the references (one game with `GAME=`) |
| `make gputest` / `make gputest-release` | Every shader library against its C++ mirror (worst share of tolerance per group; any validation error fails), then the golden images |

- References are per GPU (the driver's name). Only `NVIDIA-GeForce-RTX-5080` exists, and it lacks the cloth lab's and the forest's views (a missing reference fails); on another GPU (the laptop's RTX 4070) there is nothing to compare against until the user saves references there.
- The same GPU, driver and shaders reproduce the references bit for bit: any difference is a real change.
- Saving references for a change meant to alter the picture: say so in the commit message.
- Before a capture: 960 frames when light probes are traced (ray tracing on), 32 to settle anti-aliasing (`docs/architecture/testing.md`).
- In a hand-off, say that golden images and GPU tests were not run or saved, so the user can ask.
