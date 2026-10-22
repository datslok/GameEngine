"""
Package the demo into a zip that runs on a Windows PC without MSYS2.

Usage (from anywhere):
    python tools/package_demo.py               build the optimised release with make, then package
    python tools/package_demo.py --no-build    package the existing build/release/game.exe
    python tools/package_demo.py --anonymous   for sharing anonymously: nothing that matches the public repo

Output: build/GameEngineDemo.zip, staged in build/package/GameEngineDemo/ (build/Demo.zip with --anonymous).

Only the Python standard library is used. It needs MSYS2's UCRT64 tools (make, strip, objdump),
found in C:/msys64 by default; pass --msys-root or set MSYS2_ROOT if MSYS2 lives elsewhere.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import textwrap
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Shader sources are compiled to .spv at build time; the game only loads the .spv files.
SKIPPED_ASSET_SUFFIXES = {".vert", ".frag", ".glsl"}

# SDL3_image does not link its image decoders. It loads them with LoadLibrary the first time an image
# of that type is opened, so they never appear in an import table. Each image format maps to the
# start of the decoder DLL names SDL3_image looks for.
DECODER_PREFIXES = {
    "png": ["libpng"],
    "jpeg": ["libjpeg"],
    "webp": ["libwebp"],
    "tiff": ["libtiff"],
    "avif": ["libavif"],
    "jxl": ["libjxl"],
}

IMAGE_EXTENSIONS = {
    ".png": "png",
    ".jpg": "jpeg",
    ".jpeg": "jpeg",
    ".webp": "webp",
    ".tif": "tiff",
    ".tiff": "tiff",
    ".avif": "avif",
    ".jxl": "jxl",
}

README_TEXT = """{title}
{underline}

How to run
----------
1. Extract the whole zip into a folder (do not run it from inside the zip).
2. Double-click {exe_name} for the light lab: a sunlit test ground whose sun
   moves with the time of day (F5, Light lab, Sun).
3. Or double-click {demo} for the demo.

Keep the exe next to its DLLs and the assets folder. A console window opens
alongside the game; if something goes wrong, the error is shown there.

Windows may warn that the app is from an unknown publisher, because it is not
code-signed. Click "More info", then "Run anyway".

Requirements: Windows 10 or 11 (64-bit) and a graphics card with Vulkan
support (any reasonably recent NVIDIA, AMD or Intel GPU with up-to-date drivers).

