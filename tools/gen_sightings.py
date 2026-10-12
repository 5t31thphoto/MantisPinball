#!/usr/bin/env python3
"""gen_sightings.py - renders the Sightings pixel-art atlas + species table.
Outputs: data/content/sightings.sbi, src/sightings/sightings_data.h, sheet.png (preview).
Run from repo root:  python3 tools/gen_sightings.py
Requires: Pillow, numpy.
"""
import os
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_SBI   = os.path.join(_ROOT, "data", "content", "sightings.sbi")
OUT_HDR   = os.path.join(_ROOT, "src", "sightings", "sightings_data.h")
OUT_SHEET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sheet.png")
for _p in (OUT_SBI, OUT_HDR):
    os.makedirs(os.path.dirname(_p), exist_ok=True)

"""Shared pixel-art toolkit for Sightings icons.
24x24 icons, drawn on a grid with layered fills, dithering, outline pass.
Palette-quantized to <=15 colors + transparent per icon group.
"""
from PIL import Image, ImageDraw
import numpy as np

SZ = 24

def canvas():
    return Image.new("RGBA", (SZ, SZ), (0,0,0,0))

def px(d, x, y, c):
    if 0 <= x < SZ and 0 <= y < SZ:
        d.point((x,y), fill=c)

def hline(d, x0, x1, y, c):
    for x in range(min(x0,x1), max(x0,x1)+1): px(d,x,y,c)

def vline(d, x, y0, y1, c):
    for y in range(min(y0,y1), max(y0,y1)+1): px(d,x,y,c)

def blob(d, cx, cy, rx, ry, c):
    """filled ellipse"""
    for y in range(SZ):
        for x in range(SZ):
            dx=(x-cx)/max(rx,0.01); dy=(y-cy)/max(ry,0.01)
            if dx*dx+dy*dy <= 1.0: px(d,x,y,c)

def blob_shaded(d, cx, cy, rx, ry, base, lite, dark):
    """ellipse with top-left light, bottom-right shade"""
    for y in range(SZ):
        for x in range(SZ):
            dx=(x-cx)/max(rx,0.01); dy=(y-cy)/max(ry,0.01)
            r2=dx*dx+dy*dy
            if r2<=1.0:
                lx=(x-cx+2)/max(rx,0.01); ly=(y-cy+2)/max(ry,0.01)
                if lx*lx+ly*ly<=0.55: px(d,x,y,lite)
                elif r2>0.62 and (dx+dy)>0.25: px(d,x,y,dark)
                else: px(d,x,y,base)

def tri(d, p1, p2, p3, c):
    dd = ImageDraw.Draw(d._image) if hasattr(d,'_image') else d
    dd.polygon([p1,p2,p3], fill=c)

def poly(d, pts, c):
    d.polygon(pts, fill=c)

def line(d, p1, p2, c, w=1):
    d.line([p1,p2], fill=c, width=w)

def outline_sprite(img, col=(10,26,26,255)):
    """1px dark outline around every opaque region (classic sprite pass)."""
    a = np.array(img)
    alpha = a[:,:,3] > 0
    out = a.copy()
    H,W = alpha.shape
    for y in range(H):
        for x in range(W):
            if not alpha[y,x]:
                near = False
                for dy in (-1,0,1):
                    for dx in (-1,0,1):
                        ny,nx = y+dy, x+dx
                        if 0<=ny<H and 0<=nx<W and alpha[ny,nx]:
                            near=True; break
                    if near: break
                if near:
                    out[y,x] = col
    return Image.fromarray(out)

def sparkle(d, x, y, c):
    px(d,x,y,c); px(d,x-1,y,c); px(d,x+1,y,c); px(d,x,y-1,c); px(d,x,y+1,c)

# ---- palette helpers ---------------------------------------------------------
def quantize_group(imgs, max_colors=15):
    """Quantize a list of RGBA icons to one shared palette (<=max_colors + transparent).
    Returns (palette[list of rgb], indexed arrays[list of HxW uint8], 0=transparent)."""
    # gather all opaque pixels
    allpix = []
    for im in imgs:
        a = np.array(im)
        m = a[:,:,3] > 127
        allpix.append(a[m][:,:3])
    allc = np.concatenate(allpix) if allpix else np.zeros((0,3),np.uint8)
    if len(allc)==0:
        return [(0,0,0)], [np.zeros((SZ,SZ),np.uint8) for _ in imgs]
    # k-means-ish via PIL quantize on a strip image
    strip = Image.new("RGB", (len(allc),1))
    strip.putdata([tuple(c) for c in allc])
    q = strip.quantize(colors=max_colors, method=Image.MEDIANCUT)
    pal = q.getpalette()[:max_colors*3]
    pal = [(pal[i*3],pal[i*3+1],pal[i*3+2]) for i in range(max_colors)]
    # dedupe
    seen=[]; 
    for c in pal:
        if c not in seen: seen.append(c)
    pal = seen
    palarr = np.array(pal, dtype=np.int32)
    idxs=[]
    for im in imgs:
        a = np.array(im)
        m = a[:,:,3] > 127
        out = np.zeros((a.shape[0],a.shape[1]),np.uint8)
        if m.any():
            pixels = a[m][:,:3].astype(np.int32)
            d2 = ((pixels[:,None,:]-palarr[None,:,:])**2).sum(axis=2)
            out[m] = d2.argmin(axis=1)+1   # 0 reserved = transparent
        idxs.append(out)
    return pal, idxs

