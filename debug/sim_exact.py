# Simula EXACTAMENTE o loop do 3ds_render.c para confirmar a imagem
import struct, sys
sys.path.insert(0,'tools')
from raw2png import HDR, to_rgb, write_png
d=open('debug/dbg_src.raw','rb').read()
_,_,w,h,stride,bpp=HDR.unpack_from(d,0); px=d[HDR.size:]
# frontend passa fw=256, fh=239, crops = CORE_FRAME_* — ver main.c
# render: src_width=w=256; crop_left/right do main; row_pixels = 256 - cl - cr
FBW,FBH=240,400
MIRROR=1
# Assumindo crop 0 por enquanto (o dump nao traz crops); ver meta
cl=cr=0; ct=0
row_pixels=w-cl-cr; rows=h-ct
row_src=ct*w+cl
dst_w=min(rows,FBW); dst_h=min(row_pixels,FBH)
ox=(FBW-dst_w)//2; oy=(FBH-dst_h)//2
fb=bytearray(FBW*FBH*2)
for y in range(dst_h):
    for x in range(dst_w):
        if MIRROR:
            i=x*row_pixels+(row_pixels-1-y)
        else:
            i=x*row_pixels+y
        p=px[i*4:i*4+4]
        r5,g6,b5=(p[2]>>3)&31,(p[1]>>2)&63,(p[0]>>3)&31
        struct.pack_into('<H',fb,((oy+y)*FBW+(ox+x))*2,(r5<<11)|(g6<<5)|b5)
write_png('debug/sim_m1.png',FBW,FBH,to_rgb(FBW,FBH,FBW*2,16,fb))
print(f"sim.png  dst={dst_w}x{dst_h} em {FBW}x{FBH} mirror={MIRROR} (row_pixels={row_pixels})")