Controls
--------
{controls}"""

# Starts the demo (app/game_registry.h) from the packaged exe, which starts the light lab by itself.
DEMO_LAUNCHER = "Demo.bat"

# The zip's README is plain text read in Notepad, so lines are wrapped to this width.
README_WIDTH = 78

# For --anonymous: the zip's own name and title, neither of which is the repo's name.
ANONYMOUS_NAME = "Demo"

# For --anonymous: every file in the zip gets this date (the earliest a zip can store), so the zip does not say when
# it was made.
ANONYMOUS_ZIP_DATE = (1980, 1, 1, 0, 0, 0)


def clear_build_time(exe: Path) -> None:
    """
    For --anonymous: zero the time the linker stamped into the exe's header, so the exe does not say when it was
    built. Windows does not use the stamp to run the program. Its place: the 4-byte offset at 0x3C points to the
    "PE\\0\\0" signature, which is followed by the machine type (2 bytes), the section count (2) and then the stamp (4).
    """
    data = bytearray(exe.read_bytes())
    header = int.from_bytes(data[0x3C:0x40], "little")
    if data[header:header + 4] != b"PE\0\0":
        raise PackagingError(f"{exe} does not look like a Windows exe")
    data[header + 8:header + 12] = bytes(4)
    exe.write_bytes(data)


def brief_action(action: str) -> str:
    """
    The start of a control's description, up to its first explanation, for --anonymous: "Toggle the four floodlights
    over the obstacle course (on at the start): shadow-casting..." becomes "Toggle the four floodlights over the
    obstacle course". Short generic phrases are hard to search for; the full sentences match the public README.
    """
    return re.split(r" \(|: |; |\. ", action, maxsplit=1)[0].strip()


class PackagingError(Exception):
    pass


def readme_controls(readme: str, brief: bool = False) -> str:
    """
    The controls, taken from the "## Controls" section of the repo's README.md, so the zip's README can
    never fall behind it. Markdown becomes plain text: **bold** lines become headings, and each table
    becomes two columns with long actions wrapped under themselves. brief (for --anonymous) keeps only
    the headings, the keys and the start of each action (brief_action), and leaves out the paragraphs.
    """
    match = re.search(r"^## Controls\n(.*?)(?=^## |\Z)", readme, re.MULTILINE | re.DOTALL)
    if not match:
        raise PackagingError('README.md has no "## Controls" section to copy into the zip')

    output = []
    table = []
    headed = False

    def flush_table():
        if not table:
            return
        key_width = max(len(key) for key, _ in table) + 2
        for key, action in table:
            output.append(textwrap.fill(
                action,
                width=README_WIDTH,
                initial_indent="  " + key.ljust(key_width),
                subsequent_indent=" " * (key_width + 2),
            ))
        table.clear()

    # brief keeps only tables, so a heading waits until its table comes: one over notes alone is left out.
    waiting_heading = None

    for line in match.group(1).strip().split("\n"):
        line = line.strip()
        if line.startswith("|"):
            cells = [cell.strip().replace("**", "") for cell in line.strip("|").split("|")]
            # Skip the header row and the |---|---| line under it.
            if cells[0] == "Input" or set(cells[0]) <= {"-"}:
                continue
            if waiting_heading:
                output.extend(["", waiting_heading] if output else [waiting_heading])
                waiting_heading = None
            table.append((cells[0], brief_action(cells[1]) if brief else cells[1]))
            continue

        flush_table()
        is_heading = line.startswith("**") and line.endswith("**")
        if is_heading and brief:
            waiting_heading = line.strip("*") + ":"
        elif is_heading:
            output.append(line.strip("*") + ":")
        elif brief:
            continue
        elif line.startswith("- "):
            # A list item wraps under its own text, not under the dash.
            output.append(textwrap.fill(line.replace("**", ""), width=README_WIDTH, subsequent_indent="  "))
        elif line:
            output.append(textwrap.fill(line.replace("**", ""), width=README_WIDTH))
        elif not headed:
            # A heading sits straight on top of its table, without Markdown's blank line between.
            output.append("")
        headed = is_heading

    flush_table()
    return "\n".join(output).strip("\n") + "\n"


def msys_environment(msys_root: Path) -> dict:
    """Put the MSYS2 compiler and tools first on PATH, the same way the README describes for building."""
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join([
        str(msys_root / "ucrt64" / "bin"),
        str(msys_root / "usr" / "bin"),
        environment.get("PATH", ""),
    ])
    return environment


def run(command: list, environment: dict, cwd: Path = REPO_ROOT) -> str:
    result = subprocess.run(command, cwd=cwd, env=environment, capture_output=True, text=True)
    if result.returncode != 0:
        raise PackagingError(f"{' '.join(map(str, command))} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def imported_dlls(binary: Path, objdump: Path, environment: dict) -> list:
    """Read a binary's import table: the DLLs Windows must find before the program can start."""
    output = run([str(objdump), "-p", str(binary)], environment)
    return re.findall(r"DLL Name:\s*(\S+)", output)


def is_windows_dll(name: str) -> bool:
    """DLLs that ship with Windows are on every PC, so they are never packaged."""
    lowered = name.lower()
    if lowered.startswith(("api-ms-win-", "ext-ms-win-")):
        return True
    system32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    return (system32 / name).exists()


def collect_dlls(start: list, msys_bin: Path, objdump: Path, environment: dict) -> dict:
    """
    Follow import tables recursively from the given binaries, keeping every DLL that comes from MSYS2.
    Returns {dll name: path in MSYS2}. Names are compared case-insensitively, like Windows does.
    """
    found = {}
    pending = list(start)
    visited = set()

    while pending:
        binary = pending.pop()
        if binary.name.lower() in visited:
            continue
        visited.add(binary.name.lower())

        for name in imported_dlls(binary, objdump, environment):
            candidate = msys_bin / name
            if name.lower() in found or not candidate.exists():
                continue
            found[name.lower()] = candidate
            pending.append(candidate)

    return found


