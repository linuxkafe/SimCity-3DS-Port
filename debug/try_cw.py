import struct, sys
sys.path.insert(0,'tools')
from raw2png import HDR, to_rgb, write_png
d=open('debug/dbg_src.raw','rb').read()
_,_,w,h,stride,bpp = HDR.unpack_from(d,0)
px=d[HDR.size:]
FBW,FBH=240,400
# rotate 90 CW: source (w x h) -> (h x w)
ow,oh=h,w
img=bytearray(ow*oh*4)
for y in range(h):
    for x in range(w):
        # CW: (x,y)->(h-1-y, x)
        nxx,nYY=h-1-y,x
        img[(nYY*ow+nxx)*4:(nYY*ow+nxx)*4+4]=px[(y*w+x)*4:(y*w+x)*4+4]
fb=bytearray(FBW*FBH*2)
dst_w=min(ow,FBW); dst_h=min(oh,FBH)
ox=(FBW-dst_w)//2; oy=(FBH-dst_h)//2
for y in range(dst_h):
    for x in range(dst_w):
        p=img[(y*ow+x)*4:(y*ow+x)*4+4]
        r5,g6,b5=(p[2]>>3)&31,(p[1]>>2)&63,(p[0]>>3)&31
        struct.pack_into('<H',fb,((oy+y)*FBW+(ox+x))*2,(r5<<11)|(g6<<5)|b5)
write_png('debug/cw.png',FBW,FBH,to_rgb(FBW,FBH,FBW*2,16,fb))
print(f"cw.png  (rotate-CW {ow}x{oh} em portrait {FBW}x{FBH})")
