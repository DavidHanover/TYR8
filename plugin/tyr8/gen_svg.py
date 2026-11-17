#!/usr/bin/env python3
"""Writes tyr8.svg, the screen's SVG: empty. Since 0.9.6 the whole picture (road, tyre, rim, labels) is drawn by
tyr8.c, lines with op1_raster_line and the black rubber as filled pixels, because the tyre deforms (flattens with
sag), grows spokes and sits on a diagonal road, which the native SVG renderer's one-matrix-a-group parts cannot do.
The screen object still needs an SVG, as kastle's all-pixel page has.
Copyright (C) 2026 David Hanover. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent
(HERE / 'tyr8.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="320" height="160" viewBox="0 0 320 160"/>\n')
old = HERE / 'tyr8_geom.h'
if old.exists():
    old.unlink()                              # the geometry now lives in tyr8.c
print('tyr8.svg', (HERE / 'tyr8.svg').stat().st_size, 'bytes')