def image_formats_used(assets: Path) -> set:
    """
    Which image formats the packaged assets contain: image files by extension, plus images embedded in
    or referenced by glTF files (found by their MIME type or file extension in the file's bytes).
    """
    formats = set()

    for path in assets.rglob("*"):
        if not path.is_file():
            continue

        suffix = path.suffix.lower()
        if suffix in IMAGE_EXTENSIONS:
            formats.add(IMAGE_EXTENSIONS[suffix])

        if suffix in {".glb", ".gltf"}:
            data = path.read_bytes().lower()
            if b"image/png" in data or b".png" in data:
                formats.add("png")
            if b"image/jpeg" in data or b".jpg" in data or b".jpeg" in data:
                formats.add("jpeg")
            if b"image/webp" in data or b".webp" in data:
                formats.add("webp")

    return formats


def runtime_decoders(sdl_image: Path, formats: set, msys_bin: Path) -> list:
    """
    SDL3_image names its optional decoders as plain strings inside its DLL. Pick the ones for the formats
    in use, so the zip does not carry decoders (and their dependencies) the game never loads.
    """
    names = set(re.findall(rb"lib[a-z0-9_\-]+\.dll", sdl_image.read_bytes()))
    decoders = []

    for raw in sorted(names):
        name = raw.decode("ascii")
        for image_format in formats:
            if any(name.startswith(prefix) for prefix in DECODER_PREFIXES.get(image_format, [])):
                path = msys_bin / name
                if not path.exists():
                    raise PackagingError(f"SDL3_image wants {name} for {image_format} images, but it is not in {msys_bin}")
                decoders.append(path)

    return decoders


def copy_assets(source: Path, destination: Path) -> None:
    for path in source.rglob("*"):
        if path.is_file() and path.suffix.lower() not in SKIPPED_ASSET_SUFFIXES:
            target = destination / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def verify(stage: Path, objdump: Path, environment: dict) -> None:
    """
    The check that matters for a friend's PC: every DLL that anything in the package imports must be in the
    package or part of Windows. Runs with MSYS2 out of the picture, because it only looks in those two places.
    """
    packaged = {path.name.lower() for path in stage.iterdir() if path.suffix.lower() == ".dll"}
    missing = []

    for binary in stage.iterdir():
        if binary.suffix.lower() not in {".exe", ".dll"}:
            continue
        for name in imported_dlls(binary, objdump, environment):
            if name.lower() not in packaged and not is_windows_dll(name):
                missing.append(f"{binary.name} needs {name}")

    if missing:
        raise PackagingError("The package would not run on another PC:\n  " + "\n  ".join(missing))


def make_zip(stage: Path, zip_path: Path, fixed_date: tuple = None) -> None:
    """
    Zip the staged folder itself, so extracting gives one tidy folder instead of loose files. Each file keeps
    its own date unless fixed_date is given (--anonymous), which every file then gets instead.
    """
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(stage.rglob("*")):
            if not path.is_file():
                continue
            name = path.relative_to(stage.parent).as_posix()
            if fixed_date is None:
                archive.write(path, name)
            else:
                entry = zipfile.ZipInfo(name, date_time=fixed_date)
                entry.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(entry, path.read_bytes(), compresslevel=9)


