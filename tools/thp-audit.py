#!/usr/bin/env python3
"""Compares the luma planes tools/ThpAudit.cpp wrote with libjpeg's (through PIL) for the same frames.
usage: thp-audit.py OUT_DIR"""
import glob, io, sys
from PIL import Image

worst, frames, differing = 0, 0, 0
for jpg in sorted(glob.glob(sys.argv[1] + '/*.jpg')):
    ref = Image.open(jpg)
    ref.draft('YCbCr', ref.size)
    want = ref.getchannel(0).tobytes()
    got = open(jpg[:-4] + '.y', 'rb').read()
    diff = max(abs(a - b) for a, b in zip(want, got))
    worst = max(worst, diff)
    differing += sum(a != b for a, b in zip(want, got))
    frames += 1
print('%d frames compared, %d luma samples differ, worst by %d' % (frames, differing, worst))
