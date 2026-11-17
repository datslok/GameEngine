---
name: add-test
description: Use when writing a new CPU test file in tests/ (every feature gets its tests first), to create, register and run it correctly.
---

# Add a test

Folder rules and helpers: `tests/CLAUDE.md`.

1. Create `tests/<thing>_test.cpp` with one `void test<Thing>()` running plain `assert`s. The makefile picks it up.
2. Reuse helpers from `test_levels.h`, `demo_helpers.h`, `test_jobs.h`, `forest_helpers.h`, `cloth_helpers.h`; add new shared ones there, never copy between test files.
3. Any game the test builds gets `game.useJobs(testJobs())`.
4. Register in `tests/test_main.cpp`: a forward declaration and an entry in `routineTests` (or `runSlowTests` if it takes many seconds and guards something rarely changed; `runParkedTests` for light probes and cloth).
5. A command a feature adds is tested through `CommandRegistry::execute`.
6. Write the test before the code. Run the suite (`powershell -File tools/visible_tests.ps1`) only before committing, not while iterating.
