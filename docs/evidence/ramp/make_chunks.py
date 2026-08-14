"""Author the Bridge plugin states for the automation-ramp gate.

A — the B2-5 regression, unchanged from B6: elevation "none", az +30.000, el 0.
B — the elevated pose, elevation "dome" (3): X/Y put the object at normalised
    radius 0.5, and ElevationListener is left to derive Z from the surface.
    On the unit sphere that is el = acos(0.5) = 60 deg exactly; on the old
    surface it was 55.7, which is what this gate exists to catch.
"""
import base64, struct, re

B6 = "/data/build/b6"
OUT = "/data/build/sweep"


def load(p):
    d = base64.b64decode(open(p).read())
    n = struct.unpack_from("<I", d, 12)[0]
    return d[16:16 + n].decode("utf-8"), d[16 + n:]


def save(p, xml, tail):
    xb = xml.encode("utf-8")
    hdr = (struct.pack("<II", len(xb) + len(tail), 1) + b"VC2!" +
           struct.pack("<I", len(xb)))
    open(p, "w").write(base64.b64encode(hdr + xb + tail).decode("ascii"))


def elevation(xml, value):
    return re.sub(r'(<audio_element_spatial_layout_repository_state[^>]*?)'
                  r'elevation="-?\d+"', rf'\1elevation="{value}"', xml)


def export_to(xml, path):
    return re.sub(r'exportFile="[^"]*"', f'exportFile="{path}"', xml)


pxml, ptail = load(f"{B6}/panner-p1.b64")
rxml, rtail = load(f"{B6}/renderer-p1.b64")

# A: byte-for-byte the B6 panner state, renderer retargeted at b7.
save(f"{OUT}/panner-sweep.b64", pxml, ptail)
save(f"{OUT}/renderer-sweep.b64", export_to(rxml, f"{OUT}/bridge_sweep.iamf"), rtail)

print("sweep: flat elevation; az comes from a REAPER envelope")
