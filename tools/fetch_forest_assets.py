"""
Fetch the forest game's models and textures into assets/forest, which git ignores.

Usage (from anywhere):
    python tools/fetch_forest_assets.py            # download what is missing, check every file against its SHA-256
    python tools/fetch_forest_assets.py --record   # download, then write every file's SHA-256 into the manifest

Every asset is CC0 (public domain): scanned plants, rocks, logs and textures from Poly Haven and leaf atlases and ground
materials from ambientCG. The list, each asset's source and the maps the engine reads are in tools/forest_assets.json;
the repository keeps only that list, so it stays small (the files are about 150 MB). Without them the forest game runs
with simple stand-ins.

- Poly Haven: its public API (api.polyhaven.com/files/<id>) gives each file's address and MD5. A model is its glTF
  file with the buffers and textures it names, at their relative paths; a texture is one file a map (colour, normal,
  ARM: occlusion, roughness and metal in red, green and blue, as glTF packs them, and height).
- ambientCG: one zip an asset (kept in build/downloads/forest, so a second run needs no download), of which only the
  maps the manifest names are extracted, as <role>.png.

A file already present whose SHA-256 matches the manifest is left alone. A mismatch is an error: delete the file (or
the asset's folder) and run again, or, if the source really changed, run with --record and check the result.

Only the Python standard library is used.
"""

import hashlib
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = REPO_ROOT / "tools" / "forest_assets.json"
DOWNLOADS = REPO_ROOT / "build" / "downloads" / "forest"
HEADERS = {"User-Agent": "GameEngineAssetFetch/1.0"}

# Poly Haven offers each map in several formats: lossless where precision shows (normals, heights), JPEG elsewhere.
PREFERRED_FORMATS = {"normal": ["png", "jpg"], "height": ["png", "jpg"]}
DEFAULT_FORMATS = ["jpg", "png"]


class FetchError(Exception):
    pass


def read_url(url: str) -> bytes:
    request = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def write_atomically(path: Path, data: bytes) -> None:
    """Writes to a temporary name first, so an interrupted run never leaves half a file under the real name."""
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + ".partial")
    partial.write_bytes(data)
    partial.replace(path)


def fetch_checked(url: str, path: Path, md5: str | None) -> None:
    data = read_url(url)

    if md5 and hashlib.md5(data).hexdigest() != md5:
        raise FetchError(f"{url} did not match the MD5 its source gave")

    write_atomically(path, data)


def polyhaven_files(asset: dict) -> list[tuple[str, str, str | None]]:
    """(relative path in the asset's folder, address, MD5) for every file of one Poly Haven asset."""
    files = json.loads(read_url(f"https://api.polyhaven.com/files/{asset['id']}"))
    resolution = asset["resolution"]

    if asset["kind"] == "model":
        gltf = files["gltf"][resolution]["gltf"]
        wanted = [(Path(gltf["url"]).name, gltf["url"], gltf.get("md5"))]
        wanted += [(relative, include["url"], include.get("md5")) for relative, include in gltf.get("include", {}).items()]
        return wanted

    wanted = []
    for role, name in asset["maps"].items():
        formats = files[name][resolution]

        for extension in PREFERRED_FORMATS.get(role, DEFAULT_FORMATS):
            if extension in formats:
                wanted.append((f"{role}.{extension}", formats[extension]["url"], formats[extension].get("md5")))
                break
        else:
            raise FetchError(f"{asset['id']} has no {name} map at {resolution} in a format the engine reads")

    return wanted


def fetch_polyhaven(asset: dict, folder: Path) -> list[Path]:
    paths = []

    for relative, url, md5 in polyhaven_files(asset):
        path = folder / relative

        if not path.exists():
            print(f"  {asset['id']}/{relative}")
            fetch_checked(url, path, md5)

        paths.append(path)

    return paths


def fetch_ambientcg(asset: dict, folder: Path) -> list[Path]:
    name = f"{asset['id']}_{asset['resolution']}"
    targets = {role: folder / f"{role}.png" for role in asset["maps"]}

    if all(path.exists() for path in targets.values()):
        return list(targets.values())

    archive = DOWNLOADS / f"{name}.zip"

    if not archive.exists():
        print(f"  {asset['id']} ({name}.zip)")
        write_atomically(archive, read_url(f"https://ambientcg.com/get?file={name}.zip"))

    with zipfile.ZipFile(archive) as contents:
        names = contents.namelist()

        for role, map_name in asset["maps"].items():
            member = next((entry for entry in names if entry.endswith(f"_{map_name}.png")), None)

            if member is None:
                raise FetchError(f"{archive.name} has no {map_name} map")

            write_atomically(targets[role], contents.read(member))

    return list(targets.values())


def main() -> int:
    record = "--record" in sys.argv[1:]
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    root = REPO_ROOT / manifest["folder"]
    recorded = manifest.get("sha256", {})
    found = {}
    problems = []

    try:
        for asset in manifest["assets"]:
            folder = root / asset["id"]
            fetch = fetch_polyhaven if asset["source"] == "polyhaven" else fetch_ambientcg

            for path in fetch(asset, folder):
                key = path.relative_to(root).as_posix()
                found[key] = sha256_of(path)

                if not record and recorded.get(key) not in (None, found[key]):
                    problems.append(key)
    except (FetchError, OSError) as error:
        print(f"Failed: {error}", file=sys.stderr)
        return 1

    if record:
        manifest["sha256"] = dict(sorted(found.items()))
        MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"Recorded {len(found)} files' SHA-256 in {MANIFEST.relative_to(REPO_ROOT)}")
        return 0

    unrecorded = [key for key in found if key not in recorded]

    if problems:
        print("These files do not match their recorded SHA-256 (delete them and run again):", file=sys.stderr)
        for key in problems:
            print(f"  {key}", file=sys.stderr)
        return 1

    if unrecorded:
        print(f"{len(unrecorded)} files have no recorded SHA-256 yet: run with --record once they look right.")

    print(f"The forest's {len(manifest['assets'])} assets ({len(found)} files) are in {root.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
