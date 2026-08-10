import base64, struct, sys, re

def load(p):
    d = base64.b64decode(open(p).read())
    xml_len = struct.unpack_from('<I', d, 12)[0]
    xml = d[16:16+xml_len].decode('utf-8')
    tail = d[16+xml_len:]
    return d[:16], xml, tail

def save(p, xml, tail):
    xb = xml.encode('utf-8')
    hdr = struct.pack('<II', len(xb)+len(tail), 1) + b'VC2!' + struct.pack('<I', len(xb))
    open(p,'w').write(base64.b64encode(hdr+xb+tail).decode('ascii'))

AE_ID = "a1b2c3d4e5f6470880b1c2d3e4f50011"   # shared audio element uuid

# ---- panner ----
_, xml, tail = load('panner.b64')
xml = re.sub(r'audio_element_id="[0-9a-f]*"', f'audio_element_id="{AE_ID}"', xml)
xml = re.sub(r'first_channel="-?\d+"', 'first_channel="0"', xml)
xml = re.sub(r'(<audio_element_spatial_layout_repository_state[^>]*?)layout="\d+"', r'\1layout="7"', xml)
xml = re.sub(r'layout_selected="\d+"', 'layout_selected="1"', xml)
xml = re.sub(r'panning_enabled="\d+"', 'panning_enabled="1"', xml)
save('panner-mod.b64', xml, tail)
print("PANNER:", re.search(r'<audio_element_spatial_layout_repository_state[^>]*>', xml).group())

# ---- renderer ----
_, rxml, rtail = load('renderer.b64')
rxml = rxml.replace('<audio_elements/>',
    f'<audio_elements><audio_element id="{AE_ID}" name="AE1" description="" '
    f'channel_config="7" first_channel="0"/></audio_elements>')
rxml = re.sub(r'speaker_layout="[^"]*"', 'speaker_layout="7.1.4"', rxml)
save('renderer-mod.b64', rxml, rtail)
print("RENDERER:", re.search(r'<audio_elements>.*?</audio_elements>', rxml).group())
print("ROOM:", re.search(r'<room_setup[^>]*>', rxml).group())
