# Independent esptool-style image builder + verifier (shares no code with fwpatch.js)
import hashlib, struct, sys, os, re
def blob_bytes():
    t=open(__import__("os").path.join(__import__("os").path.dirname(__file__),"..","..","src","intro_bitmaps.h"),encoding="utf-8").read()
    hdr=[int(x) for x in re.findall(r"\d+", re.search(r"// INTRO_SPLASH_LINES = \{(.*?)\};",t).group(1))]
    data=bytes(int(x,16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", re.search(r"// INTRO_SPLASH_BYTES\[\] = \{(.*?)\};",t).group(1)))
    b=bytearray(b"MANTIS_SPLASH_1\x00")+struct.pack("<HHHH",1,4608,3,0)
    for i in range(3): b+=struct.pack("<HHHH",*hdr[i*4:i*4+4])
    b+=data+bytes(4608-len(data)); return bytes(b)
def build(path):
    rnd=os.urandom
    segs=[(0x3C000020, rnd(5000)+blob_bytes()+rnd(3001)),   # rodata holding the blob
          (0x3FC88000, rnd(1200)),                          # dram
          (0x42000020, rnd(20007))]                         # irom/text
    out=bytearray([0xE9,len(segs),2,0x4F])+struct.pack("<I",0x40375000)
    out+=bytes([0xEE,0,0,0, 9,0, 0,0,0, 0,0, 0,0,0,0]) ; out[23:24]=b"\x01"   # hash_appended
    out=out[:24]; out[23]=1
    ck=0xEF
    for a,d in segs:
        out+=struct.pack("<II",a,len(d))+d
        for x in d: ck^=x
    while (len(out)+1)%16: out+=b"\x00"
    out.append(ck)
    out+=hashlib.sha256(bytes(out)).digest()
    open(path,"wb").write(out)
def verify(path):
    b=open(path,"rb").read()
    assert b[0]==0xE9; n=b[1]; off=24; ck=0xEF; segs=[]
    for _ in range(n):
        a,l=struct.unpack_from("<II",b,off); segs.append((off+8,l))
        for x in b[off+8:off+8+l]: ck^=x
        off+=8+l
    while (off+1)%16: off+=1
    ok_ck = b[off]==ck
    ok_sha = b[off+1:off+33]==hashlib.sha256(b[:off+1]).digest() if b[23]==1 else True
    return ok_ck, ok_sha
if sys.argv[1]=="build": build(sys.argv[2]); print("built", sys.argv[2])
else:
    c,s=verify(sys.argv[2]); print(f"  checksum {'OK' if c else 'BAD'}  sha256 {'OK' if s else 'BAD'}"); sys.exit(0 if c and s else 1)
