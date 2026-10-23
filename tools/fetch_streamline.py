"""
Fetch NVIDIA Streamline's DLLs (DLSS) into external/streamline/bin, which git ignores.

Usage (from anywhere):
    python tools/fetch_streamline.py

DLSS comes under the NVIDIA RTX SDK licence, which lets an application ship it but not hand it out on its own, so the
repository does not carry its DLLs (Streamline's own headers are MIT and are committed, so the engine always builds):
this downloads the pinned Streamline release from NVIDIA's GitHub (about 276 MB, kept in build/downloads so a second
run needs no download), checks its SHA-256, and extracts only what the engine uses: the signed DLLs DLSS super
resolution needs (sl.interposer, sl.common, sl.dlss and nvngx_dlss, and NvLowLatencyVk, which sl.common loads for
Reflex whatever the features) with their licences into bin, the programming guides into docs, and the headers into
include (the same as the committed ones, unless VERSION is moved on). Without the DLLs the game runs and DLSS is
unavailable.

Only the Python standard library is used.
"""

import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

VERSION = "v2.14.16"
URL = f"https://github.com/NVIDIA-RTX/Streamline/releases/download/{VERSION}/streamline-sdk-{VERSION}.zip"
SHA256 = "32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b"

DOWNLOAD = REPO_ROOT / "build" / "downloads" / f"streamline-sdk-{VERSION}.zip"
DESTINATION = REPO_ROOT / "external" / "streamline"

# The production (signed, no on-screen text) builds; bin/x64/development has the debugging ones.
RUNTIME_FILES = ["sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "nvngx_dlss.dll", "NvLowLatencyVk.dll",
                 "nvngx_dlss.license.txt", "reflex.license.txt"]
DOCUMENTS = ["docs/ProgrammingGuide.md", "docs/ProgrammingGuideDLSS.md", "docs/ProgrammingGuideManualHooking.md"]

# Every header is MIT but this one (NVIDIA's Nsight Perf terms), which the engine does not use.
SKIPPED_HEADERS = {"sl_nvperf.h"}


class FetchError(Exception):
    pass


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download() -> None:
    """Downloads into a temporary name first, so an interrupted download is never mistaken for the release."""
    if DOWNLOAD.exists() and sha256_of(DOWNLOAD) == SHA256:
        print(f"Using {DOWNLOAD}")
        return

    DOWNLOAD.parent.mkdir(parents=True, exist_ok=True)
    partial = DOWNLOAD.with_suffix(".part")
    print(f"Downloading {URL} (about 276 MB)...")

    with urllib.request.urlopen(URL) as response, partial.open("wb") as file:
        shutil.copyfileobj(response, file, 1 << 20)

    found = sha256_of(partial)
    if found != SHA256:
        partial.unlink()
        raise FetchError(f"The download's SHA-256 is {found}, not {SHA256}: not the pinned release, so nothing was extracted.")

    partial.replace(DOWNLOAD)


def extract() -> None:
    """Only named files, each to a path this script chooses, so nothing in the zip can write elsewhere."""
    for folder in (DESTINATION / "bin", DESTINATION / "docs"):
        if folder.exists():
            shutil.rmtree(folder)

    with zipfile.ZipFile(DOWNLOAD) as archive:
        names = set(archive.namelist())
        wanted = {f"bin/x64/{name}": DESTINATION / "bin" / name for name in RUNTIME_FILES}
        wanted.update({name: DESTINATION / "docs" / Path(name).name for name in DOCUMENTS})
        wanted["license.txt"] = DESTINATION / "license.txt"
        wanted.update({name: DESTINATION / "include" / Path(name).name for name in names
                       if name.startswith("include/") and name.endswith(".h") and name.count("/") == 1
                       and Path(name).name not in SKIPPED_HEADERS})

        missing = [name for name in wanted if name not in names]
        if missing:
            raise FetchError("The release lacks " + ", ".join(missing))

        for name, target in sorted(wanted.items()):
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(name))
            print(f"  {target.relative_to(REPO_ROOT)}")


def main() -> int:
    try:
        download()
        extract()
    except (FetchError, OSError) as error:
        print(f"fetch_streamline: {error}", file=sys.stderr)
        return 1

    print(f"\nStreamline {VERSION} is in {DESTINATION.relative_to(REPO_ROOT)}. Run make to copy its DLLs beside the game.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
