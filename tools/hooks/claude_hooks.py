"""Checks Claude Code runs around Claude's tool calls, enforcing CLAUDE.md's hard rules a script can check.

Registered in .claude/settings.json; how each works: docs/architecture/build.md ("Claude Code hooks"); tested by
tools/hooks/test_claude_hooks.py.
Usage: python -I tools/hooks/claude_hooks.py <check>, with the hook event as JSON on stdin.
  commands      PreToolUse, Bash and PowerShell: forbidden flags, code commits on main, costly runs; before a
                commit, a reminder naming facts (names, numbers, links) gone from every doc
  makefile      PreToolUse, Edit and Write: forbidden flags written into the Makefile
  line-endings  PostToolUse, Edit and Write: bare LF in the CRLF folders turned into CRLF
  games         Stop: Claude's own game copies (--port 47620) still running
Exit 2 blocks the call and shows stderr to Claude; a costly run prints a permission question instead.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Make targets and executable flags that take minutes or need the GPU: run only when the user asks (golden skill).
COSTLY_TARGET = re.compile(r"(golden|gputest|test-slow|test-parked|yardstick)")
COSTLY_FLAG = re.compile(r"--(slow|parked|golden-check|golden-save|cloth-yardstick)")

# Programs that take those flags: the engine's executables and the visible test runner.
TAKES_COSTLY_FLAGS = re.compile(r"(tests|game)\.exe$|visible_tests\.ps1$")

# A heredoc's body (the text a script feeds a program), from its << line to its closing word.
HEREDOC = re.compile(r"<<-?\s*['\"]?(\w+)['\"]?[^\n]*\n.*?\n\1\s*(\n|$)", re.DOTALL)

# Quoted text holding a space is a message or a string, not a program's name (a quoted path has no spaces).
QUOTED_PHRASE = re.compile(r"\"[^\"]*\s[^\"]*\"|'[^']*\s[^']*'")

# Where one command ends and the next starts.
COMMAND_BREAK = re.compile(r"&&|\|\||[;|\n]")

# Flags that break the tests: -DNDEBUG removes the asserts, -ffast-math changes floating-point results.
FORBIDDEN_FLAG = re.compile(r"-DNDEBUG\b|-ffast-math\b")

# A command that builds: only then is a forbidden flag a problem (a search for the flag's name is fine).
BUILDS = re.compile(r"(?<![\w-])(make|g\+\+|gcc|clang\+\+|clang|c\+\+)(?![\w-])|CXXFLAGS|CFLAGS")

GIT_COMMIT = re.compile(r"\bgit\s+(-C\s+\S+\s+)?commit\b")
GIT_MERGE = re.compile(r"\bgit\s+(-C\s+\S+\s+)?merge\b([^;&|\n]*)")

# Folders whose files are CRLF (CLAUDE.md rule 10); root files such as README.md keep their own endings.
CRLF_FOLDERS = ["src", "tests", "assets/shaders", "tools"]

# A doc's facts a script can see (CLAUDE.md, "Compact, never lose meaning"): backticked names, links, and numbers that
# say something (a decimal, two digits or more, or a unit), each with its unit. Bare one-digit numbers are left out:
# they are in every doc, so one vanishing says nothing.
BACKTICKED = re.compile(r"`([^`\n]+)`")
LINK = re.compile(r"\]\(([^)\s]+)\)|https?://[^\s)>\]]+")
NUMBER = re.compile(r"(?<![\w.])(\d+(?:\.\d+)?)\s?(%|[kKMG]?B\b|ms\b|m/s\b|Hz\b|km\b|cm\b|mm\b|m\b|s\b|px\b|degrees\b)?")

# The most vanished facts the reminder names, so a big rewrite stays readable.
MOST_VANISHED = 30

# Claude's own game copies always carry this port (drive-game skill); the user's copies never do.
CLAUDE_GAME_PORT = re.compile(r"--port\s+47620\b")


# The programs a command line runs, each as its list of words, with heredoc bodies and quoted phrases left out, so
# text that only mentions a costly run (a commit message, a grep, a script's strings) does not count as one.
def commands_run(command):
    text = QUOTED_PHRASE.sub('""', HEREDOC.sub("\n", command))
    commands = []
    for part in COMMAND_BREAK.split(text):
        words = [word.strip("\"'") for word in part.split()]
        if words:
            commands.append(words)
    return commands


# Why a command needs the user's go first, or None for a routine command.
def costly_run(command):
    for words in commands_run(command):
        program = words[0].replace("\\", "/").rsplit("/", 1)[-1]
        if program == "make":
            costly = [word for word in words[1:] if COSTLY_TARGET.match(word)]
        elif program == "gputests.exe":
            costly = [program]
        elif any(TAKES_COSTLY_FLAGS.search(word) for word in words[:3]):
            costly = [word for word in words[1:] if COSTLY_FLAG.match(word)]
        else:
            costly = []
        if costly:
            return f"'{' '.join(words)}' runs slow, GPU or golden tests: the user asks for these themselves."
    return None


# Why a command must not run, or None: a build with a flag that breaks the tests.
def forbidden_flags(command):
    flag = FORBIDDEN_FLAG.search(command)
    if flag is None or BUILDS.search(command) is None:
        return None
    return f"{flag.group(0)} is never used here (CLAUDE.md rule 3): the tests rely on assert and exact floating point."


# Why Makefile text must not be written, or None: a forbidden flag outside a comment.
def forbidden_flags_in_makefile(path, text):
    if Path(path).name.lower() != "makefile":
        return None
    for line in text.splitlines():
        flag = FORBIDDEN_FLAG.search(line.split("#", 1)[0])
        if flag is not None:
            return f"{flag.group(0)} in the Makefile is never allowed (CLAUDE.md rule 3)."
    return None


# Why a git command breaks the branch flow (CLAUDE.md rule 9), or None. Markdown-only commits on main are allowed
# (docs touch-ups), as is the commit that finishes a merge.
def main_branch_problem(command, branch, merging, changed_paths):
    if branch != "main":
        return None
    code = [path for path in changed_paths if not path.lower().endswith(".md")]
    if GIT_COMMIT.search(command) and not merging and code:
        return (f"Committing code straight onto main ({', '.join(code[:5])}): make a feature branch, commit there, "
                f"then merge with --no-ff (CLAUDE.md rule 9).")
    for merge in GIT_MERGE.finditer(command):
        options = merge.group(2)
        if "--no-ff" not in options and not re.search(r"--(abort|continue|quit)\b", options):
            return "Merges into main use --no-ff (CLAUDE.md rule 9)."
    return None


# The facts in a doc's text (BACKTICKED, LINK, NUMBER), each once, in the order first met; code blocks count too.
def doc_facts(text):
    facts = {}
    for match in BACKTICKED.finditer(text):
        facts.setdefault("`" + match.group(1) + "`", None)
    for match in LINK.finditer(text):
        facts.setdefault(match.group(1) or match.group(0).rstrip(".,;:"), None)  # a bare URL ends before its sentence's punctuation
    for match in NUMBER.finditer(BACKTICKED.sub(" ", text)):
        number, unit = match.group(1), match.group(2) or ""
        if unit or "." in number or len(number) >= 2:
            facts.setdefault(number + (" " + unit if unit else ""), None)
    return list(facts)


# Whether a doc records history (what was done, tried and planned) rather than how things are now.
def is_history(path):
    return path.startswith("docs/buildlog/") or path in ("docs/BUILDLOG.md", "ROADMAP.md")


# What vanished: (path, fact) for each fact a changed doc had before (old_docs: path to its last committed text) that
# no doc has now (new_docs: every doc's text now, by path). A fact moved to another doc, or kept in the same one, is
# not reported. History keeps only history's facts: a name the build log mentions does not stand in for its
# description dropped from an architecture file (moving one there on purpose is reported, a cheap false alarm).
def vanished_facts(old_docs, new_docs):
    kept_now = set()
    kept_in_history = set()
    for path, text in new_docs.items():
        (kept_in_history if is_history(path) else kept_now).update(doc_facts(text))
    vanished = []
    for path, text in old_docs.items():
        kept = kept_now | kept_in_history if is_history(path) else kept_now
        vanished.extend((path, fact) for fact in doc_facts(text) if fact not in kept)
    return vanished


# The reminder for what vanished, or None when nothing did.
def vanished_reminder(vanished):
    if not vanished:
        return None
    named = "; ".join(f"{path}: {fact}" for path, fact in vanished[:MOST_VANISHED])
    more = f" (and {len(vanished) - MOST_VANISHED} more)" if len(vanished) > MOST_VANISHED else ""
    return (f"Docs kept check: gone from every doc since HEAD: {named}{more}. Compact, never lose meaning (CLAUDE.md): "
            f"put back or move to its home what still holds; tell the user what was dropped and why.")


# Whether a file lives in one of the repo's CRLF folders.
def wants_crlf(path, root):
    try:
        relative = Path(path).resolve().relative_to(Path(root).resolve()).as_posix()
    except ValueError:
        return False
    return any(relative.startswith(folder + "/") for folder in CRLF_FOLDERS)


# The bytes with every bare LF made CRLF, or None when nothing changes or the file is binary.
def with_crlf(data):
    if b"\x00" in data:
        return None
    converted = re.sub(rb"(?<!\r)\n", b"\r\n", data)
    return None if converted == data else converted


# The process ids among (pid, command line) pairs that are Claude's own game copies.
def claudes_games(processes):
    return [pid for pid, line in processes if CLAUDE_GAME_PORT.search(line or "")]


# (pid, command line) of every running process with this executable name. tasklist first, since it is quick and
# almost always finds nothing; only then PowerShell, slower, for the command lines.
def processes_named(name):
    listed = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH", "/FO", "CSV"], capture_output=True,
                            text=True).stdout
    if f'"{name.lower()}"' not in listed.lower():
        return []
    query = (f"Get-CimInstance Win32_Process -Filter \"name = '{name}'\" | "
             "ForEach-Object { \"$($_.ProcessId)`t$($_.CommandLine)\" }")
    output = subprocess.run(["powershell", "-NoProfile", "-Command", query], capture_output=True, text=True).stdout
    processes = []
    for line in output.splitlines():
        pid, _, command_line = line.partition("\t")
        if pid.strip().isdigit():
            processes.append((int(pid), command_line))
    return processes


# Runs git in the repo and returns its output, or "" when git fails.
def git(*arguments):
    # UTF-8 as the docs are, not the Windows code page.
    result = subprocess.run(["git", "-C", str(REPO_ROOT), *arguments], capture_output=True, encoding="utf-8", errors="replace")
    return result.stdout.strip() if result.returncode == 0 else ""


# Every doc's text now (tracked or new, Markdown), by path from the repo's root; unreadable files left out.
def docs_now():
    docs = {}
    for path in git("ls-files", "--cached", "--others", "--exclude-standard", "*.md").splitlines():
        try:
            docs[path] = (REPO_ROOT / path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            pass
    return docs


# The changed docs' last committed text, by path (none on a first commit).
def docs_at_head():
    docs = {}
    for path in git("diff", "HEAD", "--name-only", "--", "*.md").splitlines():
        text = git("show", f"HEAD:{path}")
        if text:
            docs[path] = text
    return docs


# The commands check: blocks forbidden flags and code commits on main, asks before costly runs; before a commit,
# reminds of facts gone from every doc (never blocks: a number retuned on purpose vanishes too).
def check_commands(event):
    command = event.get("tool_input", {}).get("command", "")
    problem = forbidden_flags(command)
    if problem is None and (GIT_COMMIT.search(command) or GIT_MERGE.search(command)):
        branch = git("rev-parse", "--abbrev-ref", "HEAD")
        merging = git("rev-parse", "-q", "--verify", "MERGE_HEAD") != ""
        changed = [line[3:].split(" -> ")[-1].strip('"') for line in git("status", "--porcelain").splitlines()]
        problem = main_branch_problem(command, branch, merging, changed)
    if problem is not None:
        block(problem)
    question = costly_run(command)
    if question is not None:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "ask",
                                                 "permissionDecisionReason": question}}))
    elif GIT_COMMIT.search(command):
        reminder = vanished_reminder(vanished_facts(docs_at_head(), docs_now()))
        if reminder is not None:
            print(json.dumps({"systemMessage": reminder,
                              "hookSpecificOutput": {"hookEventName": "PreToolUse", "additionalContext": reminder}}))


# The makefile check: the text an Edit or Write would put in the Makefile.
def check_makefile(event):
    tool_input = event.get("tool_input", {})
    text = tool_input.get("new_string") or tool_input.get("content") or ""
    problem = forbidden_flags_in_makefile(tool_input.get("file_path", ""), text)
    if problem is not None:
        block(problem)


# The line-endings check: rewrites a just-written file in a CRLF folder with CRLF endings.
def check_line_endings(event):
    path = Path(event.get("tool_input", {}).get("file_path", ""))
    root = Path(os.environ.get("CLAUDE_PROJECT_DIR") or REPO_ROOT)  # not the event's cwd: the shell's folder after a cd
    if not path.is_file() or not wants_crlf(path, root):
        return
    converted = with_crlf(path.read_bytes())
    if converted is not None:
        path.write_bytes(converted)


# The games check: keeps the turn going while one of Claude's game copies runs.
def check_games(event):
    pids = claudes_games(processes_named("game.exe"))
    if pids:
        block(f"Your game copies are still running (process {', '.join(map(str, pids))}). Quit each with "
              f"'game.exe --port 47620 --send quit', Stop-Process any left, check Get-Process game (CLAUDE.md rule 1).")


# Blocks the call: Claude Code shows stderr to Claude when the exit code is 2.
def block(reason):
    print(reason, file=sys.stderr)
    sys.exit(2)


CHECKS = {"commands": check_commands, "makefile": check_makefile, "line-endings": check_line_endings,
          "games": check_games}

if __name__ == "__main__":
    # utf-8-sig: a PowerShell pipe puts a byte-order mark first.
    CHECKS[sys.argv[1]](json.loads(sys.stdin.buffer.read().decode("utf-8-sig") or "{}"))
