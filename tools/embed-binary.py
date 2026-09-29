#!/usr/bin/env python3
"""Embed a built shader in an aligned C++ header."""
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
data = source.read_bytes()
rows = [", ".join(f"0x{byte:02x}" for byte in data[i:i+16]) for i in range(0, len(data), 16)]
destination.write_text("#pragma once\nalignas(4) static unsigned char modelShader[] = {\n" +
                       ",\n".join(rows) + "\n};\n")
