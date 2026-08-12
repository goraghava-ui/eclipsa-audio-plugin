#!/usr/bin/env python3
"""Render Studio's PannerScope offscreen for the B5 parity pair.

Dome constraint on both sides, same poses, same speaker layout (KALA's
smpte_714_layout, which is what Studio's main window feeds the scope).
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, "/data/projects/friday/friday-studio")

from PySide6.QtCore import QPoint                     # noqa: E402
from PySide6.QtGui import QRegion                     # noqa: E402
from PySide6.QtGui import QPainter, QPixmap           # noqa: E402
from PySide6.QtWidgets import QApplication, QWidget   # noqa: E402
from studio.compat import kala                        # noqa: E402
from studio.ui import theme as T                      # noqa: E402
from studio.ui.widgets import PannerScope             # noqa: E402

SIZE = 560
POSES = [("az+30_el0", 30.0, 0.0), ("az0_el60", 0.0, 60.0)]

app = QApplication([])
speakers, _names = kala().smpte_714_layout()

for name, az, el in POSES:
    scope = PannerScope()
    scope.resize(SIZE, SIZE)
    scope.set_speakers(list(speakers))
    scope.set_constraint("dome")
    scope.set_object(az, el)
    # Paint the window ground behind it, exactly as the app does, so the
    # vignette falls off into the same colour on both sides of the pair.
    # Without this Qt's default light palette shows in the corners and the
    # pair would differ on something that is not the design.
    pix = QPixmap(SIZE, SIZE)
    pix.fill(T.BG0)
    painter = QPainter(pix)
    # DrawChildren only: DrawWindowBackground would paint Qt's default light
    # palette over the ground we just filled, and the pair would differ on
    # something that is not the design.
    scope.render(painter, QPoint(0, 0), QRegion(),
                 QWidget.RenderFlag.DrawChildren)
    painter.end()
    out = f"/data/build/b5/studio_{name}.png"
    pix.save(out)
    print(f"{out}  {pix.width()}x{pix.height()}")
