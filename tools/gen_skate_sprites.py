"""Mantis Skate sprites: skater poses + props. 4bpp shared palette."""
from iconlib import canvas, px, hline, vline, blob, poly, line, outline_sprite, quantize_group, rgb565
from PIL import Image, ImageDraw
import math

def cv(w=24,h=24): return Image.new("RGBA",(w,h),(0,0,0,0))
def d_(im): return ImageDraw.Draw(im)

G  = (90,180,70,255); GD=(60,130,48,255); GL=(140,220,110,255)
DECK=(150,90,50,255); DECKL=(190,125,70,255); WHEEL=(235,230,220,255); TRUCK=(120,124,134,255)

def board(d, cx, y, tilt=0, flip=0):
    """deck drawn at angle: tilt in deg nose-up; flip 0..1 = mid-kickflip roll"""
    if flip >= 1:      # edge-on: deck seen as a line
        hline(d, cx-6, cx+6, y-1, DECK); hline(d, cx-6, cx+6, y-2, DECKL)
        px(d, cx-5, y, WHEEL); px(d, cx+5, y, WHEEL)
        return
    a = math.radians(tilt)
    dx, dy = math.cos(a), -math.sin(a)
    n = (cx+7*dx, y-2+7*dy); t = (cx-7*dx, y-2-7*dy)
    line(d, t, n, DECK, 2)
    px(d, int(n[0]), int(n[1]-1), DECKL); px(d, int(t[0]), int(t[1]-1), DECKL)
    for s in (-4.5, 4.5):
        wx, wy = cx+s*dx, y-2+s*dy
        px(d, int(wx), int(wy+2), TRUCK)
        blob(d, wx, wy+3.2, 1.1, 1.1, WHEEL)

def mantis(d, cx, gy, crouch=0.0, lean=0, arms="ride", head_dy=0):
    """skater: crouch 0..1, lean deg (body tilt), arms: ride/tuck/grab/flail/out"""
    hip = gy - 9 + crouch*3
    sh  = hip - 6 + crouch*2      # shoulder height
    la = math.radians(lean)
    shx = cx + math.sin(la)*4
    # legs: two bent legs from hips to board
    for fx in (-3, 3):
        kneex = cx + fx*0.6 - 1
        kneey = hip + (gy-4-hip)*0.5 - (2 - crouch*1)
        line(d, (cx, hip), (kneex, kneey), GD, 2)
        line(d, (kneex, kneey), (cx+fx, gy-3), GD, 2)
    # abdomen sweeping back
    poly(d, [(cx-1,hip),(cx-8,hip-3+crouch*2),(cx-8,hip-1+crouch*2),(cx-2,hip+2)], GD)
    poly(d, [(cx-1,hip-1),(cx-7,hip-3+crouch*2),(cx-2,hip)], G)
    # thorax
    line(d, (cx, hip), (shx, sh), G, 2)
    # head
    hy = sh - 3 + head_dy
    poly(d, [(shx-1,hy),(shx+4,hy-1),(shx+2,hy+3)], GL)
    px(d, int(shx+1), int(hy), (10,10,10,255))
    line(d,(shx+1,hy-1),(shx-1,hy-4),GD); line(d,(shx+2,hy-1),(shx+4,hy-4),GD)
    # arms
    if arms == "ride":
        line(d,(shx,sh),(shx+4,sh+3),G,2); line(d,(shx+4,sh+3),(shx+6,sh+2),GL)
        line(d,(shx,sh),(shx-4,sh+2),G)
    elif arms == "tuck":
        line(d,(shx,sh),(shx+3,sh+4),G,2); line(d,(shx,sh),(shx-2,sh+4),G,2)
    elif arms == "grab":
        line(d,(shx,sh),(shx+2,gy-4),G,2); line(d,(shx+2,gy-4),(shx+3,gy-3),GL)
        line(d,(shx,sh),(shx-4,sh-2),G)
    elif arms == "out":
        line(d,(shx,sh),(shx+6,sh-2),G,2); line(d,(shx+6,sh-2),(shx+8,sh-3),GL)
        line(d,(shx,sh),(shx-6,sh-1),G,2)
    elif arms == "flail":
        line(d,(shx,sh),(shx+5,sh-4),G,2); line(d,(shx,sh),(shx-4,sh-4),G,2)

def sp_roll(f):
    im=cv(); d=d_(im); gy=22
    board(d, 12, gy)
    mantis(d, 12, gy, crouch=0.15 + 0.15*f, lean=4, arms="ride", head_dy=f)
    return im

def sp_crouch():
    im=cv(); d=d_(im); gy=22
    board(d, 12, gy)
    mantis(d, 12, gy, crouch=1.0, lean=6, arms="tuck")
    return im

def sp_air():
    im=cv(); d=d_(im); gy=21
    board(d, 12, gy, tilt=8)
    mantis(d, 12, gy, crouch=0.7, lean=8, arms="tuck")
    return im

def sp_flip(stage):
    im=cv(); d=d_(im); gy=22
    board(d, 12, gy, flip=1) if stage==0 else board(d, 12, gy, tilt=170)
    mantis(d, 12, gy-3, crouch=1.0, lean=6, arms="tuck")
    return im

