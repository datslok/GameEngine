# A Claude Code hook (.claude/settings.json, PreToolUse on Bash and PowerShell): before a `git commit`, says which
# changed code folders have an architecture doc that did not change, so the doc is rewritten when behaviour changed.
# A reminder, never a block: most commits (fixes, tuning) change nothing a doc describes.
#
# Which doc covers what: each docs/architecture/*.md may have a line "Covers: `src/x/`, `makefile`"; anything no file
# claims belongs to docs/ARCHITECTURE.md (until phase 19's split ends). Changes are measured against HEAD with
# untracked files included, since `git add -A && git commit` stages in the same command.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$call = [Console]::In.ReadToEnd() | ConvertFrom-Json
$command = [string]$call.tool_input.command

if ($command -notmatch "\bgit\b[^\r\n]*\bcommit\b") {
    exit 0
}

$changed = @(git diff HEAD --name-only) + @(git ls-files --others --exclude-standard) | Where-Object { $_ }

# Prefix -> the doc that covers it, from each architecture file's "Covers:" line.
$owners = @{}

foreach ($doc in Get-ChildItem "docs/architecture" -Filter *.md -ErrorAction SilentlyContinue) {
    $covers = Select-String -Path $doc.FullName -Pattern "^Covers:" | Select-Object -First 1

    if ($covers) {
        foreach ($match in [regex]::Matches($covers.Line, "``([^``]+)``")) {
            $owners[$match.Groups[1].Value] = "docs/architecture/" + $doc.Name
        }
    }
}

# The doc owning a path: its longest covering prefix, else the old single reference.
function Find-Owner([string]$path) {
    $best = ""

    foreach ($prefix in $owners.Keys) {
        if ($path.StartsWith($prefix) -and $prefix.Length -gt $best.Length) {
            $best = $prefix
        }
    }

    if ($best) {
        return $owners[$best]
    }

    return "docs/ARCHITECTURE.md"
}

$missing = [ordered]@{}

foreach ($path in $changed) {
    if (-not ($path.StartsWith("src/") -or $path -eq "makefile")) {
        continue
    }

    $doc = Find-Owner $path

    if ($changed -contains $doc) {
        continue
    }

    $folder = if ($path.StartsWith("src/")) { "src/" + $path.Split("/")[1] + "/" } else { $path }

    if (-not $missing.Contains($doc)) {
        $missing[$doc] = [System.Collections.Generic.List[string]]::new()
    }

    if (-not $missing[$doc].Contains($folder)) {
        $missing[$doc].Add($folder)
    }
}

if ($missing.Count -eq 0) {
    exit 0
}

$lines = foreach ($doc in $missing.Keys) {
    ($missing[$doc] -join ", ") + " changed; " + $doc + " did not"
}

$text = "Docs reminder: " + ($lines -join ". ") + ". If behaviour changed, rewrite its section (do not append) and commit that too."

@{
    systemMessage = $text
    hookSpecificOutput = @{
        hookEventName = "PreToolUse"
        additionalContext = $text
    }
} | ConvertTo-Json -Compress -Depth 4
exit 0
