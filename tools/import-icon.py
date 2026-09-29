#!/usr/bin/env python3
"""Convert an explicitly selected original game texture into one native UI icon."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

BUILDER = "devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=["star-bit", "camera", "pointer"])
    parser.add_argument("archive", type=Path)
    parser.add_argument("entry", help="Exact TPL/BTI entry path printed by archive_tool")
    parser.add_argument("--archive-tool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="SD staging ui directory, outside this checkout")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    if output.is_relative_to(root):
        parser.error("Keep extracted game assets outside the source checkout")
    if Path(args.entry).suffix.lower() not in (".tpl", ".bti"):
        parser.error("Select a TPL or BTI texture from the archive listing")
    archive = args.archive.resolve(strict=True)
    archive_tool = args.archive_tool.resolve(strict=True)
    if not shutil.which("wimgt"):
        parser.error("Wiimms Image Tool (wimgt) is required to decode original textures")
    output.mkdir(parents=True, exist_ok=True)
    names = [output / f"{args.kind}.{ext}" for ext in ("t3x", "png", "json")]
    if any(path.exists() for path in names):
        parser.error("Output already exists; use a new staging directory to preserve previous assets")
    with tempfile.TemporaryDirectory(prefix="smg-icon-", dir=output.parent) as temporary:
        work = Path(temporary)
        original = work / ("original" + Path(args.entry).suffix.lower())
        png = work / f"{args.kind}.png"
        texture = work / f"{args.kind}.t3x"
        manifest = work / f"{args.kind}.json"
        subprocess.run([str(archive_tool), str(archive), "--extract", args.entry, str(original)], check=True)
        subprocess.run(["wimgt", "decode", "--no-wildcards", str(original), "--dest", str(png)], check=True)
        subprocess.run([
            "docker", "run", "--rm", "--network", "none", "--user", f"{os.getuid()}:{os.getgid()}",
            "--mount", f"type=bind,source={work},target=/work", BUILDER,
            "/opt/devkitpro/tools/bin/tex3ds", f"/work/{png.name}", "-o", f"/work/{texture.name}",
            "-f", "rgba8", "-z", "auto",
        ], check=True)
        manifest.write_text(json.dumps({
            "kind": args.kind, "archive": str(archive), "archive_sha256": digest(archive),
            "entry": args.entry, "entry_sha256": digest(original), "png_sha256": digest(png),
            "texture_sha256": digest(texture), "builder": BUILDER,
        }, indent=2) + "\n")
        for path in (png, texture, manifest):
            # Exclusive creation avoids overwriting another import racing this process.
            with path.open("rb") as source, (output / path.name).open("xb") as destination:
                shutil.copyfileobj(source, destination)
    print(f"Imported {args.kind}: {output / texture.name}")


if __name__ == "__main__":
    main()
