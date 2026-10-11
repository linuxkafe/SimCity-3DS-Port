import struct, sys
sys.path.insert(0,'tools')
from raw2png import HDR, to_rgb, write_png
d=open('debug/dbg_src.raw','rb').read()
_,_,w,h,_,_=HDR.unpack_from(d,0); px=d[HDR.size:]

# O painel apresenta o portrait rodado 90 CCW.
# Simula: o que写入 no fb, depois aplicar rot90 CCW -> o que o olho ve.
def rot_ccw(img,W,H):  # img WxH -> HxW, rotaciona 90 CCW
    o_w,o_h=H,W; o=bytearray(o_w*o_h*4)
    for y in range(H):
        for x in range(W):
            o[((W-1-x)*o_w+y)*4:((W-1-x)*o_w+y)*4+4]=img[(y*W+x)*4:(y*W+x)*4+4]
    return o,o_w,o_h
def rot_cw(img,W,H):
    o_w,o_h=H,W; o=bytearray(o_w*o_h*4)
    for y in range(H):
        for x in range(W):
            o[(x*o_w+(H-1-y))*4:(x*o_w+(H-1-y))*4+4]=img[(y*W+x)*4:(y*W+x)*4+4]
    return o,o_w,o_h

FBW,FBH=400,240   # o que o olho ve (paisagem)
for name,fn in [('fb_cw_then_ccw',rot_ccw),('fb_ccw_then_ccw',rot_cw)]:
    r_img,r_w,r_h=fn(px,w,h)   # imagem ja rodada gravada no fb
    # painel aplica rot CCW
    seen,s_w,s_h=rot_ccw(r_img,r_w,r_h)
    # seen tem HxW... colocar em paisagem 400x240
    # seen dims: o_w=H(239),o_h=W(256) -> 239x256 landscape-ish
    W2,H2=s_w,s_h
    fb=bytearray(FBW*FBH*2)
    dw=min(W2,FBW); dh=min(H2,FBH); ox=(FBW-dw)//2; oy=(FBH-dh)//2
    for y in range(dh):
        for x in range(dw):
            p=seen[(y*W2+x)*4:(y*W2+x)*4+4]
            r5,g6,b5=(p[2]>>3)&31,(p[1]>>2)&63,(p[0]>>3)&31
            struct.pack_into('<H',fb,((oy+y)*FBW+(ox+x))*2,(r5<<11)|(g6<<5)|b5)
    write_png(f'debug/seen_{name}.png',FBW,FBH,to_rgb(FBW,FBH,FBW*2,16,fb))
    print(f"seen_{name}.png  (painel: {FBW}x{FBH})")