def main() -> int:
    parser = argparse.ArgumentParser(description="Package the demo into a zip for another Windows PC.")
    parser.add_argument("--no-build", action="store_true", help="package the existing build/release/game.exe without running make")
    parser.add_argument("--msys-root", type=Path, default=Path(os.environ.get("MSYS2_ROOT", r"C:\msys64")), help="MSYS2 install folder (default C:\\msys64)")
    parser.add_argument("--name", help="name of the exe, folder and zip (default GameEngineDemo, or Demo with --anonymous)")
    parser.add_argument("--anonymous", action="store_true",
                        help="for sharing anonymously: a README with only short generic control names, a neutral name and undated zip entries, "
                             "so nothing in the zip matches the public repo's text or says when it was made")
    arguments = parser.parse_args()

    if arguments.name is None:
        arguments.name = ANONYMOUS_NAME if arguments.anonymous else "GameEngineDemo"

    msys_bin = arguments.msys_root / "ucrt64" / "bin"
    objdump = msys_bin / "objdump.exe"
    strip = msys_bin / "strip.exe"

    # Full paths, because Windows finds a program by name using this script's PATH, not the PATH passed to the child.
    make = arguments.msys_root / "usr" / "bin" / "make.exe"
    for tool in (objdump, strip, make):
        if not tool.exists():
            raise PackagingError(f"{tool} not found. Is MSYS2 installed with the UCRT64 toolchain? Use --msys-root if it lives elsewhere.")

    environment = msys_environment(arguments.msys_root)

    if not arguments.no_build:
        print("Building the release with make...")
        run([str(make), "release"], environment)

    # Players get the optimised build: the -O0 debug build runs physics several times slower.
    game = REPO_ROOT / "build" / "release" / "game.exe"
    if not game.exists():
        raise PackagingError(f"{game} does not exist. Run without --no-build, or run make release first.")

    stage = REPO_ROOT / "build" / "package" / arguments.name
    zip_path = REPO_ROOT / "build" / f"{arguments.name}.zip"

    if stage.parent.exists():
        shutil.rmtree(stage.parent)
    stage.mkdir(parents=True)

    # Debug symbols are only useful with a debugger and make the exe several times larger.
    exe = stage / f"{arguments.name}.exe"
    run([str(strip), "--strip-debug", "-o", str(exe), str(game)], environment)
    if arguments.anonymous:
        clear_build_time(exe)

    print("Copying assets...")
    copy_assets(REPO_ROOT / "assets", stage / "assets")

    print("Finding DLLs...")
    linked = collect_dlls([game], msys_bin, objdump, environment)

    decoders = []
    if "sdl3_image.dll" in linked:
        formats = image_formats_used(stage / "assets")
        decoders = runtime_decoders(linked["sdl3_image.dll"], formats, msys_bin)
        print(f"  image formats in assets: {', '.join(sorted(formats)) or 'none'}")

    # Decoders can have their own dependencies (libpng needs zlib), so follow their imports too.
    everything = dict(linked)
    everything.update({path.name.lower(): path for path in decoders})
    everything.update(collect_dlls(decoders, msys_bin, objdump, environment))

    for path in sorted(everything.values(), key=lambda p: p.name.lower()):
        shutil.copy2(path, stage / path.name)
        loaded_at_runtime = path in decoders
        print(f"  {path.name}{'  (loaded at runtime by SDL3_image)' if loaded_at_runtime else ''}")

    # The second game is the same exe with a command-line option; a batch file gives it a double-click of its own.
    demo = stage / DEMO_LAUNCHER
    demo.write_text(f'@echo off\r\n"%~dp0{exe.name}" --game demo\r\n', encoding="ascii", newline="")

    controls = readme_controls((REPO_ROOT / "README.md").read_text(encoding="utf-8").replace("\r\n", "\n"), brief=arguments.anonymous)
    title = ANONYMOUS_NAME if arguments.anonymous else "GameEngine demo"
    readme = README_TEXT.format(title=title, underline="=" * len(title), exe_name=exe.name, demo=demo.name,
                                controls=controls)
    (stage / "README.txt").write_text(readme.replace("\n", "\r\n"), encoding="utf-8", newline="")

    print("Checking every DLL resolves inside the package or Windows...")
    verify(stage, objdump, environment)

    if zip_path.exists():
        zip_path.unlink()
    make_zip(stage, zip_path, ANONYMOUS_ZIP_DATE if arguments.anonymous else None)

    size = zip_path.stat().st_size / (1024 * 1024)
    print(f"\nDone: {zip_path} ({size:.1f} MB)")
    print("Test it by extracting the zip somewhere new and double-clicking the exe.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PackagingError as error:
        print(f"\nPackaging failed: {error}", file=sys.stderr)
        sys.exit(1)
