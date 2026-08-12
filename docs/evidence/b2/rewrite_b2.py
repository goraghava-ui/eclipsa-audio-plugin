"""Author the Bridge plugin states for the B2 gate: one Audio Element, the
panner bound to it, and the renderer armed to export .iamf."""
import base64, struct, re, sys

def load(p):
    d = base64.b64decode(open(p).read())
    n = struct.unpack_from('<I', d, 12)[0]
    return d[16:16+n].decode('utf-8'), d[16+n:]

def save(p, xml, tail):
    xb = xml.encode('utf-8')
    hdr = struct.pack('<II', len(xb)+len(tail), 1) + b'VC2!' + struct.pack('<I', len(xb))
    open(p, 'w').write(base64.b64encode(hdr+xb+tail).decode('ascii'))

AE_ID = "a1b2c3d4e5f6470880b1c2d3e4f50011"
OUT_IAMF = sys.argv[1] if len(sys.argv) > 1 else "/data/build/b2/bridge.iamf"

xml, tail = load('panner.b64')
xml = re.sub(r'audio_element_id="[0-9a-f]*"', f'audio_element_id="{AE_ID}"', xml)
xml = re.sub(r'first_channel="-?\d+"', 'first_channel="0"', xml)
xml = re.sub(r'(<audio_element_spatial_layout_repository_state[^>]*?)layout="\d+"', r'\1layout="7"', xml)
xml = re.sub(r'layout_selected="\d+"', 'layout_selected="1"', xml)
xml = re.sub(r'panning_enabled="\d+"', 'panning_enabled="1"', xml)
save('panner-b2.b64', xml, tail)
print("panner:", re.search(r'<audio_element_spatial_layout_repository_state[^>]*>', xml).group()[:150])

rxml, rtail = load('renderer.b64')
rxml = rxml.replace('<audio_elements/>',
    f'<audio_elements><audio_element id="{AE_ID}" name="AE1" description="" '
    f'channel_config="7" first_channel="0"/></audio_elements>')
rxml = re.sub(r'speaker_layout="[^"]*"', 'speaker_layout="7.1.4"', rxml)
# arm the export: IAMF (audioFileFormat 0), 24-bit, 48 kHz, whole timeline
rxml = re.sub(r'exportFile="[^"]*"', f'exportFile="{OUT_IAMF}"', rxml)
rxml = re.sub(r'exportAudio="\d+"', 'exportAudio="1"', rxml)
rxml = re.sub(r'audioFileFormat="\d+"', 'audioFileFormat="0"', rxml)
rxml = re.sub(r'bitDepth="\d+"', 'bitDepth="24"', rxml)
rxml = re.sub(r'sampleRate="\d+"', 'sampleRate="48000"', rxml)
rxml = re.sub(r'startSampleIdx="\d+"', 'startSampleIdx="0"', rxml)
rxml = re.sub(r'endSampleIdx="\d+"', 'endSampleIdx="0"', rxml)
save('renderer-b2.b64', rxml, rtail)
print("renderer:", re.search(r'<file_export[^>]*>', rxml).group()[:220])
