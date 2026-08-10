import wave, math, sys, hashlib
def analyse(p, label=None):
    w=wave.open(p,'rb'); ch,sw,sr,n=w.getnchannels(),w.getsampwidth(),w.getframerate(),w.getnframes()
    raw=w.readframes(n); w.close()
    peak=[0.0]*ch; sq=[0.0]*ch; step=sw*ch; full=float(1<<(8*sw-1))
    for i in range(0,len(raw)-step+1,step):
        for c in range(ch):
            v=int.from_bytes(raw[i+c*sw:i+c*sw+sw],'little',signed=True)/full
            a=abs(v)
            if a>peak[c]: peak[c]=a
            sq[c]+=v*v
    db=lambda x: 20*math.log10(x) if x>1e-12 else float('-inf')
    act=[(c+1,round(db(peak[c]),2),round(db(math.sqrt(sq[c]/n)),2)) for c in range(ch) if db(math.sqrt(sq[c]/n))>-80]
    h=hashlib.sha256(open(p,'rb').read()).hexdigest()
    print(f"{label or p}: ch={ch} {sw*8}bit {sr}Hz {n/sr:.3f}s sha256={h[:16]}…")
    print("   active (ch, peak dBFS, rms dBFS):", act if act else "NONE — all silent")
    return act
for p in sys.argv[1:]:
    try: analyse(p)
    except Exception as e: print(f"{p}: ERROR {e}")
