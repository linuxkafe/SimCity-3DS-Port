# A imagem correcta no painel = rot_ccw(fb). Queremos = src_upright.
# seen_correcto (fb=rot_cw(src)) -> painel mostra src_upright.
# Logo o render deve gravar fb = rot_cw(src) no portrait.
# rot_cw: (x,y) -> (H-1-y, x) num destino H x W.
import struct, sys
sys.path.insert(0,'tools')
from raw2png import HDR, to_rgb, write_png
d=open('debug/dbg_src.raw','rb').read()
_,_,w,h,_,_=HDR.unpack_from(d,0); px=d[HDR.size:]
FBW,FBH=240,400
ow,oh=h,w  # portrait: 239x256
img=bytearray(ow*oh*4)
for y in range(h):          # source rows = 239 (h)
    for x in range(w):      # source cols = 256 (w)
        nxx,nYY=h-1-y,x     # CW
        img[(nYY*ow+nxx)*4:(nYY*ow+nxx)*4+4]=px[(y*w+x)*4:(y*w+x)*4+4]
fb=bytearray(FBW*FBH*2)
dw=min(ow,FBW); dh=min(oh,FBH); ox=(FBW-dw)//2; oy=(FBH-dh)//2
for y in range(dh):
    for x in range(dw):
        p=img[(y*ow+x)*4:(y*ow+x)*4+4]
        r5,g6,b5=(p[2]>>3)&31,(p[1]>>2)&63,(p[0]>>3)&31
        struct.pack_into('<H',fb,((oy+y)*FBW+(ox+x))*2,(r5<<11)|(g6<<5)|b5)
write_png('debug/RENDER_CW.png',FBW,FBH,to_rgb(FBW,FBH,FBW*2,16,fb))
print(f"RENDER_CW.png: gravar rot_cw(src) no portrait {FBW}x{FBH} (footprint {dw}x{dh})")