def rgb565(r,g,b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
from PIL import ImageDraw

def _d(im): return ImageDraw.Draw(im)

def raptor(body, head, wing, beak=(230,180,60,255), tail=None, crown=None):
    """Soaring raptor, 3/4 view, wings spread."""
    im = canvas(); d = _d(im)
    tail = tail or body
    # wings spread
    poly(d, [(1,10),(9,6),(12,9),(9,12)], wing)
    poly(d, [(22,10),(15,6),(12,9),(15,12)], wing)
    # wing feather notches
    for i,x in enumerate((2,4,6)): px(d,x,11+ (i%2), (0,0,0,0))
    # body
    blob(d, 12, 12, 3, 5, body)
    # tail fan
    poly(d, [(10,16),(14,16),(15,21),(9,21)], tail)
    vline(d, 12, 17, 20, (max(tail[0]-30,0),max(tail[1]-30,0),max(tail[2]-30,0),255))
    # head
    blob(d, 12, 6, 2.6, 2.6, head)
    if crown: hline(d, 10, 14, 4, crown)
    # beak
    poly(d, [(12,7),(14,8),(12,9)], beak)
    # eye
    px(d, 11, 6, (10,10,10,255))
    return im

def owl(body, face, ear=True):
    im = canvas(); d = _d(im)
    blob_shaded(d, 12, 14, 6, 7, body, tuple(min(c+25,255) for c in body[:3])+(255,), tuple(max(c-30,0) for c in body[:3])+(255,))
    # facial disc
    blob(d, 12, 8, 5.4, 4.6, face)
    # ear tufts
    if ear:
        poly(d, [(6,5),(8,2),(9,6)], body)
        poly(d, [(18,5),(16,2),(15,6)], body)
    # eyes
    blob(d, 9.5, 8, 1.6, 1.6, (247,214,80,255)); px(d,9,8,(10,10,10,255)); px(d,10,8,(10,10,10,255))
    blob(d, 14.5, 8, 1.6, 1.6, (247,214,80,255)); px(d,14,8,(10,10,10,255)); px(d,15,8,(10,10,10,255))
    # beak
    poly(d, [(11,10),(13,10),(12,12)], (90,70,50,255))
    # chest bars
    for y in range(14,20,2): hline(d, 9, 15, y, tuple(max(c-25,0) for c in body[:3])+(255,))
    # feet
    px(d,10,21,(120,90,40,255)); px(d,11,21,(120,90,40,255)); px(d,13,21,(120,90,40,255)); px(d,14,21,(120,90,40,255))
    return im

def songbird(body, breast, wing, beak=(60,55,50,255), crest=False, tailc=None, eye_ring=None):
    """Perched songbird facing right."""
    im = canvas(); d = _d(im)
    tailc = tailc or wing
    # tail
    poly(d, [(3,13),(8,12),(8,15),(2,16)], tailc)
    # body
    blob_shaded(d, 12, 13, 5.4, 4.6, body, tuple(min(c+20,255) for c in body[:3])+(255,), tuple(max(c-30,0) for c in body[:3])+(255,))
    # breast
    blob(d, 13.5, 15, 3.2, 3.0, breast)
    # wing
    poly(d, [(8,11),(13,10),(14,14),(9,16)], wing)
    line(d, (9,12),(13,12), tuple(max(c-35,0) for c in wing[:3])+(255,))
    # head
    blob(d, 16, 8, 3.2, 3.0, body)
    if crest:
        poly(d, [(15,5),(17,2),(18,6)], body)
    if eye_ring:
        blob(d, 17, 7.5, 1.4, 1.4, eye_ring)
    px(d, 17, 7, (10,10,10,255))
    # beak
    poly(d, [(19,8),(22,8),(19,9)], beak)
    # legs
    vline(d, 11, 17, 20, (90,70,40,255)); vline(d, 14, 17, 20, (90,70,40,255))
    hline(d, 9, 20, 21, (110,85,45,255))
    return im

def duck(body, head, bill=(240,170,60,255), wingpatch=None):
    im = canvas(); d = _d(im)
    # water line
    for x in range(1,23,3): hline(d,x,x+1,20,(60,170,190,255))
    # body
    blob_shaded(d, 11, 15, 7, 4, body, tuple(min(c+25,255) for c in body[:3])+(255,), tuple(max(c-30,0) for c in body[:3])+(255,))
    # tail
    poly(d, [(3,13),(6,12),(5,15)], body)
    if wingpatch:
        poly(d,[(9,13),(14,12),(13,16),(9,16)], wingpatch)
    # neck+head
    vline(d,16,8,13, head); vline(d,17,8,13, head)
    blob(d, 17, 7, 2.6, 2.4, head)
    px(d, 18, 6, (10,10,10,255))
    poly(d, [(19,7),(23,7),(19,9)], bill)
    return im

def swan(body, head=(250,250,250,255)):
    im = canvas(); d = _d(im)
    for x in range(1,23,3): hline(d,x,x+1,20,(60,170,190,255))
    blob_shaded(d, 11, 16, 7, 3.6, body, (255,255,255,255), (200,205,210,255))
    # S neck
    pts=[(16,15),(18,12),(18,8),(16,5)]
    for i in range(len(pts)-1): line(d, pts[i], pts[i+1], body, 2)
    blob(d, 16, 4.5, 2.0, 1.8, head)
    poly(d,[(18,4),(21,5),(18,6)], (30,30,30,255))
    px(d, 16, 4, (10,10,10,255))
    return im

def crane(body, crown=(210,60,60,255)):
    im = canvas(); d = _d(im)
    blob_shaded(d, 12, 11, 5.4, 3.4, body, tuple(min(c+25,255) for c in body[:3])+(255,), tuple(max(c-25,0) for c in body[:3])+(255,))
    poly(d,[(5,9),(9,8),(8,12)], body)  # tail bustle
    vline(d,16,5,9, body)
    blob(d,16,4,1.8,1.6, body)
    hline(d,15,17,3, crown)
    poly(d,[(18,4),(22,4),(18,5)], (70,60,50,255))
    px(d,16,4,(10,10,10,255))
    vline(d, 10, 14, 21, (90,80,60,255)); vline(d, 14, 14, 21, (90,80,60,255))
    return im

def woodpecker(body, head_red, back, belly=(235,230,220,255)):
    """Clinging to trunk, side view."""
    im = canvas(); d = _d(im)
    # trunk
    for y in range(0,24):
        vline(d, 3, 0, 23, (110,80,55,255)); vline(d, 4, 0, 23, (95,68,46,255)); vline(d,5,0,23,(110,80,55,255))
    px(d,4,6,(80,55,38,255)); px(d,4,14,(80,55,38,255)); px(d,3,10,(80,55,38,255))
    # tail brace
    poly(d, [(7,18),(10,16),(10,20)], back)
    # body
    blob_shaded(d, 12, 13, 4.4, 5.0, back, tuple(min(c+18,255) for c in back[:3])+(255,), tuple(max(c-25,0) for c in back[:3])+(255,))
    blob(d, 13.5, 14.5, 2.6, 3.4, belly)
    # head
    blob(d, 14, 6.5, 3.0, 2.8, body)
    hline(d, 12, 16, 4, head_red); hline(d, 12, 16, 5, head_red)
    px(d, 15, 6, (10,10,10,255))
    poly(d, [(17,7),(21,7),(17,8)], (60,60,60,255))
    # feet
    px(d, 8, 12, (90,70,40,255)); px(d, 8, 13, (90,70,40,255))
    return im

def hummingbird(body, throat, wing):
    im = canvas(); d = _d(im)
    # blur wings
    poly(d, [(6,4),(11,8),(6,10)], wing)
    poly(d, [(17,3),(13,8),(18,9)], wing)
    # body angled
    blob_shaded(d, 12, 12, 3.0, 4.4, body, tuple(min(c+30,255) for c in body[:3])+(255,), tuple(max(c-25,0) for c in body[:3])+(255,))
    blob(d, 12.5, 9, 2.4, 2.2, body)
    blob(d, 13, 11, 1.8, 1.4, throat)
    px(d, 13, 8, (10,10,10,255))
    line(d, (15,8),(21,6),(40,40,40,255))
    # tail
    poly(d, [(10,16),(13,16),(11,21)], tuple(max(c-25,0) for c in body[:3])+(255,))
    # flower
    blob(d, 20, 13, 2.0, 2.0, (240,110,150,255)); px(d,20,13,(255,220,120,255))
    vline(d, 20, 15, 20, (70,150,70,255))
    return im

def magpie():
    im = canvas(); d = _d(im)
    poly(d,[(1,9),(9,11),(9,14),(1,12)], (40,90,120,255))   # long tail iridescent
    blob_shaded(d, 13, 12, 4.6, 3.8, (30,32,36,255), (60,64,70,255), (16,17,20,255))
    blob(d, 14, 14.5, 2.8, 2.0, (240,240,240,255))          # white belly
    poly(d,[(10,10),(15,9),(15,13),(11,13)], (250,250,250,255))  # wing patch
    blob(d, 17, 8, 2.8, 2.6, (30,32,36,255))
    px(d, 18, 7, (240,240,240,255))
    poly(d,[(19,8),(22,9),(19,10)], (50,50,55,255))
    vline(d,12,16,20,(70,70,70,255)); vline(d,15,16,20,(70,70,70,255))
    return im

def raven():
    im = canvas(); d = _d(im)
    blob_shaded(d, 11, 13, 6.4, 4.6, (28,30,36,255), (62,68,84,255), (12,13,18,255))
    poly(d, [(2,12),(6,11),(6,15),(2,16)], (28,30,36,255))
    blob(d, 17, 8, 3.2, 3.0, (28,30,36,255))
    px(d, 18, 7, (200,200,210,255))
    poly(d, [(20,8),(23,9),(20,11)], (40,42,48,255))   # heavy bill
    px(d,20,9,(60,64,74,255))
    vline(d, 10, 17, 21, (40,40,44,255)); vline(d, 14, 17, 21, (40,40,44,255))
    # shaggy throat
    px(d,15,11,(50,54,66,255)); px(d,16,12,(50,54,66,255))
    return im

def vulture():
    im = canvas(); d = _d(im)
    # V-hold soaring silhouette
    poly(d, [(1,13),(9,6),(11,10),(4,15)], (40,36,34,255))
    poly(d, [(22,13),(14,6),(12,10),(19,15)], (40,36,34,255))
    # silver flight feathers
    line(d,(3,12),(9,8),(150,150,155,255)); line(d,(20,12),(14,8),(150,150,155,255))
    blob(d, 11.5, 11, 2.6, 3.6, (40,36,34,255))
    blob(d, 11.5, 6.5, 1.6, 1.6, (200,70,60,255))  # red head
    px(d, 12, 6, (10,10,10,255))
    poly(d,[(10,16),(13,16),(14,20),(9,20)], (40,36,34,255))
    return im

def grouse(body, chest=None, fan=False, fanc=None):
    im = canvas(); d = _d(im)
    if fan:
        fanc = fanc or (140,100,70,255)
        for a in range(-4,5):
            line(d,(12,12),(12+a*2, 3+abs(a)),(fanc if a%2==0 else tuple(max(c-30,0) for c in fanc[:3])+(255,)),2)
    blob_shaded(d, 12, 15, 6, 5, body, tuple(min(c+20,255) for c in body[:3])+(255,), tuple(max(c-28,0) for c in body[:3])+(255,))
    if chest: blob(d, 12, 16, 3.2, 3.0, chest)
    blob(d, 16, 9.5, 2.4, 2.2, body)
    px(d, 17, 9, (10,10,10,255))
    poly(d,[(18,10),(21,10),(18,11)], (90,80,60,255))
    vline(d,10,20,22,(110,85,45,255)); vline(d,14,20,22,(110,85,45,255))
    return im

def shorebird(body, belly, legs=(230,190,80,255), bill=(40,40,40,255), long_bill=True, band=None):
    im = canvas(); d = _d(im)
    # sand
    hline(d,1,22,21,(220,200,150,255)); px(d,4,22,(200,180,130,255)); px(d,15,22,(200,180,130,255))
    blob_shaded(d, 11, 13, 5, 3.6, body, tuple(min(c+22,255) for c in body[:3])+(255,), tuple(max(c-25,0) for c in body[:3])+(255,))
    blob(d, 12, 15, 3.4, 2.2, belly)
    if band: hline(d, 8, 15, 12, band)
    blob(d, 15.5, 8.5, 2.4, 2.2, body)
    px(d, 16, 8, (10,10,10,255))
    if long_bill: line(d,(18,9),(23,10), bill)
    else: poly(d,[(17,8),(20,9),(17,10)], bill)
    vline(d, 10, 16, 20, legs); vline(d, 13, 16, 20, legs)
    return im

def pelican():
    im = canvas(); d = _d(im)
    for x in range(1,23,3): hline(d,x,x+1,21,(60,170,190,255))
    blob_shaded(d, 10, 16, 6.4, 3.6, (120,110,105,255), (160,152,148,255), (90,82,78,255))
    vline(d,15,9,14,(230,225,220,255)); vline(d,16,9,14,(230,225,220,255))
    blob(d, 16, 8, 2.4, 2.2, (240,235,230,255))
    px(d, 17, 7, (10,10,10,255))
    # huge bill + pouch
    poly(d,[(18,8),(23,9),(18,10)], (240,170,80,255))
    poly(d,[(18,10),(22,10),(19,14)], (250,200,120,255))
    return im

def dipper():
    im = canvas(); d = _d(im)
    # stream boulder + water
    blob(d, 12, 20, 7, 3, (120,120,125,255))
    for x in range(0,24,4): hline(d,x,x+2,22,(70,180,200,255))
    blob_shaded(d, 12, 13, 4.2, 3.6, (72,70,74,255), (105,102,108,255), (48,46,50,255))
    blob(d, 15, 9.5, 2.4, 2.2, (80,70,66,255))
    px(d, 16, 9, (240,240,240,255))
    poly(d,[(17,10),(20,10),(17,11)], (50,50,50,255))
    poly(d,[(7,12),(10,11),(10,14)], (60,58,62,255))
    vline(d,11,16,18,(200,190,170,255)); vline(d,14,16,18,(200,190,170,255))
    return im

def swallow():
    im = canvas(); d = _d(im)
    # streamer-tailed flight
    poly(d, [(2,4),(9,9),(6,11)], (40,110,90,255))     # far wing
    blob(d, 12, 11, 4.0, 2.8, (46,140,110,255))
    blob(d, 12, 13, 3.0, 1.8, (245,245,245,255))       # white rump/belly
    poly(d, [(14,8),(22,3),(16,12)], (40,110,90,255))  # near wing
    blob(d, 8.5, 9, 2.2, 2.0, (46,140,110,255))
    px(d, 8, 8, (10,10,10,255))
    poly(d,[(6,9),(4,9),(6,10)], (60,55,50,255))
    poly(d, [(15,14),(21,20),(17,14)], (40,110,90,255))
    poly(d, [(13,14),(9,21),(12,14)], (40,110,90,255))
    return im

def kestrel_hover():
    im = canvas(); d = _d(im)
    poly(d,[(2,7),(10,4),(11,8),(4,11)], (150,90,60,255))
    poly(d,[(21,7),(13,4),(12,8),(19,11)], (80,110,150,255))
    blob(d, 12, 11, 2.8, 4.2, (210,160,110,255))
    for y in (9,11,13): px(d, 11, y, (120,80,50,255)); px(d,13,y,(120,80,50,255))
    blob(d, 12, 5.5, 2.2, 2.0, (90,120,160,255))
    px(d, 11, 5, (10,10,10,255))
    vline(d, 11, 4, 7, (60,60,60,255))  # malar stripe
    poly(d,[(10,15),(14,15),(15,21),(9,21)], (170,110,70,255))
    hline(d, 9, 15, 20, (40,40,40,255))  # tail band
    return im

def _d(im): return ImageDraw.Draw(im)

def bear(fur, hump=True, muzzle=(200,170,130,255)):
    im = canvas(); d = _d(im)
    lite = tuple(min(c+22,255) for c in fur[:3])+(255,)
    dark = tuple(max(c-28,0) for c in fur[:3])+(255,)
    blob_shaded(d, 11, 14, 7.4, 5.6, fur, lite, dark)
    if hump: blob(d, 8, 9.5, 3.4, 2.6, fur)
    blob(d, 17, 9, 3.4, 3.0, fur)
    blob(d, 19.5, 10.5, 1.8, 1.4, muzzle)
    px(d, 21, 10, (20,20,20,255)); px(d, 17, 8, (10,10,10,255))
    blob(d, 15.5, 6, 1.2, 1.2, fur); blob(d, 18.5, 5.6, 1.2, 1.2, fur)  # ears
    for x in (6,9,13,16): vline(d, x, 18, 21, dark)
    px(d,5,21,dark); px(d,6,22,dark)
    return im

def canid(fur, belly, tail_tip=None, ears_big=False, howl=False):
    im = canvas(); d = _d(im)
    lite = tuple(min(c+22,255) for c in fur[:3])+(255,)
    dark = tuple(max(c-30,0) for c in fur[:3])+(255,)
    blob_shaded(d, 11, 14, 6.4, 4.2, fur, lite, dark)
    blob(d, 12, 16, 4.0, 2.4, belly)
    # bushy tail
    poly(d, [(3,10),(7,12),(6,16),(2,14)], fur)
    if tail_tip: blob(d, 3, 11, 1.6, 1.6, tail_tip)
    if howl:
        # head thrown back
        blob(d, 16, 8, 2.8, 2.6, fur)
        poly(d, [(17,6),(21,3),(18,8)], fur)      # muzzle up
        poly(d, [(14,6),(13,3),(16,6)], fur)      # ear
    else:
        blob(d, 17, 9.5, 3.0, 2.6, fur)
        poly(d, [(19,9),(23,10),(19,11)], fur)
        px(d, 22, 10, (15,15,15,255))
        e = 3 if ears_big else 2
        poly(d, [(15,7),(14,7-e),(17,7)], fur)
        poly(d, [(17,7),(18,7-e),(19,8)], fur)
        px(d, 18, 8, (10,10,10,255))
    for x in (7,10,13,16): vline(d, x, 17, 21, dark)
    return im

def felid(fur, spots=None, tail_long=True, tufts=False, muzzle=(235,225,215,255)):
    im = canvas(); d = _d(im)
    lite = tuple(min(c+20,255) for c in fur[:3])+(255,)
    dark = tuple(max(c-28,0) for c in fur[:3])+(255,)
    blob_shaded(d, 11, 14, 6.6, 4.0, fur, lite, dark)
    if tail_long:
        pts=[(4,13),(2,10),(3,7)]
        line(d,(5,14),pts[0],fur,2); line(d,pts[0],pts[1],fur,2); line(d,pts[1],pts[2],fur,2)
        px(d,3,6,dark)
    else:
        poly(d,[(4,12),(6,11),(6,14)], fur)  # bob
    blob(d, 17, 9, 3.0, 2.8, fur)
    blob(d, 18.5, 10.5, 1.6, 1.2, muzzle)
    px(d, 17.5, 8, (10,10,10,255)); px(d, 20, 10, (200,120,120,255))
    poly(d, [(15,7),(14,4),(17,7)], fur); poly(d, [(19,7),(20,4),(17,7)], fur)
    if tufts: px(d,14,3,(20,20,20,255)); px(d,20,3,(20,20,20,255))
    if spots:
        for sx,sy in ((8,12),(11,11),(13,14),(9,16),(14,12)): px(d,sx,sy,spots)
    for x in (7,10,13,16): vline(d, x, 17, 21, dark)
    return im

def mustelid(fur, belly=None, long_body=True, water=False):
    im = canvas(); d = _d(im)
    lite = tuple(min(c+22,255) for c in fur[:3])+(255,)
    dark = tuple(max(c-28,0) for c in fur[:3])+(255,)
    if water:
        for x in range(0,24,4): hline(d,x,x+2,20,(60,170,190,255))
    # sinuous body
    blob(d, 8, 15, 4.6, 3.0, fur)
    blob(d, 13, 13, 4.4, 2.8, fur)
    blob(d, 17, 10.5, 3.0, 2.4, fur)
    blob(d, 10, 13, 3.5, 2.0, lite)
    if belly: blob(d, 16, 12, 2.0, 1.4, belly)
    poly(d, [(2,17),(5,15),(5,19)], fur)   # tail
    px(d, 18, 9.5, (10,10,10,255))
    blob(d, 19.5, 11.5, 1.2, 1.0, dark)
    px(d, 16, 8, fur[0:3]+(255,)); px(d, 18, 8, fur[0:3]+(255,))
    for x in (7,11,14): vline(d, x, 17, 19, dark)
    return im

def beaver():
    im = canvas(); d = _d(im)
    for x in range(0,24,4): hline(d,x,x+2,21,(60,170,190,255))
    blob_shaded(d, 11, 14, 6.0, 4.6, (120,80,50,255), (150,105,70,255), (92,60,36,255))
    # flat tail (crosshatch)
    poly(d, [(2,16),(7,14),(7,19),(2,20)], (70,55,45,255))
    px(d,4,16,(55,42,34,255)); px(d,5,18,(55,42,34,255)); px(d,3,18,(55,42,34,255))
    blob(d, 16.5, 9.5, 3.0, 2.8, (120,80,50,255))
    px(d, 17, 8, (10,10,10,255))
    blob(d, 18.5, 11, 1.6, 1.3, (150,105,70,255))
    hline(d, 18, 19, 12, (240,200,90,255))  # orange teeth
    px(d, 14.5, 7, (100,66,42,255))
    # gnawed log
    hline(d, 15, 22, 20, (140,100,60,255)); poly(d,[(21,18),(23,20),(21,21)],(180,150,100,255))
    return im

def ungulate(fur, antlers=None, antler_big=False, white_rump=False, horn=None, shaggy=False, bell=False):
    """Deer/elk/moose/pronghorn/sheep/goat base: standing side profile."""
    im = canvas(); d = _d(im)
    lite = tuple(min(c+22,255) for c in fur[:3])+(255,)
    dark = tuple(max(c-30,0) for c in fur[:3])+(255,)
    blob_shaded(d, 11, 12, 6.0, 3.6, fur, lite, dark)
    if shaggy:
        for x in range(6,16,2): px(d, x, 15, dark)
    if white_rump: blob(d, 5.6, 11.5, 1.8, 2.2, (245,245,240,255))
    # neck+head
    poly(d, [(15,10),(18,6),(20,7),(16,13)], fur)
    blob(d, 19, 6, 2.2, 1.9, fur)
    px(d, 19, 5, (10,10,10,255))
    px(d, 21.5, 6.5, (25,25,25,255))
    if bell: vline(d, 19, 8, 11, dark)
    # legs
    for x in (7,9,13,15): vline(d, x, 15, 21, fur); px(d, x, 21, (40,35,30,255))
    # headgear
    if antlers == 'deer':
        line(d,(18,4),(16,1),(150,120,90,255)); line(d,(17,3),(15,3),(150,120,90,255))
        line(d,(20,4),(22,1),(150,120,90,255)); line(d,(21,3),(23,3),(150,120,90,255))
    elif antlers == 'elk':
        line(d,(18,4),(14,0),(160,130,95,255)); line(d,(16,2),(13,3),(160,130,95,255)); line(d,(17,3),(15,0),(160,130,95,255))
        line(d,(20,4),(23,0),(160,130,95,255)); line(d,(21,2),(23,3),(160,130,95,255))
    elif antlers == 'moose':
        poly(d,[(14,3),(18,1),(18,4)], (170,140,100,255))
        poly(d,[(23,3),(20,1),(20,4)], (170,140,100,255))
        px(d,15,1,(170,140,100,255)); px(d,22,1,(170,140,100,255))
    if horn == 'curl':
        # bighorn curl
        for t in range(10):
            import math
            a = t*0.62
            px(d, int(19+2.8*math.cos(a)), int(4+2.8*math.sin(a)), (190,160,110,255))
    elif horn == 'spike':
        line(d,(18,4),(17,1),(50,45,40,255)); line(d,(20,4),(21,1),(50,45,40,255))
    elif horn == 'prong':
        line(d,(18,4),(17,1),(60,50,40,255)); px(d,16,2,(60,50,40,255))
        line(d,(20,4),(21,1),(60,50,40,255)); px(d,22,2,(60,50,40,255))
    return im

def rodent_small(fur, stripe=None, cheeks=False, standing=True):
    im = canvas(); d = _d(im)
    lite = tuple(min(c+25,255) for c in fur[:3])+(255,)
    dark = tuple(max(c-25,0) for c in fur[:3])+(255,)
    if standing:
        blob_shaded(d, 12, 15, 3.8, 5.2, fur, lite, dark)
        blob(d, 12.5, 16, 2.4, 3.2, lite)
        blob(d, 12, 8, 3.0, 2.8, fur)
        px(d, 13.5, 7.5, (10,10,10,255))
        px(d, 10, 5.5, fur[0:3]+(255,)); px(d, 14, 5.5, fur[0:3]+(255,))
        if stripe: vline(d, 12, 12, 19, stripe)
        if cheeks: px(d, 15, 9, lite); px(d, 15, 10, lite)
        # tail curl
        line(d,(15,19),(19,17),fur,2); line(d,(19,17),(19,12),fur,2)
        hline(d, 9, 15, 21, dark)
    return im

def marmot():
    im = canvas(); d = _d(im)
    # rock
    blob(d, 12, 20, 8, 3, (130,128,132,255))
    blob_shaded(d, 12, 13, 5.0, 5.4, (150,110,70,255), (185,140,95,255), (115,82,52,255))
    blob(d, 12.5, 15, 3.2, 3.4, (200,160,100,255))
    blob(d, 12, 7, 3.2, 2.8, (150,110,70,255))
    px(d, 13.5, 6.5, (10,10,10,255))
    px(d, 10, 4.5, (150,110,70,255)); px(d, 14, 4.5, (150,110,70,255))
    px(d, 12.5, 8.5, (60,45,35,255))
    return im

def pika():
    im = canvas(); d = _d(im)
    blob(d, 12, 20, 8, 3, (130,128,132,255))
    blob_shaded(d, 12, 14, 4.6, 4.0, (165,135,105,255), (195,165,130,255), (130,102,76,255))
    blob(d, 12, 9, 3.4, 3.0, (165,135,105,255))
    blob(d, 9.5, 6, 1.6, 2.0, (165,135,105,255)); blob(d, 14.5, 6, 1.6, 2.0, (165,135,105,255))
    px(d, 9.5, 6, (120,95,70,255)); px(d, 14.5, 6, (120,95,70,255))
    px(d, 11, 9, (10,10,10,255)); px(d, 13.5, 9, (10,10,10,255))
    px(d, 12.5, 10.5, (60,45,35,255))
    # hay sprig in mouth
    line(d,(13,11),(18,9),(120,180,80,255)); px(d,18,8,(150,200,90,255)); px(d,19,9,(150,200,90,255))
    return im

def seal(fur, spots=True, elephant=False):
    im = canvas(); d = _d(im)
    hline(d,0,23,20,(220,200,150,255)); hline(d,0,23,21,(210,190,140,255))
    blob_shaded(d, 11, 15, 8.0, 4.0, fur, tuple(min(c+25,255) for c in fur[:3])+(255,), tuple(max(c-25,0) for c in fur[:3])+(255,))
    blob(d, 18, 11, 3.0, 2.8, fur)
    if elephant:
        blob(d, 20.5, 12.5, 1.8, 2.0, tuple(max(c-20,0) for c in fur[:3])+(255,))
    px(d, 19, 10, (10,10,10,255))
    px(d, 21, 12, (25,25,25,255))
    if spots:
        for sx,sy in ((7,13),(10,15),(13,13),(9,17),(14,16)): px(d,sx,sy,tuple(max(c-35,0) for c in fur[:3])+(255,))
    poly(d, [(2,15),(5,13),(5,18)], fur)
    # whiskers
    line(d,(20,13),(23,12),(220,220,220,255))
    return im

def otter_sea():
    im = canvas(); d = _d(im)
    for x in range(0,24,4): hline(d,x,x+2,18,(60,170,190,255))
    for x in range(2,24,4): hline(d,x,x+2,21,(50,155,175,255))
    # floating on back
    blob_shaded(d, 11, 15, 7.0, 3.0, (95,70,50,255), (125,95,68,255), (72,52,36,255))
    blob(d, 18, 12, 2.8, 2.6, (110,82,58,255))
    blob(d, 18, 13, 1.8, 1.4, (200,180,160,255))
    px(d, 19, 11, (10,10,10,255))
    # paws holding rock
    px(d, 12, 12, (110,82,58,255)); px(d, 14, 12, (110,82,58,255))
    blob(d, 13, 12.5, 1.2, 1.0, (140,140,145,255))
    poly(d, [(3,16),(6,14),(6,18)], (95,70,50,255))
    return im

def whale_tail(fluke=(60,70,84,255), gray=False):
    im = canvas(); d = _d(im)
    for x in range(0,24,3): hline(d,x,x+1,19,(60,170,190,255))
    for x in range(1,24,3): hline(d,x,x+1,21,(50,155,175,255))
    c = (130,132,138,255) if gray else fluke
    poly(d, [(12,18),(4,10),(8,9),(12,14)], c)
    poly(d, [(12,18),(20,10),(16,9),(12,14)], c)
    lite = tuple(min(x+30,255) for x in c[:3])+(255,)
    line(d,(6,11),(11,15), lite); line(d,(18,11),(13,15), lite)
    if gray:
        px(d,7,10,(200,200,200,255)); px(d,16,11,(200,200,200,255))  # barnacle mottle
    # splash
    px(d,3,17,(230,245,250,255)); px(d,21,17,(230,245,250,255)); px(d,12,19,(230,245,250,255))
    return im

def orca():
    im = canvas(); d = _d(im)
    for x in range(0,24,3): hline(d,x,x+1,20,(60,170,190,255))
    blob_shaded(d, 11, 14, 8.2, 4.2, (22,24,30,255), (52,56,66,255), (12,13,16,255))
    poly(d, [(11,10),(14,2),(15,10)], (22,24,30,255))   # tall dorsal
    blob(d, 16, 15.5, 3.0, 1.8, (240,240,245,255))       # belly
    blob(d, 17.5, 12, 1.6, 1.1, (240,240,245,255))       # eye patch
    px(d, 19, 13.5, (10,10,10,255))
    poly(d, [(2,13),(5,11),(5,16)], (22,24,30,255))
    return im

def dolphin():
    im = canvas(); d = _d(im)
    for x in range(0,24,3): hline(d,x,x+1,20,(60,170,190,255))
    # arcing leap
    blob(d, 12, 11, 7.5, 3.2, (110,130,150,255))
    blob(d, 12, 12.5, 6.0, 2.0, (190,200,210,255))
    poly(d, [(11,8),(13,4),(14,9)], (110,130,150,255))
    poly(d, [(4,10),(2,7),(6,9)], (110,130,150,255))
    blob(d, 18, 10.5, 2.2, 1.8, (110,130,150,255))
    line(d,(20,11),(23,12),(90,108,128,255),2)
    px(d, 19, 10, (10,10,10,255))
    px(d,6,18,(230,245,250,255)); px(d,9,19,(230,245,250,255))
    return im
import math

def _d(im): return ImageDraw.Draw(im)

def troutform(body, belly, spots=None, slash=None, stripe=None, jaw_hook=False, sail=False, forked=False):
    """Side-profile salmonid."""
    im = canvas(); d = _d(im)
    lite = tuple(min(c+22,255) for c in body[:3])+(255,)
    dark = tuple(max(c-30,0) for c in body[:3])+(255,)
    # body
    blob_shaded(d, 11, 12, 7.6, 3.4, body, lite, dark)
    blob(d, 11, 13.5, 6.4, 1.8, belly)
    # tail
    if forked:
        poly(d, [(3,12),(0,8),(2,12),(0,16)], body)
        poly(d, [(4,10),(1,8),(3,12)], body); poly(d, [(4,14),(1,16),(3,12)], body)
    else:
        poly(d, [(4,9),(1,10),(1,14),(4,15)], body)
    # dorsal
    if sail:
        poly(d, [(8,9),(9,3),(13,4),(14,9)], tuple(min(c+15,255) for c in body[:3])+(255,))
        for x in (9,11,13): vline(d, x, 5, 8, dark)
    else:
        poly(d, [(9,9),(11,6),(13,9)], dark)
    # fins
    poly(d, [(10,15),(12,18),(13,15)], dark)
    # head/eye/mouth
    px(d, 16.5, 11, (10,10,10,255))
    if jaw_hook:
        line(d,(19,13),(21,11), dark)
    else:
        px(d, 19, 12.5, dark)
    if stripe: 
        for x in range(6,17): px(d, x, 12, stripe)
        for x in range(7,16): px(d, x, 11, tuple(min(stripe[0]+20,255),)+stripe[1:3]+(255,)) if False else None
    if slash:
        line(d, (16,13),(18,14), slash, 2)
    if spots:
        for sx,sy in ((7,10),(10,9),(13,10),(8,13),(12,13),(15,10),(6,12)):
            px(d, sx, sy, spots)
    return im

def pike_form(body, belly, spots=None, teeth=True):
    im = canvas(); d = _d(im)
    dark = tuple(max(c-30,0) for c in body[:3])+(255,)
    blob_shaded(d, 11, 12, 8.4, 2.8, body, tuple(min(c+22,255) for c in body[:3])+(255,), dark)
    blob(d, 11, 13.4, 7.0, 1.4, belly)
    poly(d, [(3,9),(0,9),(0,15),(3,15)], body)
    poly(d, [(6,10),(8,8),(9,10)], dark)   # dorsal set far back
    # long jaw
    poly(d, [(17,11),(23,11),(17,13)], body)
    px(d, 17, 10.5, (10,10,10,255))
    if teeth: px(d, 20, 12, (240,240,240,255)); px(d, 22, 12, (240,240,240,255))
    if spots:
        for sx,sy in ((6,11),(9,10),(12,11),(8,13),(13,13),(15,11)): px(d,sx,sy,spots)
    return im

def bass_form(body, belly, stripe=None, red_eye=False):
    im = canvas(); d = _d(im)
    dark = tuple(max(c-30,0) for c in body[:3])+(255,)
    blob_shaded(d, 11, 12, 7.0, 4.2, body, tuple(min(c+22,255) for c in body[:3])+(255,), dark)
    blob(d, 11, 14, 5.6, 2.0, belly)
    poly(d, [(4,9),(1,8),(1,16),(4,15)], body)
    # spiny dorsal
    for i,x in enumerate(range(7,15,2)): line(d,(x,8),(x+1,5), dark)
    poly(d, [(9,16),(11,19),(12,16)], dark)
    px(d, 16.5, 10.5, (200,60,50,255) if red_eye else (10,10,10,255))
    # big mouth
    line(d,(18,12),(21,13), dark, 2)
    if stripe:
        for x in range(5,18): px(d, x, 12, stripe)
    return im

def catfish():
    im = canvas(); d = _d(im)
    blob_shaded(d, 11, 13, 7.6, 3.6, (100,95,88,255), (130,124,116,255), (74,70,64,255))
    blob(d, 11, 14.6, 6.2, 1.8, (200,190,170,255))
    poly(d, [(3,10),(0,10),(0,16),(3,16)], (100,95,88,255))
    poly(d, [(8,10),(10,7),(11,10)], (74,70,64,255))
    px(d, 16, 12, (10,10,10,255))
    # barbels
    line(d,(19,14),(23,12),(70,66,60,255)); line(d,(19,14),(23,15),(70,66,60,255)); line(d,(18,15),(21,17),(70,66,60,255))
    return im

def sturgeon():
    im = canvas(); d = _d(im)
    blob_shaded(d, 11, 13, 8.6, 2.8, (110,105,112,255), (140,136,144,255), (80,76,84,255))
    # scutes
    for x in range(5,18,3): px(d, x, 11, (170,168,176,255))
    for x in range(6,18,3): px(d, x, 14, (150,148,156,255))
    # pointed snout + barbels
    poly(d, [(17,12),(23,13),(17,14)], (110,105,112,255))
    line(d,(19,15),(20,17),(90,86,92,255)); line(d,(21,15),(22,17),(90,86,92,255))
    px(d, 16, 12, (10,10,10,255))
    poly(d, [(3,10),(0,8),(2,13),(0,16)], (110,105,112,255))  # shark-like tail
    poly(d, [(8,10),(10,7),(12,10)], (80,76,84,255))
    return im

def perch_form(body, bars, belly):
    im = canvas(); d = _d(im)
    dark = tuple(max(c-30,0) for c in body[:3])+(255,)
    blob_shaded(d, 11, 12, 6.6, 4.0, body, tuple(min(c+22,255) for c in body[:3])+(255,), dark)
    blob(d, 11, 14, 5.2, 1.8, belly)
    poly(d, [(4,9),(1,9),(1,15),(4,15)], (230,140,60,255))
    for i,x in enumerate(range(7,15,2)): line(d,(x,8),(x+1,5), dark)
    for x in (7,10,13): vline(d, x, 9, 15, bars)
    px(d, 16, 11, (10,10,10,255))
    poly(d, [(9,16),(10,19),(11,16)], (230,140,60,255))
    return im

def crab(shell, claws_big=True):
    im = canvas(); d = _d(im)
    lite = tuple(min(c+25,255) for c in shell[:3])+(255,)
    dark = tuple(max(c-30,0) for c in shell[:3])+(255,)
    blob_shaded(d, 12, 13, 6.0, 4.2, shell, lite, dark)
    # legs
    for i,(sx,sy,ex,ey) in enumerate([(6,12,2,8),(6,14,1,12),(7,16,2,18),(18,12,22,8),(18,14,23,12),(17,16,22,18)]):
        line(d,(sx,sy),(ex,ey), dark)
    # claws
    if claws_big:
        blob(d, 5, 8, 2.2, 1.8, shell); px(d, 3.5, 7, dark)
        blob(d, 19, 8, 2.2, 1.8, shell); px(d, 20.5, 7, dark)
    px(d, 10, 10, (10,10,10,255)); px(d, 14, 10, (10,10,10,255))
    px(d, 10, 9, lite); px(d, 14, 9, lite)
    return im

def seastar(col, points=5, knobby=False):
    im = canvas(); d = _d(im)
    cx, cy, R, r = 12, 12, 10, 4
    pts=[]
    for i in range(points*2):
        a = math.pi*i/points - math.pi/2
        rad = R if i%2==0 else r
        pts.append((cx+rad*math.cos(a), cy+rad*math.sin(a)))
    poly(d, pts, col)
    lite = tuple(min(c+30,255) for c in col[:3])+(255,)
    if knobby:
        for i in range(points):
            a = 2*math.pi*i/points - math.pi/2
            px(d, int(cx+5*math.cos(a)), int(cy+5*math.sin(a)), lite)
            px(d, int(cx+8*math.cos(a)), int(cy+8*math.sin(a)), lite)
    blob(d, cx, cy, 1.6, 1.6, lite)
    return im

def urchin():
    im = canvas(); d = _d(im)
    blob(d, 12, 13, 5.0, 4.6, (90,40,110,255))
    for i in range(16):
        a = 2*math.pi*i/16
        x2 = 12+9*math.cos(a); y2 = 13+8.4*math.sin(a)
        line(d,(int(12+4*math.cos(a)),int(13+3.8*math.sin(a))),(int(x2),int(y2)),(130,70,150,255))
    blob(d, 11, 11.5, 1.4, 1.2, (160,100,180,255))
    return im

def anemone(col, tent):
    im = canvas(); d = _d(im)
    # rock base
    blob(d, 12, 20, 7, 3, (110,108,114,255))
    blob(d, 12, 16, 4.6, 3.2, col)
    for i in range(11):
        a = math.pi*(0.08+0.84*i/10)
        x2 = 12+8.5*math.cos(a); y2 = 14-8.0*math.sin(a)
        line(d,(int(12+3*math.cos(a)),int(14-2.6*math.sin(a))),(int(x2),int(y2)), tent, 2)
        px(d,int(x2),int(y2), tuple(min(c+35,255) for c in tent[:3])+(255,))
    return im

def shell_limpet():
    im = canvas(); d = _d(im)
    blob(d, 12, 20, 8, 2.6, (110,108,114,255))
    poly(d, [(4,18),(12,6),(20,18)], (200,170,130,255))
    for i in range(1,5):
        line(d,(12,6),(4+i*4,18),(170,140,100,255))
    px(d,12,7,(230,210,170,255))
    return im

def chiton(giant=False):
    im = canvas(); d = _d(im)
    blob(d, 12, 20, 8, 2.6, (110,108,114,255))
    c = (140,60,50,255) if giant else (80,90,80,255)
    blob(d, 12, 15, 7.0 if giant else 6.0, 3.6, c)
    lp = tuple(min(x+30,255) for x in c[:3])+(255,)
    if giant:
        blob(d, 12, 15, 7.0, 3.6, c)  # gumboot hides plates
        px(d,9,14,lp); px(d,13,15,lp); px(d,15,14,lp)
    else:
        for i in range(6):
            vline(d, 7+i*2, 12, 18, lp if i%2==0 else c)
    return im

def snail_moon():
    im = canvas(); d = _d(im)
    hline(d,1,22,20,(220,200,150,255))
    blob_shaded(d, 12, 12, 6.0, 5.4, (170,150,120,255), (205,185,150,255), (135,115,88,255))
    # spiral
    for t in range(20):
        a = t*0.5; r = 1+t*0.24
        px(d, int(12+r*math.cos(a)), int(12+r*math.sin(a)*0.9), (120,100,75,255))
    blob(d, 13, 18.5, 6, 1.8, (200,180,150,255))
    px(d, 19, 17.5, (10,10,10,255))
    return im

def nudibranch(body, tips):
    im = canvas(); d = _d(im)
    blob_shaded(d, 12, 15, 7.0, 3.4, body, tuple(min(c+30,255) for c in body[:3])+(255,), tuple(max(c-25,0) for c in body[:3])+(255,))
    # cerata
    for x in range(7,18,2):
        line(d,(x,12),(x-1,8), tips); px(d,x-1,7, tuple(min(c+40,255) for c in tips[:3])+(255,))
    # rhinophores
    line(d,(17,12),(19,8), tips); line(d,(15,12),(16,8), tips)
    px(d, 18, 13.5, (10,10,10,255))
    return im

def cucumber_sea():
    im = canvas(); d = _d(im)
    blob_shaded(d, 12, 14, 8.0, 3.4, (140,70,50,255), (175,100,70,255), (105,50,36,255))
    for x in range(6,20,3): px(d, x, 11, (190,120,85,255))
    for x in range(7,19,3): px(d, x, 17, (105,50,36,255))
    # feeding tentacles
    for a in range(5):
        line(d,(20,13),(22+((a%2)),(9+a*2)),(190,120,85,255))
    return im

def octopus_gp():
    im = canvas(); d = _d(im)
    blob_shaded(d, 12, 9, 5.2, 4.6, (190,80,70,255), (225,115,95,255), (150,55,48,255))
    px(d, 10, 8, (250,240,230,255)); px(d, 14, 8, (250,240,230,255))
    px(d, 10, 8, (10,10,10,255)) ; px(d, 14, 8, (10,10,10,255))
    for i,xs in enumerate((4,7,10,13,16,19)):
        line(d,(xs,13),(xs-1+(i%3),20),(190,80,70,255),2)
        px(d, xs-1+(i%3), 21, (150,55,48,255))
    return im
import math

def _d(im): return ImageDraw.Draw(im)

def butterfly(fore, hind, marks=None, tails=False, body=(50,45,42,255)):
    im = canvas(); d = _d(im)
    dk_f = tuple(max(c-35,0) for c in fore[:3])+(255,)
    # forewings
    poly(d, [(11,10),(3,3),(2,9),(10,12)], fore)
    poly(d, [(13,10),(21,3),(22,9),(14,12)], fore)
    # hindwings
    poly(d, [(11,13),(4,15),(6,20),(11,15)], hind)
    poly(d, [(13,13),(20,15),(18,20),(13,15)], hind)
    if tails:
        line(d,(6,19),(5,23), hind); line(d,(18,19),(19,23), hind)
    # veins
    line(d,(10,11),(4,5), dk_f); line(d,(14,11),(20,5), dk_f)
    if marks:
        for mx,my in ((5,6),(19,6),(7,16),(17,16)): px(d,mx,my,marks)
    # body
    vline(d, 12, 8, 17, body); px(d, 11, 9, body); px(d,13,9,body)
    px(d, 12, 7, body)
    line(d,(12,7),(10,4),body); line(d,(12,7),(14,4),body)
    return im

def moth(wing, marks, furry=(150,130,100,255), tails=False):
    im = canvas(); d = _d(im)
    poly(d, [(11,11),(2,6),(3,13),(11,14)], wing)
    poly(d, [(13,11),(22,6),(21,13),(13,14)], wing)
    poly(d, [(11,14),(5,17),(8,21),(12,16)], wing)
    poly(d, [(13,14),(19,17),(16,21),(12,16)], wing)
    if tails:
        line(d,(8,20),(7,23), wing); line(d,(16,20),(17,23), wing)
    for mx,my in ((6,9),(18,9)):
        blob(d,mx,my,1.4,1.4, marks); px(d,mx,my,(30,30,30,255))
    blob(d, 12, 12, 1.8, 3.4, furry)
    # feather antennae
    line(d,(11,8),(8,4),furry); line(d,(13,8),(16,4),furry)
    px(d,8,4,furry); px(d,16,4,furry)
    return im

def dragonfly(body, wing=(200,220,230,180)):
    im = canvas(); d = _d(im)
    # wings
    poly(d, [(11,9),(2,6),(2,9),(11,11)], wing)
    poly(d, [(13,9),(22,6),(22,9),(13,11)], wing)
    poly(d, [(11,11),(3,12),(3,15),(11,13)], wing)
    poly(d, [(13,11),(21,12),(21,15),(13,13)], wing)
    # long abdomen
    vline(d, 12, 10, 21, body); vline(d, 11, 10, 20, tuple(max(c-25,0) for c in body[:3])+(255,))
    for y in range(12,21,2): px(d, 12, y, tuple(min(c+35,255) for c in body[:3])+(255,))
    blob(d, 12, 8, 2.0, 1.8, body)
    px(d, 10.5, 7.5, (20,60,40,255)); px(d, 13.5, 7.5, (20,60,40,255))
    return im

def mantis_bug():
    im = canvas(); d = _d(im)
    G=(90,180,70,255); GD=(60,130,48,255); GL=(140,220,110,255)
    # abdomen + wings
    poly(d, [(4,17),(12,12),(13,15),(6,20)], GD)
    poly(d, [(5,16),(12,11),(12,13),(6,18)], G)
    # thorax (long pronotum)
    line(d,(12,12),(16,7), G, 2)
    # triangular head
    poly(d, [(15,5),(19,4),(17,8)], GL)
    px(d, 16, 5, (10,10,10,255)); px(d, 18, 5, (10,10,10,255))
    line(d,(16,4),(14,1),GD); line(d,(18,4),(20,1),GD)
    # raptorial forelegs folded
    line(d,(15,8),(12,6),G,2); line(d,(12,6),(13,9),GL)
    line(d,(16,9),(14,8),G); 
    # walking legs
    line(d,(10,14),(6,11),GD); line(d,(11,16),(8,20),GD); line(d,(9,17),(4,14),GD)
    return im

def beetle(shell, horn=False, metallic=None, jaws=False):
    im = canvas(); d = _d(im)
    lite = metallic or tuple(min(c+40,255) for c in shell[:3])+(255,)
    dark = tuple(max(c-35,0) for c in shell[:3])+(255,)
    blob_shaded(d, 12, 13, 5.0, 6.0, shell, lite, dark)
    vline(d, 12, 8, 19, dark)   # elytra split
    blob(d, 12, 6.5, 2.6, 2.0, dark)
    px(d, 10.5, 6, (240,240,240,255)); px(d, 13.5, 6, (240,240,240,255))
    if jaws:
        line(d,(10,5),(8,1), shell, 2); line(d,(14,5),(16,1), shell, 2)
        px(d,8,2,dark); px(d,16,2,dark)
    if horn:
        line(d,(12,5),(12,1), dark, 2); px(d,11,1,dark)
    # legs
    for sx,sy,ex,ey in [(8,10,4,8),(8,13,3,13),(8,16,4,19),(16,10,20,8),(16,13,21,13),(16,16,20,19)]:
        line(d,(sx,sy),(ex,ey), dark)
    return im

def hopper(body, wings=True):
    im = canvas(); d = _d(im)
    dark = tuple(max(c-30,0) for c in body[:3])+(255,)
    lite = tuple(min(c+30,255) for c in body[:3])+(255,)
    blob(d, 11, 13, 5.6, 2.6, body)
    if wings: poly(d, [(5,11),(15,11),(13,14),(5,13)], lite)
    blob(d, 17, 11, 2.2, 2.0, body)
    px(d, 18, 10.5, (10,10,10,255))
    line(d,(18,9),(22,6), dark)
    # giant jump legs
    line(d,(9,14),(5,10), dark, 2); line(d,(5,10),(3,17), dark, 2)
    line(d,(13,14),(11,18), dark); line(d,(15,14),(14,18), dark)
    return im

def bee(fuzzy=True, big=True):
    im = canvas(); d = _d(im)
    Y=(240,190,60,255); B=(40,36,32,255)
    r = 5.2 if big else 4.2
    blob_shaded(d, 12, 13, r, r*0.86, Y, (255,220,110,255), (200,150,40,255))
    for y in (10,13,16): hline(d, int(12-r+1), int(12+r-1), y, B)
    blob(d, 12, 7.5, 2.2, 1.9, B)
    # wings
    poly(d, [(9,9),(3,4),(7,10)], (215,230,240,200))
    poly(d, [(15,9),(21,4),(17,10)], (215,230,240,200))
    px(d,11,7,(240,240,240,255))
    if fuzzy:
        px(d,7,10,Y); px(d,17,10,Y); px(d,6,13,(255,220,110,255)); px(d,18,13,(255,220,110,255))
    return im

def wasp_th():
    im = canvas(); d = _d(im)
    # tarantula hawk: blue-black body, orange wings
    poly(d, [(9,10),(2,4),(6,11)], (235,130,40,255))
    poly(d, [(15,10),(22,4),(18,11)], (235,130,40,255))
    blob_shaded(d, 12, 14, 3.4, 4.6, (30,36,60,255), (60,72,110,255), (16,20,36,255))
    blob(d, 12, 8, 2.0, 1.8, (30,36,60,255))
    px(d,11,7,(150,160,200,255))
    line(d,(11,6),(8,3),(30,36,60,255)); line(d,(13,6),(16,3),(30,36,60,255))
    for sx,sy,ex,ey in [(9,13,5,16),(9,15,5,19),(15,13,19,16),(15,15,19,19)]:
        line(d,(sx,sy),(ex,ey),(20,24,40,255))
    return im

def velvet_ant():
    im = canvas(); d = _d(im)
    R=(220,60,40,255); B=(30,26,24,255)
    blob(d, 8, 14, 3.0, 2.6, R)      # head fuzz
    blob(d, 12, 14, 2.2, 2.0, B)     # waist
    blob_shaded(d, 17, 14, 3.6, 3.2, R, (250,100,70,255), (170,40,28,255))
    px(d, 7, 13, (10,10,10,255))
    line(d,(7,12),(5,9),B); line(d,(9,12),(8,9),B)
    for sx,ex in ((9,6),(13,11),(16,19)):
        line(d,(sx,16),(ex,20),B)
    px(d,17,13,(255,140,100,255))
    return im

# ---- PLANTS -----------------------------------------------------------------
def conifer(green, trunk=(110,80,55,255), tall=True, snow=False, dead=False):
    im = canvas(); d = _d(im)
    dark = tuple(max(c-30,0) for c in green[:3])+(255,)
    if dead:
        vline(d, 12, 4, 21, (120,110,100,255))
        for y,(l,r) in zip((7,10,13,16),((8,16),(7,17),(6,18),(7,17))):
            line(d,(12,y),(l,y+2),(130,120,110,255)); line(d,(12,y),(r,y+2),(130,120,110,255))
        return im
    vline(d, 11, 18, 22, trunk); vline(d, 12, 18, 22, trunk)
    tiers = [(12,2,3),(12,6,5),(12,10,7),(12,14,9)] if tall else [(12,5,4),(12,9,6),(12,13,8)]
    for cx,ty,hw in tiers:
        poly(d, [(cx,ty),(cx-hw,ty+5),(cx+hw,ty+5)], green)
        line(d,(cx,ty),(cx-hw,ty+5), dark)
    if snow:
        for cx,ty,hw in tiers:
            hline(d, cx-2, cx+2, ty+1, (240,245,250,255))
    return im

def aspen():
    im = canvas(); d = _d(im)
    vline(d, 11, 8, 22, (235,235,230,255)); vline(d, 12, 8, 22, (218,218,212,255))
    px(d, 11, 12, (60,60,58,255)); px(d, 12, 16, (60,60,58,255)); px(d, 11, 19, (60,60,58,255))
    # gold crown
    blob_shaded(d, 12, 6, 6.0, 5.0, (240,190,60,255), (255,220,110,255), (205,150,40,255))
    px(d, 8, 4, (255,225,130,255)); px(d, 15, 8, (205,150,40,255))
    return im

def sequoia(redwood=False):
    im = canvas(); d = _d(im)
    # massive trunk
    for x,(c) in zip((9,10,11,12,13,14),((150,80,55,255),(170,95,62,255),(185,105,70,255),(170,95,62,255),(150,80,55,255),(135,70,48,255))):
        vline(d, x, 6, 22, c)
    vline(d, 10, 8, 20, (140,72,50,255))
    # base flare
    poly(d, [(7,22),(9,17),(9,22)], (150,80,55,255))
    poly(d, [(16,22),(14,17),(14,22)], (135,70,48,255))
    # high crown
    g = (60,110,70,255) if not redwood else (54,100,64,255)
    blob(d, 11.5, 4, 6.4, 3.6, g)
    px(d, 6, 3, (80,135,90,255)); px(d, 16, 5, (46,88,56,255))
    # tiny person for scale
    px(d, 19, 20, (240,200,160,255)); vline(d, 19, 21, 22, (60,90,150,255))
    return im

def cactus_saguaro():
    im = canvas(); d = _d(im)
    hline(d,1,22,21,(220,190,140,255))
    G=(80,150,90,255); GD=(58,115,66,255); GL=(110,180,115,255)
    vline(d, 11, 4, 21, G); vline(d, 12, 4, 21, G); vline(d, 13, 4, 21, GD)
    vline(d, 10, 6, 21, GL)
    # arms
    vline(d, 6, 9, 13, G); hline(d, 6, 10, 13, G); px(d,6,8,GL)
    vline(d, 18, 7, 11, G); hline(d, 14, 18, 11, GD); px(d,18,6,GL)
    for y in range(6,20,3): px(d, 12, y, (200,230,200,255))
    return im

def joshua():
    im = canvas(); d = _d(im)
    hline(d,1,22,21,(220,190,140,255))
    T=(120,90,60,255)
    vline(d, 12, 12, 21, T); vline(d, 11, 13, 21, (100,74,48,255))
    line(d,(12,13),(7,9),T,2); line(d,(12,13),(17,9),T,2)
    for cx,cy in ((6,7),(18,7),(12,11)):
        for a in range(8):
            ang = 2*math.pi*a/8
            line(d,(cx,cy),(int(cx+3.4*math.cos(ang)),int(cy+3.4*math.sin(ang))),(110,170,90,255))
    return im

def prickly_pear():
    im = canvas(); d = _d(im)
    hline(d,1,22,21,(220,190,140,255))
    G=(90,160,100,255); GL=(120,190,125,255)
    blob_shaded(d, 12, 16, 5.0, 4.0, G, GL, (66,124,74,255))
    blob_shaded(d, 8, 9, 3.4, 3.6, G, GL, (66,124,74,255))
    blob_shaded(d, 16, 10, 3.0, 3.2, G, GL, (66,124,74,255))
    for sx,sy in ((7,8),(9,10),(15,9),(17,11),(10,15),(14,16),(12,18)): px(d,sx,sy,(240,240,220,255))
    blob(d, 8, 5.5, 1.4, 1.6, (200,60,90,255))   # tuna fruit
    blob(d, 16, 6.8, 1.2, 1.4, (200,60,90,255))
    return im

def sagebrush():
    im = canvas(); d = _d(im)
    hline(d,1,22,21,(210,185,140,255))
    T=(130,110,80,255)
    line(d,(12,21),(12,15),T,2)
    for ang,ln in ((-0.9,7),(-0.4,8),(0.0,8),(0.4,8),(0.9,7)):
        ex = int(12+ln*math.sin(ang)); ey = int(15-ln*math.cos(ang))
        line(d,(12,15),(ex,ey),T)
        blob(d, ex, ey, 2.6, 2.2, (150,165,140,255))
        px(d, ex, ey-1, (180,195,170,255))
    return im

def wildflower(petal, center=(250,220,110,255), spike=False, stem=(80,140,70,255)):
    im = canvas(); d = _d(im)
    vline(d, 12, 12, 21, stem)
    line(d,(12,17),(9,15),stem); line(d,(12,15),(15,13),stem)
    if spike:
        for y in range(4,13,2):
            px(d, 11, y, petal); px(d, 13, y+1, petal)
            px(d, 12, y, tuple(min(c+30,255) for c in petal[:3])+(255,))
        px(d, 12, 3, tuple(min(c+30,255) for c in petal[:3])+(255,))
    else:
        for a in range(8):
            ang = 2*math.pi*a/8
            blob(d, 12+3.4*math.cos(ang), 8+3.4*math.sin(ang), 1.7, 1.7, petal)
        blob(d, 12, 8, 2.0, 2.0, center)
    return im

def berry_bush(berry, leaf=(70,130,80,255)):
    im = canvas(); d = _d(im)
    blob_shaded(d, 12, 13, 7.0, 6.0, leaf, tuple(min(c+30,255) for c in leaf[:3])+(255,), tuple(max(c-30,0) for c in leaf[:3])+(255,))
    vline(d, 12, 19, 22, (110,80,55,255))
    for bx,by in ((8,11),(12,9),(16,12),(10,15),(14,15),(12,12)):
        blob(d, bx, by, 1.5, 1.5, berry)
        px(d, bx, by-1, tuple(min(c+50,255) for c in berry[:3])+(255,))
    return im

def strawberry_wild():
    im = canvas(); d = _d(im)
    for sx in (5,12,19):
        poly(d,[(sx-3,21),(sx,17),(sx+3,21)], (70,140,75,255))
    blob(d, 12, 12, 3.0, 3.6, (220,60,70,255))
    poly(d,[(10,9),(14,9),(12,7)], (80,150,80,255))
    for px_,py_ in ((11,11),(13,13),(12,14),(10,13)): px(d,px_,py_,(250,230,150,255))
    blob(d, 6, 9, 2.2, 2.2, (250,250,250,255)); blob(d,6,9,1.0,1.0,(250,220,110,255))
    return im

# (name, habitat, xp, rarity1-4, draw_fn)
BIRDS = [
 ("Bald Eagle","RIV",25,2, lambda: raptor((80,60,45,255),(245,245,240,255),(70,52,40,255),tail=(245,245,240,255))),
 ("Golden Eagle","MTN",30,3, lambda: raptor((95,70,45,255),(120,90,50,255),(80,58,38,255),crown=(190,150,80,255))),
 ("Osprey","RIV",20,2, lambda: raptor((100,90,80,255),(240,238,232,255),(85,76,66,255),tail=(150,140,130,255))),
 ("Peregrine Falcon","MTN",35,3, lambda: raptor((70,80,95,255),(230,228,222,255),(58,66,80,255),tail=(90,100,115,255))),
 ("Red-tailed Hawk","MDW",15,1, lambda: raptor((120,95,70,255),(140,110,80,255),(105,82,60,255),tail=(190,90,60,255))),
 ("Great Gray Owl","FOR",40,3, lambda: owl((120,115,110,255),(160,155,148,255),ear=False)),
 ("Great Horned Owl","FOR",20,2, lambda: owl((140,110,80,255),(190,160,120,255),ear=True)),
 ("Mountain Bluebird","MDW",15,1, lambda: songbird((80,140,220,255),(150,190,235,255),(60,110,190,255))),
 ("Steller's Jay","FOR",10,1, lambda: songbird((50,80,180,255),(70,110,200,255),(35,55,140,255),crest=True)),
 ("Clark's Nutcracker","MTN",20,2, lambda: songbird((170,168,165,255),(190,188,184,255),(45,45,48,255))),
 ("Gray Jay","MTN",15,1, lambda: songbird((150,150,152,255),(215,215,215,255),(120,120,124,255))),
 ("American Dipper","RIV",25,2, lambda: dipper()),
 ("Harlequin Duck","RIV",30,3, lambda: duck((70,80,110,255),(60,70,100,255),bill=(140,150,160,255),wingpatch=(180,80,70,255))),
 ("Trumpeter Swan","RIV",30,3, lambda: swan((245,245,242,255))),
 ("Sandhill Crane","MDW",20,2, lambda: crane((150,145,138,255))),
 ("Ruffed Grouse","FOR",15,1, lambda: grouse((140,105,75,255),chest=(180,150,110,255),fan=True,fanc=(150,110,75,255))),
 ("Sage Grouse","DES",35,3, lambda: grouse((120,110,95,255),chest=(240,238,232,255),fan=True,fanc=(130,120,100,255))),
 ("Pinyon Jay","DES",15,1, lambda: songbird((95,115,180,255),(115,135,195,255),(75,92,150,255))),
 ("Common Raven","ANY",10,1, lambda: raven()),
 ("Turkey Vulture","DES",10,1, lambda: vulture()),
 ("Lewis's Woodpecker","FOR",25,2, lambda: woodpecker((90,60,70,255),(180,70,80,255),(50,80,60,255),belly=(220,160,150,255))),
 ("Pileated Woodpecker","FOR",20,2, lambda: woodpecker((40,40,44,255),(220,50,50,255),(35,35,40,255))),
 ("Violet-green Swallow","RIV",15,1, lambda: swallow()),
 ("Calliope Hummingbird","MDW",25,2, lambda: hummingbird((110,160,90,255),(220,80,130,255),(150,170,150,180))),
 ("Rufous Hummingbird","ANY",15,1, lambda: hummingbird((205,110,50,255),(230,70,50,255),(190,160,120,180))),
 ("Black-billed Magpie","MDW",10,1, lambda: magpie()),
 ("Western Meadowlark","MDW",15,1, lambda: songbird((150,130,95,255),(240,200,60,255),(120,100,70,255))),
 ("Canyon Wren","MTN",20,2, lambda: songbird((160,100,70,255),(235,225,210,255),(135,82,58,255),tailc=(150,90,60,255))),
 ("American Kestrel","MDW",15,1, lambda: kestrel_hover()),
 ("Prairie Falcon","DES",25,2, lambda: raptor((150,130,105,255),(170,150,120,255),(130,112,90,255))),
 ("Northern Flicker","ANY",10,1, lambda: woodpecker((160,140,115,255),(200,80,70,255),(140,120,95,255))),
]

ANIMALS = [
 ("Grizzly Bear","MTN",50,4, lambda: bear((130,95,60,255),hump=True)),
 ("Black Bear","FOR",30,3, lambda: bear((45,40,38,255),hump=False,muzzle=(150,120,90,255))),
 ("Gray Wolf","FOR",50,4, lambda: canid((130,128,130,255),(200,198,200,255),howl=True)),
 ("Coyote","ANY",15,1, lambda: canid((160,135,100,255),(210,190,160,255))),
 ("Red Fox","FOR",20,2, lambda: canid((205,100,55,255),(240,230,220,255),tail_tip=(245,245,240,255),ears_big=True)),
 ("Mountain Lion","MTN",50,4, lambda: felid((175,145,110,255))),
 ("Bobcat","FOR",30,3, lambda: felid((160,130,95,255),spots=(110,85,60,255),tail_long=False,tufts=True)),
 ("Lynx","FOR",45,3, lambda: felid((175,168,158,255),spots=(135,128,118,255),tail_long=False,tufts=True)),
 ("Wolverine","MTN",60,4, lambda: mustelid((70,55,45,255),belly=(150,120,80,255))),
 ("American Marten","FOR",35,3, lambda: mustelid((140,95,60,255),belly=(230,180,90,255))),
 ("River Otter","RIV",25,2, lambda: mustelid((100,75,55,255),water=True)),
 ("Beaver","RIV",20,2, lambda: beaver()),
 ("Moose","RIV",35,3, lambda: ungulate((80,65,55,255),antlers='moose',bell=True)),
 ("Elk","MDW",25,2, lambda: ungulate((150,120,85,255),antlers='elk',white_rump=True)),
 ("Mule Deer","ANY",15,1, lambda: ungulate((150,135,110,255),antlers='deer')),
 ("White-tailed Deer","FOR",15,1, lambda: ungulate((160,125,90,255),antlers='deer',white_rump=True)),
 ("Pronghorn","DES",25,2, lambda: ungulate((190,150,105,255),horn='prong',white_rump=True)),
 ("Bighorn Sheep","MTN",30,3, lambda: ungulate((150,135,115,255),horn='curl',white_rump=True)),
 ("Mountain Goat","MTN",35,3, lambda: ungulate((240,240,235,255),horn='spike',shaggy=True)),
 ("Yellow-bellied Marmot","MTN",15,1, lambda: marmot()),
 ("Golden-mantled Squirrel","FOR",10,1, lambda: rodent_small((160,120,75,255),stripe=(240,235,220,255),cheeks=True)),
 ("Least Chipmunk","ANY",10,1, lambda: rodent_small((150,110,70,255),stripe=(60,45,35,255),cheeks=True)),
 ("American Pika","MTN",30,3, lambda: pika()),
]

FISH = [
 ("Cutthroat Trout","RIV",25,2, lambda: troutform((170,150,100,255),(220,205,160,255),spots=(60,50,40,255),slash=(220,70,50,255))),
 ("Rainbow Trout","RIV",15,1, lambda: troutform((130,150,130,255),(215,220,210,255),spots=(55,60,55,255),stripe=(230,110,120,255))),
 ("Brown Trout","RIV",15,1, lambda: troutform((165,135,80,255),(225,205,150,255),spots=(170,60,50,255))),
 ("Brook Trout","RIV",20,2, lambda: troutform((80,110,90,255),(230,120,80,255),spots=(230,200,120,255))),
 ("Bull Trout","RIV",35,3, lambda: troutform((110,120,105,255),(210,160,110,255),spots=(235,190,150,255))),
 ("Kokanee Salmon","RIV",25,2, lambda: troutform((200,60,50,255),(160,50,45,255),jaw_hook=True)),
 ("Mountain Whitefish","RIV",15,1, lambda: troutform((185,185,180,255),(230,230,225,255),forked=True)),
 ("Northern Pike","RIV",20,2, lambda: pike_form((90,120,70,255),(190,200,160,255),spots=(200,210,150,255))),
 ("Yellow Perch","RIV",10,1, lambda: perch_form((200,175,80,255),(90,110,60,255),(235,225,180,255))),
 ("Largemouth Bass","RIV",15,1, lambda: bass_form((100,130,80,255),(210,215,190,255),stripe=(50,70,45,255))),
 ("Smallmouth Bass","RIV",15,1, lambda: bass_form((140,115,70,255),(210,195,155,255),red_eye=True)),
 ("Channel Catfish","RIV",15,1, lambda: catfish()),
 ("Walleye","RIV",20,2, lambda: perch_form((160,140,85,255),(120,105,65,255),(230,225,200,255))),
 ("White Sturgeon","RIV",40,3, lambda: sturgeon()),
 ("Arctic Grayling","RIV",30,3, lambda: troutform((120,120,140,255),(180,180,200,255),sail=True,spots=(70,70,95,255))),
 ("Tiger Muskie","RIV",45,3, lambda: pike_form((110,125,80,255),(200,205,170,255),spots=(60,75,45,255))),
 ("Golden Trout","MTN",50,4, lambda: troutform((235,180,60,255),(250,215,110,255),stripe=(225,90,60,255),spots=(80,60,40,255))),
 ("Lake Trout","RIV",25,2, lambda: troutform((105,115,120,255),(180,188,192,255),spots=(210,215,218,255),forked=True)),
 ("Sockeye Salmon","RIV",30,3, lambda: troutform((215,55,45,255),(170,45,40,255),jaw_hook=True)),
 ("Chinook Salmon","CST",35,3, lambda: troutform((120,130,140,255),(200,205,210,255),spots=(50,55,60,255),jaw_hook=True)),
]

BUGS = [
 ("Monarch","MDW",20,2, lambda: butterfly((230,130,40,255),(215,110,35,255),marks=(250,250,250,255))),
 ("Tiger Swallowtail","RIV",15,1, lambda: butterfly((240,210,70,255),(235,200,60,255),marks=(40,40,40,255),tails=True)),
 ("Mourning Cloak","FOR",15,1, lambda: butterfly((90,60,80,255),(85,55,75,255),marks=(240,225,150,255))),
 ("Two-tailed Swallowtail","DES",20,2, lambda: butterfly((245,215,80,255),(240,205,70,255),marks=(60,60,180,255),tails=True)),
 ("Luna Moth","FOR",30,3, lambda: moth((170,230,170,255),(200,240,190,255),furry=(220,240,220,255),tails=True)),
 ("Cecropia Moth","FOR",30,3, lambda: moth((140,90,70,255),(220,120,100,255),furry=(160,100,80,255))),
 ("Hummingbird Moth","MDW",25,2, lambda: moth((190,160,110,255),(210,150,90,255),furry=(170,150,110,255))),
 ("Praying Mantis","MDW",15,1, lambda: mantis_bug()),
 ("Green Darner","RIV",10,1, lambda: dragonfly((80,160,90,255))),
 ("Damselfly","RIV",10,1, lambda: dragonfly((70,120,220,255),wing=(190,210,235,150))),
 ("Cicada","FOR",15,1, lambda: beetle((90,110,70,255),metallic=(150,180,110,255))),
 ("Grasshopper","MDW",5,1, lambda: hopper((130,170,80,255))),
 ("Cricket","MDW",5,1, lambda: hopper((60,50,45,255),wings=False)),
 ("Stag Beetle","FOR",20,2, lambda: beetle((70,50,40,255),jaws=True)),
 ("Tiger Beetle","RIV",15,1, lambda: beetle((60,150,110,255),metallic=(140,230,180,255))),
 ("Jewel Beetle","FOR",20,2, lambda: beetle((50,130,120,255),metallic=(120,220,190,255))),
 ("Velvet Ant","DES",25,2, lambda: velvet_ant()),
 ("Tarantula Hawk","DES",40,3, lambda: wasp_th()),
 ("Bumblebee","MDW",10,1, lambda: bee(big=True)),
 ("Honey Bee","ANY",10,1, lambda: bee(big=False,fuzzy=False)),
]

PLANTS = [
 ("Quaking Aspen","MTN",10,1, lambda: aspen()),
 ("Ponderosa Pine","FOR",10,1, lambda: conifer((90,140,90,255),trunk=(190,120,70,255))),
 ("Lodgepole Pine","FOR",10,1, lambda: conifer((70,120,80,255),tall=True)),
 ("Douglas Fir","FOR",10,1, lambda: conifer((60,110,75,255))),
 ("Engelmann Spruce","MTN",15,1, lambda: conifer((70,110,110,255),snow=True)),
 ("Subalpine Fir","MTN",15,1, lambda: conifer((60,105,85,255),tall=True,snow=True)),
 ("Bristlecone Pine","MTN",40,3, lambda: conifer((100,110,80,255),dead=True)),
 ("Giant Sequoia","FOR",35,3, lambda: sequoia()),
 ("Coast Redwood","CST",35,3, lambda: sequoia(redwood=True)),
 ("Joshua Tree","DES",20,2, lambda: joshua()),
 ("Saguaro","DES",25,2, lambda: cactus_saguaro()),
 ("Prickly Pear","DES",10,1, lambda: prickly_pear()),
 ("Sagebrush","DES",5,1, lambda: sagebrush()),
 ("Lupine","MDW",10,1, lambda: wildflower((110,90,200,255),spike=True)),
 ("Indian Paintbrush","MDW",15,1, lambda: wildflower((230,80,50,255),spike=True)),
 ("Fireweed","FOR",10,1, lambda: wildflower((225,90,160,255),spike=True)),
 ("Beargrass","MTN",20,2, lambda: wildflower((240,240,225,255),spike=True,stem=(120,160,90,255))),
 ("Balsamroot","DES",10,1, lambda: wildflower((240,200,60,255),center=(160,110,40,255))),
 ("Sego Lily","DES",15,1, lambda: wildflower((245,240,235,255),center=(200,150,190,255))),
 ("Camas Lily","MDW",15,1, lambda: wildflower((90,110,220,255),center=(240,220,120,255))),
 ("Bitterroot","DES",15,1, lambda: wildflower((240,140,170,255),center=(250,210,150,255))),
 ("Wild Strawberry","FOR",10,1, lambda: strawberry_wild()),
 ("Huckleberry","MTN",15,1, lambda: berry_bush((80,60,140,255))),
 ("Thimbleberry","FOR",10,1, lambda: berry_bush((210,60,70,255),leaf=(90,150,90,255))),
]

TIDEPOOL = [
 ("Ochre Sea Star","TID",25,2, lambda: seastar((220,120,60,255),knobby=True)),
 ("Purple Sea Star","TID",20,2, lambda: seastar((130,80,160,255),knobby=True)),
 ("Brittle Star","TID",20,2, lambda: seastar((150,140,120,255),points=5)),
 ("Sea Urchin","TID",20,2, lambda: urchin()),
 ("Green Anemone","TID",25,2, lambda: anemone((90,150,110,255),(120,200,140,255))),
 ("Pink Anemone","TID",15,1, lambda: anemone((170,110,130,255),(220,150,170,255))),
 ("Hermit Crab","TID",15,1, lambda: crab((200,140,80,255),claws_big=False)),
 ("Dungeness Crab","TID",20,2, lambda: crab((160,90,70,255))),
 ("Red Rock Crab","TID",20,2, lambda: crab((190,60,50,255))),
 ("Limpet","TID",10,1, lambda: shell_limpet()),
 ("Chiton","TID",15,1, lambda: chiton()),
 ("Gumboot Chiton","TID",30,3, lambda: chiton(giant=True)),
 ("Moon Snail","TID",20,2, lambda: snail_moon()),
 ("Sea Lemon","TID",30,3, lambda: nudibranch((240,210,90,255),(250,230,140,255))),
 ("Opalescent Nudibranch","TID",35,3, lambda: nudibranch((150,200,230,255),(240,150,80,255))),
 ("Sea Cucumber","TID",20,2, lambda: cucumber_sea()),
 ("Giant Pacific Octopus","TID",45,3, lambda: octopus_gp()),
]

BEACH = [
 ("Harbor Seal","CST",25,2, lambda: seal((150,148,152,255))),
 ("California Sea Lion","CST",20,2, lambda: seal((130,100,75,255),spots=False)),
 ("Elephant Seal","CST",35,3, lambda: seal((140,125,110,255),spots=False,elephant=True)),
 ("Sea Otter","CST",30,3, lambda: otter_sea()),
 ("Gray Whale","CST",40,3, lambda: whale_tail(gray=True)),
 ("Humpback Whale","CST",45,3, lambda: whale_tail((50,60,75,255))),
 ("Orca","CST",50,4, lambda: orca()),
 ("Pacific Dolphin","CST",25,2, lambda: dolphin()),
 ("Brown Pelican","CST",20,2, lambda: pelican()),
 ("Western Gull","CST",10,1, lambda: shorebird((190,190,195,255),(240,240,240,255),legs=(230,180,120,255),long_bill=False,bill=(230,190,80,255))),
 ("Black Oystercatcher","CST",25,2, lambda: shorebird((40,38,40,255),(45,42,45,255),legs=(230,170,150,255),bill=(230,70,50,255))),
 ("Snowy Plover","CST",25,2, lambda: shorebird((205,195,175,255),(245,242,235,255),long_bill=False,bill=(60,60,60,255),band=(60,60,60,255))),
 ("Killdeer","CST",10,1, lambda: shorebird((150,120,90,255),(240,238,232,255),band=(50,45,42,255),long_bill=False)),
 ("Sanderling","CST",10,1, lambda: shorebird((200,198,192,255),(245,245,242,255))),
 ("Whimbrel","CST",20,2, lambda: shorebird((160,140,110,255),(220,210,190,255),long_bill=True)),
]

CATEGORIES = [
    ("BIRDS",    BIRDS),
    ("MAMMALS",  ANIMALS),
    ("FISH",     FISH),
    ("BUGS",     BUGS),
    ("PLANTS",   PLANTS),
    ("TIDEPOOL", TIDEPOOL),
    ("COAST",    BEACH),
]
HABS = {"MTN":"Mountains","FOR":"Forest","RIV":"River/Lake","DES":"Desert",
        "CST":"Coast","TID":"Tide Pool","MDW":"Meadow","ANY":"Anywhere"}
"""Render all sprites -> outline -> per-category palette quantize -> pack sightings.sbi
.sbi layout (little-endian):
  "SBI1"                       4
  u8  catCount
  per category:
    u8  palCount               (<=15; index 0 = transparent)
    u16 palette[palCount]      rgb565
    u8  iconCount
    per icon: 288 bytes        (24x24 packed 2px/byte, 4-bit palette indices)
Also emits sightings_data.h (species table for the C++ side) and sheet.png.
"""
from PIL import Image
import struct

def render_all():
    out = []
    for cname, lst in CATEGORIES:
        imgs = []
        for (name, hab, xp, rar, fn) in lst:
            im = fn()
            im = outline_sprite(im)
            imgs.append(im)
        out.append((cname, lst, imgs))
    return out

def main():
    rendered = render_all()
    blob = bytearray(b"SBI1")
    blob.append(len(rendered))
    sheet_cols = 12
    total_icons = sum(len(x[2]) for x in rendered)
    rows = (total_icons + sheet_cols - 1)//sheet_cols
    sheet = Image.new("RGBA", (sheet_cols*(SZ+4)+4, rows*(SZ+16)+4), (0,34,34,255))
    si = 0
    hdr_lines = []
    for ci,(cname, lst, imgs) in enumerate(rendered):
        pal, idxs = quantize_group(imgs, max_colors=15)
        blob.append(len(pal))
        for (r,g,b) in pal:
            blob += struct.pack("<H", rgb565(r,g,b))
        blob.append(len(imgs))
        for k,(arr, meta) in enumerate(zip(idxs, lst)):
            packed = bytearray()
            flat = arr.flatten()
            for i in range(0, len(flat), 2):
                packed.append((flat[i] & 0xF) | ((flat[i+1] & 0xF) << 4))
            assert len(packed) == 288
            blob += packed
            # rebuild preview from quantized data (what the DEVICE will show)
            prev = Image.new("RGBA",(SZ,SZ),(0,0,0,0))
            pp = prev.load()
            for y in range(SZ):
                for x in range(SZ):
                    v = arr[y,x]
                    if v: pp[x,y] = pal[v-1]+(255,)
            gx = 4+(si%sheet_cols)*(SZ+4); gy = 4+(si//sheet_cols)*(SZ+16)
            sheet.paste(prev,(gx,gy),prev)
            si += 1
        for k,(name,hab,xp,rar,_) in enumerate(lst):
            hdr_lines.append((ci,k,name,hab,xp,rar))
    open(OUT_SBI,"wb").write(blob)
    sheet = sheet.resize((sheet.width*3, sheet.height*3), Image.NEAREST)
    sheet.save(OUT_SHEET)
    # C header: species table only (icons stream from flash)
    with open(OUT_HDR,"w") as f:
        f.write("// sightings_data.h - GENERATED by tools/gen_sightings.py. Do not edit.\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("enum SgHab : uint8_t { SG_MTN=0, SG_FOR, SG_RIV, SG_DES, SG_CST, SG_TID, SG_MDW, SG_ANY };\n\n")
        f.write("struct SgSpecies { const char* name; uint8_t cat; uint8_t icon; uint8_t hab; uint8_t xp; uint8_t rarity; };\n\n")
        f.write("static const char* const SG_CAT_NAMES[] = {")
        f.write(", ".join('"%s"' % c for c,_,__ in rendered))
        f.write("};\n")
        f.write("static constexpr int SG_CAT_COUNT = %d;\n\n" % len(rendered))
        hm={"MTN":"SG_MTN","FOR":"SG_FOR","RIV":"SG_RIV","DES":"SG_DES","CST":"SG_CST","TID":"SG_TID","MDW":"SG_MDW","ANY":"SG_ANY"}
        f.write("static const SgSpecies SG[] = {\n")
        for (ci,k,name,hab,xp,rar) in hdr_lines:
            f.write('    { "%s", %d, %d, %s, %d, %d },\n' % (name, ci, k, hm[hab], xp, rar))
        f.write("};\n")
        f.write("static constexpr int SG_COUNT = %d;\n" % len(hdr_lines))
    print("icons:", total_icons, " sbi bytes:", len(blob))

if __name__ == "__main__":
    main()
