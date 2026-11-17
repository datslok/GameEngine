---
name: docs-review
description: Use at the end of a roadmap phase, after a large feature, or when the user asks to review the docs.
---

# Docs review

Homes and budgets: root `CLAUDE.md`, "Where new information goes".

1. **Placement:** read every `CLAUDE.md`, `.claude/rules/*`, skill and `docs/INDEX.md`. Move each item to its narrowest home; merge duplicates into one with links to it.
2. **Truth:** check each rule and named file or function against the code; fix or delete what is stale (deletions listed for the user first). Start from `build/release/tests.exe --docs-report`: a date or measurement moves to the build log, a missing name is fixed; a setting's value (`1 ms` of baking) stays.
3. **Roadmap:** cut the finished phase to its milestone and its branch names; mark the current phase in `ROADMAP.md` and the root `CLAUDE.md` overview.
4. **Architecture:** each file describes the code as it is now; history and measurements belong in `docs/BUILDLOG.md`. Split any file over its budget into narrower topics.
5. **The guide (`docs/ENGINE_GUIDE.md` and its chapters in `docs/guide/`):** update the chapters the phase touched, add one for a new system (prose for a learner with a physics background); update the index's Covers line.
6. **`README.md`:** features, controls and layout current; within its budget.
7. **Memory:** project rules out of memory (they live in `CLAUDE.md`, rules or skills); duplicates merged; stale entries deleted.
8. Run the suite (`powershell -File tools/visible_tests.ps1`): the docs test checks budgets, paths, duplicates and the index.
