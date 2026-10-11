import struct, sys
sys.path.insert(0,'tools')
from raw2png import HDR, to_rgb, write_png
d=open('debug/dbg_src.raw','rb').read()
_,_,w,h,_,_=HDR.unpack_from(d,0); px=d[HDR.size:]
FBW,FBH=240,400
ow,oh=h,w           # 239 x 256
img=bytearray(ow*oh*4)
for sy in range(h):
    for sx in range(w):
        nxx=h-1-sy; nYY=sx
        img[(nYY*ow+nxx)*4:(nYY*ow+nxx)*4+4]=px[(sy*w+sx)*4:(sy*w+sx)*4+4]
fb=bytearray(FBW*FBH*2)
ox=(FBW-ow)//2; oy=(FBH-oh)//2
for y in range(oh):
    for x in range(ow):
        p=img[(y*ow+x)*4:(y*ow+x)*4+4]
        r5,g6,b5=(p[2]>>3)&31,(p[1]>>2)&63,(p[0]>>3)&31
        struct.pack_into('<H',fb,((oy+y)*FBW+(ox+x))*2,(r5<<11)|(g6<<5)|b5)
write_png('debug/REF_FB.png',FBW,FBH,to_rgb(FBW,FBH,FBW*2,16,fb))
print(f"REF_FB.png: o que gravar no portrait. footprint {ow}x{oh} em {FBW}x{FBH}, offset ({ox},{oy})")
