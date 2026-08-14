"""Author the two Bridge plugin states for the B7 dome gate.

A — the B2-5 regression, unchanged from B6: elevation "none", az +30.000, el 0.
B — the elevated pose, elevation "dome" (3): X/Y put the object at normalised
    radius 0.5, and ElevationListener is left to derive Z from the surface.
    On the unit sphere that is el = acos(0.5) = 60 deg exactly; on the old
    surface it was 55.7, which is what this gate exists to catch.
"""
import base64, struct, re

B6 = "/data/build/b6"
OUT = "/data/build/b7"


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
save(f"{OUT}/panner-a.b64", pxml, ptail)
save(f"{OUT}/renderer-a.b64", export_to(rxml, f"{OUT}/bridge_a.iamf"), rtail)

# B: dome elevation (kDome = 3), with the horizontal position baked into the
# saved state rather than automated in from the host.
#
# Why baked in: ElevationListener recomputes Z from the ValueTree whenever the
# spatial-layout repository changes, and a chunk load is such a change. Driving
# X/Y as host parameter automation instead would depend on APVTS's own
# parameter->ValueTree flush, which is a different mechanism and not the one
# under test here. Loading the state exercises exactly the path a session
# recall takes, and the surface is what fills in Z.
#
#   X = 0.0, Y = 25.0  ->  normalised radius 0.5  ->  el = acos(0.5) = 60 deg
# Z is left at 0.0 in the state so the value the surface writes is visible.
def param(xml, pid, value):
    return re.sub(rf'(<PARAM id="{pid}" value=")[^"]*(")',
                  rf'\g<1>{value}\g<2>', xml)


bxml = elevation(pxml, 3)
bxml = param(bxml, "X", "0.0")
bxml = param(bxml, "Y", "25.0")
bxml = param(bxml, "Z", "0.0")
assert 'elevation="3"' in bxml, "dome elevation not applied"
assert '<PARAM id="Y" value="25.0"/>' in bxml, "Y not applied"
save(f"{OUT}/panner-b.b64", bxml, ptail)
save(f"{OUT}/renderer-b.b64", export_to(rxml, f"{OUT}/bridge_b.iamf"), rtail)

for tag, x in (("A", pxml), ("B", bxml)):
    m = re.search(r'<audio_element_spatial_layout_repository_state[^>]*>', x)
    print(tag, m.group()[:200])
