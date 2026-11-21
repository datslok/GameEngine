# games/forest/

The jungle valley showcase (`--game forest`, phase 18). Plan and status: `ROADMAP.md`. Design: `docs/architecture/forest.md` and the forest files it links.

- Read before building: `docs/research/JUNGLE.md`, `docs/research/TREES.md`, `docs/research/VINES.md`, `docs/research/WIND.md`, `docs/research/LOD.md`, `docs/research/GROUNDCOVER.md`.
- Judge looks and cost with the typical GPU setup on (`--typical-gpu`, `typical-gpu`); label measurements as the laptop's RTX 4070.
- Random numbers: `forestUnitRandom` and `forestShapeSeed` (`forest_terrain`) only, so the valley is the same every run.
- Scanned assets: `python tools/fetch_forest_assets.py` into `assets/forest`; without them the ground uses plain stand-ins.
- Commands: `forest-view`, `forest-climber [n]`, `forest-epiphyte [n]`, `forest-twigs [off]`; settings under F5 Forest.
- Tests build the valley with `TestForest` (`tests/forest_helpers.h`) on the shared `testJobs`.
