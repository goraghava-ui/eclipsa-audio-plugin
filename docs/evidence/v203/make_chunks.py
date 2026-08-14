"""Author the Bridge plugin states for the V2-03 audio-feed gate.

A — the B2-5 regression, unchanged from B6: elevation "none", az +30.000, el 0.
B — the elevated pose, elevation "dome" (3): X/Y put the object at normalised
    radius 0.5, and ElevationListener is left to derive Z from the surface.
    On the unit sphere that is el = acos(0.5) = 60 deg exactly; on the old
    surface it was 55.7, which is what this gate exists to catch.
"""
import base64, struct, re

B6 = "/data/build/b6"
OUT = "/data/build/v203"


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
save(f"{OUT}/panner-v203.b64", pxml, ptail)
save(f"{OUT}/renderer-v203.b64", export_to(rxml, f"{OUT}/bridge_v203.iamf"), rtail)

# The bench's renderer state has an EMPTY <mix_presentations/>, and
# RendererProcessor::initializeMixPresentations() then creates one with no
# audio elements in it. Eclipsa renders a mix presentation, so with nothing in
# it the monitoring path renders SILENCE -- which no gate has ever noticed,
# because the KALA export path renders captured objects and never reads this
# bed. V2-03 is the first thing that does, so the presentation has to be real.
AE_ID = "a1b2c3d4e5f6470880b1c2d3e4f50011"
MIX_ID = "b7c8d9e0f1a24b3c8d9e0f1a2b3c4d5e"
mix = (
    f'<mix_presentations><mix_presentation id="{MIX_ID}" '
    'presentation_name="V2-03 monitor" default_mix_gain="1.0" language="0" '
    'tag_names="" tag_values="">'
    f'<audio_elements><mix_presentation_audio_element id="{AE_ID}" '
    'default_mix_gain="1.0" reference_id="0" name="AE1" is_binaural="0"/>'
    '</audio_elements></mix_presentation></mix_presentations>'
)
rxml = rxml.replace("<mix_presentations/>", mix)

# The renderer looks the presentation's loudness record up with .value(), so a
# presentation with no matching entry aborts the plugin outright
# (std::bad_optional_access, observed). Author it alongside. Layout 7 = 7.1.4,
# matching the room setup and the audio element's channel_config.
loud = (
    '<mix_presentation_loudness>'
    f'<mix_presentation_loudness id="{MIX_ID}" largest_layout="7">'
    '<layout_loudnesses>'
    f'<layout_loudness id="{MIX_ID}" audio_element_layout="7" '
    'Integrated_Loudness="0.0" Digital_Peak="0.0" True_Peak="0.0"/>'
    '</layout_loudnesses>'
    '</mix_presentation_loudness>'
    '</mix_presentation_loudness>'
)
rxml = rxml.replace("<mix_presentation_loudness/>", loud)
assert "mix_presentation_audio_element" in rxml, "mix presentation not authored"
save(f"{OUT}/renderer-v203.b64", export_to(rxml, f"{OUT}/bridge_v203.iamf"), rtail)
print("v203: flat elevation, az +30, monitor mix presentation authored")
