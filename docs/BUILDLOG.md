# Build log

The history of the finished roadmap items: when each was built, why, what was measured at the time, and what was tried and dropped.

It is kept apart from the plan so `ROADMAP.md` stays a short list of what is done and what comes next.

- `ROADMAP.md`: the plan; finished items there are one line each.
- `docs/architecture/`: how the code works today. Facts about the current code belong there, not here.
- This file: decisions, reasons, measurements and dead ends, in the order they happened, grouped by roadmap phase.
  - Numbers are as measured then, so later work may have changed them.
  - Branch names are given so each step can be found in `git log` (`git log --merges` lists them with dates).

When a roadmap item is ticked off, cut it to one line there and add its story to the current phase's last part, in the same branch.

---

## Parts

Split into files of about 28 KB at entry boundaries, the text unchanged. New entries go at the end of the current phase's last part; start a new part past 30 KB.

| Part | Covers |
|---|---|
| `docs/buildlog/phase-00-05.md` | Phases 0-5 |
| `docs/buildlog/phase-06-1.md` | Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 1 of 5) |
| `docs/buildlog/phase-06-2.md` | Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 2 of 5) |
| `docs/buildlog/phase-06-3.md` | Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 3 of 5) |
| `docs/buildlog/phase-06-4.md` | Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 4 of 5) |
| `docs/buildlog/phase-06-5.md` | Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026) (part 5 of 5) |
| `docs/buildlog/phase-07-1.md` | Phase 7: Cloth simulation (Oct 24 2026 onwards) (part 1 of 3) |
| `docs/buildlog/phase-07-2.md` | Phase 7: Cloth simulation (Oct 24 2026 onwards) (part 2 of 3) |
| `docs/buildlog/phase-07-3.md` | Phase 7: Cloth simulation (Oct 24 2026 onwards) (part 3 of 3) |
| `docs/buildlog/phase-16-1.md` | Phase 16: Beautiful without ray tracing (Nov 6-13 2026) (part 1 of 3) |
| `docs/buildlog/phase-16-2.md` | Phase 16: Beautiful without ray tracing (Nov 6-13 2026) (part 2 of 3) |
| `docs/buildlog/phase-16-3.md` | Phase 16: Beautiful without ray tracing (Nov 6-13 2026) (part 3 of 3) |
| `docs/buildlog/phase-18-1.md` | Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 1 of 4) |
| `docs/buildlog/phase-18-2.md` | Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 2 of 4) |
| `docs/buildlog/phase-18-3.md` | Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 3 of 4) |
| `docs/buildlog/phase-18-4.md` | Phase 18: A jungle valley showcase (Nov 13 2026 onwards) (part 4 of 4) |
| `docs/buildlog/phase-19.md` | Phase 19: Documentation refactor (Nov 17 2026 onwards) |
| `docs/buildlog/phase-21-1.md` | Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards) (part 1 of 3) |
| `docs/buildlog/phase-21-2.md` | Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards) (part 2 of 3) |
| `docs/buildlog/phase-21-3.md` | Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards) (part 3 of 3) |
| `docs/buildlog/tools-1.md` | Tools outside the phases (part 1 of 3) |
| `docs/buildlog/tools-2.md` | Tools outside the phases (part 2 of 3) |
