import base64
import json
import struct
from pathlib import Path
from urllib.parse import unquote

root = Path(__file__).resolve().parents[1]
source = root / "assets/models/textured_quad.gltf"
destination = source.with_suffix(".glb")

document = json.loads(source.read_text(encoding="utf-8"))

# Decode the quad's embedded geometry buffer.
buffer_uri = document["buffers"][0]["uri"]
geometry = base64.b64decode(
    buffer_uri.split(",", 1)[1],
    validate=True,
)

binary = bytearray(geometry)

# Align the image to a four-byte boundary.
binary.extend(b"\0" * ((-len(binary)) % 4))
image_offset = len(binary)

image_uri = document["images"][0]["uri"]
image_path = source.parent / unquote(image_uri)
image_bytes = image_path.read_bytes()

binary.extend(image_bytes)

image_view_index = len(document["bufferViews"])
document["bufferViews"].append({
    "buffer": 0,
    "byteOffset": image_offset,
    "byteLength": len(image_bytes),
})

document["images"][0] = {
    "bufferView": image_view_index,
    "mimeType": "image/png",
}

# No URI: buffer zero now comes from the GLB binary chunk.
document["buffers"] = [{
    "byteLength": len(binary),
}]

json_chunk = json.dumps(
    document,
    separators=(",", ":"),
).encode("utf-8")

json_chunk += b" " * ((-len(json_chunk)) % 4)
binary.extend(b"\0" * ((-len(binary)) % 4))

total_length = 12 + 8 + len(json_chunk) + 8 + len(binary)

with destination.open("wb") as output:
    output.write(struct.pack("<4sII", b"glTF", 2, total_length))
    output.write(struct.pack("<I4s", len(json_chunk), b"JSON"))
    output.write(json_chunk)
    output.write(struct.pack("<I4s", len(binary), b"BIN\0"))
    output.write(binary)

print(f"Created {destination}")