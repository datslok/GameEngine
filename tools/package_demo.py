"""
Package the demo into a zip that runs on a Windows PC without MSYS2.

Usage (from anywhere):
    python tools/package_demo.py               build with make, then package
    python tools/package_demo.py --no-build    package the existing build/game.exe

Output: build/GameEngineDemo.zip, staged in build/package/GameEngineDemo/.

Only the Python standard library is used. It needs MSYS2's UCRT64 tools (make, strip, objdump),
found in C:/msys64 by default; pass --msys-root or set MSYS2_ROOT if MSYS2 lives elsewhere.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Shader sources are compiled to .spv at build time; the game only loads the .spv files.
SKIPPED_ASSET_SUFFIXES = {".vert", ".frag"}

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

README_TEXT = """GameEngine demo
===============

How to run
----------
1. Extract the whole zip into a folder (do not run it from inside the zip).
2. Double-click {exe_name}.

Keep the exe next to its DLLs and the assets folder. A console window opens
alongside the game; if something goes wrong, the error is shown there.

Windows may warn that the app is from an unknown publisher, because it is not
code-signed. Click "More info", then "Run anyway".

Requirements: Windows 10 or 11 (64-bit) and a graphics card with Vulkan
support (any reasonably recent NVIDIA, AMD or Intel GPU with up-to-date drivers).

Controls
--------
The demo starts in first-person mode. Click in the window to capture the mouse.

  F1          First-person mode
  F2          MOBA mode
  F3          Free camera mode
  Alt+Enter   Toggle fullscreen
  Escape      Release the mouse
  F           Toggle the flashlight (in MOBA mode the duck carries it)
  F4          Debug view: bounding boxes, light markers, walk target

First person and free camera:
  Mouse       Look around
  W A S D     Move
  Space       Jump (first person)
  Space/Ctrl  Up / down (free camera)
  Shift       Run (first person) or fly faster (free camera)

MOBA:
  Left-click              Keep the cursor inside the window
  Right-click (or hold)   Move the duck
  Cursor at screen edge   Pan the camera
  Space                   Lock the camera on the duck
"""


class PackagingError(Exception):
    pass


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


def make_zip(stage: Path, zip_path: Path) -> None:
    """Zip the staged folder itself, so extracting gives one tidy folder instead of loose files."""
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(stage.parent))


def main() -> int:
    parser = argparse.ArgumentParser(description="Package the demo into a zip for another Windows PC.")
    parser.add_argument("--no-build", action="store_true", help="package the existing build/game.exe without running make")
    parser.add_argument("--msys-root", type=Path, default=Path(os.environ.get("MSYS2_ROOT", r"C:\msys64")), help="MSYS2 install folder (default C:\\msys64)")
    parser.add_argument("--name", default="GameEngineDemo", help="name of the exe, folder and zip (default GameEngineDemo)")
    arguments = parser.parse_args()

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
        print("Building with make...")
        run([str(make)], environment)

    game = REPO_ROOT / "build" / "game.exe"
    if not game.exists():
        raise PackagingError(f"{game} does not exist. Run without --no-build, or build it first.")

    stage = REPO_ROOT / "build" / "package" / arguments.name
    zip_path = REPO_ROOT / "build" / f"{arguments.name}.zip"

    if stage.parent.exists():
        shutil.rmtree(stage.parent)
    stage.mkdir(parents=True)

    # Debug symbols are only useful with a debugger and make the exe several times larger.
    exe = stage / f"{arguments.name}.exe"
    run([str(strip), "--strip-debug", "-o", str(exe), str(game)], environment)

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

    (stage / "README.txt").write_text(README_TEXT.format(exe_name=exe.name).replace("\n", "\r\n"), encoding="utf-8", newline="")

    print("Checking every DLL resolves inside the package or Windows...")
    verify(stage, objdump, environment)

    if zip_path.exists():
        zip_path.unlink()
    make_zip(stage, zip_path)

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