def sp_grab():
    im=cv(); d=d_(im); gy=21
    board(d, 12, gy, tilt=14)
    mantis(d, 12, gy, crouch=0.85, lean=10, arms="grab")
    return im

def sp_manual():
    im=cv(); d=d_(im); gy=22
    board(d, 12, gy, tilt=22)
    mantis(d, 11, gy-1, crouch=0.35, lean=-6, arms="out")
    return im

def sp_grind():
    im=cv(); d=d_(im); gy=21
    board(d, 12, gy, tilt=-4)
    mantis(d, 12, gy, crouch=0.8, lean=2, arms="out")
    return im

def sp_bail(f):
    im=cv(); d=d_(im)
    cx, cy = 12, 12
    a = 0.9 + f*1.4
    # tumbling: body as rotated segments
    line(d,(cx-4*math.cos(a),cy-4*math.sin(a)),(cx+4*math.cos(a),cy+4*math.sin(a)),G,3)
    line(d,(cx+4*math.cos(a),cy+4*math.sin(a)),(cx+7*math.cos(a+0.6),cy+7*math.sin(a+0.6)),GD,2)
    blob(d, cx+5*math.cos(a-0.8), cy+5*math.sin(a-0.8), 2.0, 1.8, GL)
    for k in range(4):
        aa = a + 1.1*k
        line(d,(cx,cy),(cx+6*math.cos(aa),cy+6*math.sin(aa)),GD)
    return im

def sp_board_fly():
    im=cv(12,8); d=d_(im)
    line(d,(1,4),(10,2),DECK,2); px(d,2,3,DECKL)
    px(d,3,5,WHEEL); px(d,8,4,WHEEL)
    return im

def sp_cone():
    im=cv(10,10); d=d_(im)
    poly(d,[(1,9),(9,9),(6,1),(4,1)],(230,120,50,255))
    hline(d,3,7,6,(250,250,245,255))
    hline(d,1,9,9,(200,95,40,255))
    px(d,4,2,(250,170,100,255))
    return im

def sp_hydrant():
    im=cv(10,13); d=d_(im)
    poly(d,[(2,12),(8,12),(8,4),(2,4)],(190,60,50,255))
    blob(d,5,3,3,2,(210,80,60,255))
    px(d,5,1,(230,110,80,255))
    px(d,1,7,(210,80,60,255)); px(d,9,7,(210,80,60,255))
    hline(d,2,8,12,(150,45,38,255))
    px(d,3,5,(235,130,90,255))
    return im

sprites = [
    ("SK_ROLL1", sp_roll(0)), ("SK_ROLL2", sp_roll(1)),
    ("SK_CROUCH", sp_crouch()), ("SK_AIR", sp_air()),
    ("SK_FLIP1", sp_flip(0)), ("SK_FLIP2", sp_flip(1)),
    ("SK_GRAB", sp_grab()), ("SK_MANUAL", sp_manual()), ("SK_GRIND", sp_grind()),
    ("SK_BAIL1", sp_bail(0)), ("SK_BAIL2", sp_bail(1)),
    ("SK_BOARD", sp_board_fly()),
    ("SK_CONE", sp_cone()), ("SK_HYDRANT", sp_hydrant()),
]
imgs = [outline_sprite(im) for _, im in sprites]
pal, idxs = quantize_group(imgs, max_colors=15)

with open("/home/claude/rev/src/mantisskate_sprites.h","w") as f:
    f.write("// mantisskate_sprites.h - GENERATED by tools/gen_skate_sprites.py. Do not edit.\n")
    f.write("// 4bpp packed (2px/byte, low nibble first), index 0 = transparent.\n")
    f.write("#pragma once\n#include <stdint.h>\n\n")
    f.write("static const uint16_t SK_PAL[%d] = {" % len(pal))
    f.write(",".join("0x%04X" % rgb565(*c) for c in pal))
    f.write("};\nstatic constexpr int SK_PAL_N = %d;\n\n" % len(pal))
    for (name, im), arr in zip(sprites, idxs):
        h, w = arr.shape
        packed=[]
        for y in range(h):
            for x in range(0,w,2):
                v0=int(arr[y,x]); v1=int(arr[y,x+1]) if x+1<w else 0
                packed.append((v0&0xF)|((v1&0xF)<<4))
        f.write("static const uint8_t %s[%d] = { // %dx%d\n    " % (name,len(packed),w,h))
        f.write(",".join(str(b) for b in packed))
        f.write("\n};\nstatic constexpr int %s_W = %d, %s_H = %d;\n\n" % (name,w,name,h))
print("palette", len(pal))

sheet = Image.new("RGBA", (14*30+8, 40), (0,34,34,255))
for i,(nm,_) in enumerate(sprites):
    prev = Image.new("RGBA", imgs[i].size, (0,0,0,0)); pp=prev.load(); a=idxs[i]
    for y in range(a.shape[0]):
        for x in range(a.shape[1]):
            if a[y,x]: pp[x,y]=pal[a[y,x]-1]+(255,)
    sheet.paste(prev,(6+i*30, 8), prev)
sheet.resize((sheet.width*4, sheet.height*4), Image.NEAREST).save("skate_sprites.png")
print("done")
