#!/usr/bin/env python3
"""Compose the B5 parity sheets: Bridge's pad beside Studio's PannerScope.

The two panels are rendered separately -- `bridge_*.png` by the gtest
`FridayPannerScope_test.renders_the_parity_poses` (run with FRIDAY_B5_SHOTS
set), `studio_*.png` by `studio_scope_shot.py` -- and this only lays them out,
so the sheet can be regenerated whenever either side changes without either
side having to know about the other.

The geometry is measured from the original B5 sheets: 1184x606 on the window
ground, panels at (16, 30) and (608, 30), labels on the panel's left edge.

    python3 parity_sheet.py [out-dir]      # default: this directory
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import Qt                                    # noqa: E402
from PySide6.QtGui import QColor, QFont, QPainter, QPixmap       # noqa: E402
from PySide6.QtWidgets import QApplication                       # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else HERE

W, H = 1184, 606
PANEL = 560
LEFT_X, RIGHT_X, PANEL_Y = 16, 608, 30
BG = QColor(11, 13, 17)
LABEL = QColor(232, 235, 240)
LABEL_BASELINE = 23

POSES = ["az+30_el0", "az0_el60"]
CAPTIONS = ("FRIDAY Bridge — panner pad (JUCE)",
            "FRIDAY Studio — PannerScope (Qt)")

app = QApplication([])

for pose in POSES:
    sheet = QPixmap(W, H)
    sheet.fill(BG)
    painter = QPainter(sheet)
    painter.setRenderHint(QPainter.TextAntialiasing, True)

    for x, caption, src in ((LEFT_X, CAPTIONS[0], f"bridge_{pose}.png"),
                            (RIGHT_X, CAPTIONS[1], f"studio_{pose}.png")):
        panel = QPixmap(os.path.join(HERE, src))
        if panel.isNull():
            raise SystemExit(f"missing panel: {src}")
        if panel.width() != PANEL or panel.height() != PANEL:
            raise SystemExit(f"{src} is {panel.width()}x{panel.height()}, "
                             f"expected {PANEL}x{PANEL}")
        painter.drawPixmap(x, PANEL_Y, panel)

        font = QFont()
        font.setPixelSize(15)
        painter.setFont(font)
        painter.setPen(LABEL)
        painter.drawText(x + 1, LABEL_BASELINE, caption)

    painter.end()
    out = os.path.join(OUT, f"parity_{pose}.png")
    if not sheet.save(out, "PNG"):
        raise SystemExit(f"could not write {out}")
    print("wrote", out)
