"""Tests of tools/hooks/claude_hooks.py, the checks Claude Code runs around Claude's tool calls.

Run from the repo root: python -I tools/hooks/test_claude_hooks.py (docs/architecture/build.md, Claude Code hooks)
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HOOKS_FOLDER = Path(__file__).resolve().parent
sys.path.insert(0, str(HOOKS_FOLDER))

import claude_hooks  # noqa: E402

SCRIPT = HOOKS_FOLDER / "claude_hooks.py"


# Runs the hook script as Claude Code does: the check's name as argument, the event as JSON on stdin.
def run_hook(check, event):
    return subprocess.run([sys.executable, "-I", str(SCRIPT), check], input=json.dumps(event), capture_output=True,
                          text=True, timeout=60)


class CostlyRuns(unittest.TestCase):
    # Every make target and executable flag the user wants asked about first.
    def test_costly_commands_are_named(self):
        for command in ["make golden", "make golden-save GAME=forest", "make gputest-release", "make test-slow",
                        "make test-parked-release", "make yardstick-quick",
                        '$env:PATH = "C:\\msys64\\ucrt64\\bin;" + $env:PATH; make gputest',
                        "build/release/tests.exe --slow", "build/tests.exe --parked", "build/game.exe --golden-check",
                        "build/release/game.exe --cloth-yardstick-dress", "build/release/gputests.exe",
                        "powershell -File tools/visible_tests.ps1 -Arguments --slow",
                        '& "build/release/tests.exe" --slow', "cd build && make golden", "make release; make gputest"]:
            self.assertIsNotNone(claude_hooks.costly_run(command), command)

    # Text that only quotes the costly words runs nothing slow: messages, greps, scripts' strings.
    def test_quoted_words_pass(self):
        for command in ['git commit -m "make test-slow is slow now"', 'git commit -m "x; make golden later"',
                        'grep -n "tests.exe --slow" Makefile',
                        "python -I - <<'PY'\nold = \"| `make test-slow`, `golden*` |\"\nmake golden\nPY\necho done"]:
            self.assertIsNone(claude_hooks.costly_run(command), command)

    # The routine runs pass without a question.
    def test_routine_commands_pass(self):
        for command in ["make", "make release", "make test-release", "make run-release GAME=forest",
                        "powershell -File tools/visible_tests.ps1", "build/release/tests.exe",
                        "git log --grep golden", "grep -n gputest Makefile"]:
            self.assertIsNone(claude_hooks.costly_run(command), command)


class ForbiddenFlags(unittest.TestCase):
    # -DNDEBUG removes the asserts the tests rely on; -ffast-math breaks NaN checks and changes results.
    def test_flags_in_builds_are_blocked(self):
        for command in ['make CXXFLAGS="-O2 -DNDEBUG"', "g++ -ffast-math -c a.cpp", "make EXTRA_FLAGS=-ffast-math",
                        "clang++ -DNDEBUG x.cpp"]:
            self.assertIsNotNone(claude_hooks.forbidden_flags(command), command)

    # Searching for the flags' names is not building with them.
    def test_searches_pass(self):
        for command in ['grep -n -E "NDEBUG|fast-math" Makefile', "make release", 'git log --grep "-ffast-math"']:
            self.assertIsNone(claude_hooks.forbidden_flags(command), command)

    # Makefile edits: the flags in a recipe are blocked, in a comment they are not.
    def test_makefile_text(self):
        self.assertIsNotNone(claude_hooks.forbidden_flags_in_makefile("E:/r/Makefile", "CXXFLAGS += -ffast-math\n"))
        self.assertIsNone(claude_hooks.forbidden_flags_in_makefile(
            "E:/r/Makefile", "# Never -DNDEBUG or -ffast-math (they break the tests).\n"))
        self.assertIsNone(claude_hooks.forbidden_flags_in_makefile("E:/r/src/a.cpp", "// -ffast-math\n"))


class MainBranch(unittest.TestCase):
    # Code changes committed straight onto main skip the branch flow (CLAUDE.md rule 9).
    def test_code_commit_on_main_is_blocked(self):
        self.assertIsNotNone(claude_hooks.main_branch_problem('git commit -m "x"', "main", False,
                                                             ["src/a.cpp", "ROADMAP.md"]))

    # Markdown-only commits on main are the user's practice for docs touch-ups.
    def test_docs_commit_on_main_passes(self):
        self.assertIsNone(claude_hooks.main_branch_problem('git commit -m "x"', "main", False,
                                                          ["ROADMAP.md", "docs/a.md"]))

    # Finishing a merge (after resolving conflicts) commits on main legitimately.
    def test_commit_finishing_a_merge_passes(self):
        self.assertIsNone(claude_hooks.main_branch_problem("git commit --no-edit", "main", True, ["src/a.cpp"]))

    # Feature branches commit freely.
    def test_commit_on_feature_branch_passes(self):
        self.assertIsNone(claude_hooks.main_branch_problem('git commit -m "x"', "claude-hooks", False, ["src/a.cpp"]))

    # Merges into main keep the branch visible in history.
    def test_merge_without_no_ff_is_blocked(self):
        self.assertIsNotNone(claude_hooks.main_branch_problem("git merge claude-hooks", "main", False, []))
        self.assertIsNone(claude_hooks.main_branch_problem("git merge --no-ff claude-hooks -F msg.txt", "main",
                                                          False, []))
        self.assertIsNone(claude_hooks.main_branch_problem("git merge --abort", "main", True, []))
        self.assertIsNone(claude_hooks.main_branch_problem("git merge main", "claude-hooks", False, []))


class LineEndings(unittest.TestCase):
    ROOT = Path("E:/Code/GameEngine")

    # Source folders are CRLF; README.md and other root files keep what they have.
    def test_folders(self):
        for path in ["src/render/a.cpp", "tests/a_test.cpp", "assets/shaders/a.glsl", "tools/hooks/a.py"]:
            self.assertTrue(claude_hooks.wants_crlf(self.ROOT / path, self.ROOT), path)
        for path in ["README.md", "ROADMAP.md", "assets/models/a.obj", "docs/a.md"]:
            self.assertFalse(claude_hooks.wants_crlf(self.ROOT / path, self.ROOT), path)
        self.assertFalse(claude_hooks.wants_crlf(Path("C:/elsewhere/src/a.cpp"), self.ROOT))

    # Bare LF becomes CRLF; existing CRLF is left alone; binary files are not touched.
    def test_conversion(self):
        self.assertEqual(claude_hooks.with_crlf(b"a\nb\r\nc\n"), b"a\r\nb\r\nc\r\n")
        self.assertIsNone(claude_hooks.with_crlf(b"a\r\nb\r\n"))
        self.assertIsNone(claude_hooks.with_crlf(b"\x00\x01\n"))


class LeftoverGames(unittest.TestCase):
    # Claude's copies carry --port 47620; the user's never do.
    def test_only_claudes_copies_count(self):
        processes = [(10, '"build/release/game.exe" --game forest --hidden --port 47620'),
                     (11, "build/release/game.exe --game forest"),
                     (12, "build\\game.exe --port 47613")]
        self.assertEqual(claude_hooks.claudes_games(processes), [10])


class EndToEnd(unittest.TestCase):
    # A blocked call exits 2 with the reason on stderr; an allowed one exits 0 silently.
    def test_commands_hook(self):
        blocked = run_hook("commands", {"tool_name": "PowerShell", "tool_input": {"command": "make CXXFLAGS=-ffast-math"}})
        self.assertEqual(blocked.returncode, 2)
        self.assertIn("fast-math", blocked.stderr)
        allowed = run_hook("commands", {"tool_name": "Bash", "tool_input": {"command": "make release"}})
        self.assertEqual((allowed.returncode, allowed.stdout), (0, ""))

    # PowerShell pipes put a byte-order mark before the JSON; a hook that crashes would let the call through.
    def test_byte_order_mark_is_read(self):
        event = json.dumps({"tool_name": "Bash", "tool_input": {"command": "g++ -ffast-math a.cpp"}})
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), "commands"], input="﻿" + event,
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode, 2, result.stderr)

    # A costly run asks the user through the permission JSON.
    def test_commands_hook_asks(self):
        asked = run_hook("commands", {"tool_name": "Bash", "tool_input": {"command": "make golden"}})
        self.assertEqual(asked.returncode, 0)
        decision = json.loads(asked.stdout)["hookSpecificOutput"]
        self.assertEqual((decision["hookEventName"], decision["permissionDecision"]), ("PreToolUse", "ask"))

    # The line-ending hook rewrites a written file in a CRLF folder.
    def test_line_endings_hook(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "src" / "a.cpp"
            source.parent.mkdir()
            source.write_bytes(b"int a;\nint b;\n")
            result = run_hook("line-endings", {"tool_name": "Write", "cwd": folder,
                                               "tool_input": {"file_path": str(source)}})
            self.assertEqual(result.returncode, 0)
            self.assertEqual(source.read_bytes(), b"int a;\r\nint b;\r\n")

    # The process listing finds a live process by its command line (a stand-in named like the check asks).
    def test_process_listing(self):
        sleeper = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)", "--port", "47620"])
        try:
            found = claude_hooks.processes_named(Path(sys.executable).name)
            self.assertIn(sleeper.pid, [pid for pid, line in found if "--port 47620" in line])
        finally:
            sleeper.kill()
            sleeper.wait()


if __name__ == "__main__":
    unittest.main(verbosity=1)
