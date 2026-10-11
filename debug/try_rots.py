import struct, sys
sys.path.insert(0,'tools')
from raw2png import HDR, to_rgb, write_png
d=open('debug/dbg_src.raw','rb').read()
_,_,w,h,stride,bpp = HDR.unpack_from(d,0)
px=d[HDR.size:]
FBW,FBH=240,400
def place(img,ow,oh,name):
    fb=bytearray(FBW*FBH*2)
    dst_w=min(ow,FBW); dst_h=min(oh,FBH)
    ox=(FBW-dst_w)//2; oy=(FBH-dst_h)//2
    for y in range(dst_h):
        for x in range(dst_w):
            p=img[(y*ow+x)*4:(y*ow+x)*4+4]
            r5,g6,b5=(p[2]>>3)&31,(p[1]>>2)&63,(p[0]>>3)&31
            struct.pack_into('<H',fb,((oy+y)*FBW+(ox+x))*2,(r5<<11)|(g6<<5)|b5)
    write_png(f'debug/{name}.png',FBW,FBH,to_rgb(FBW,FBH,FBW*2,16,fb))
    print(f"{name}.png")
def rot_cw(img,sw,sh):
    ow,oh=sh,sw; o=bytearray(ow*oh*4)
    for y in range(sh):
        for x in range(sw):
            o[((oh-1-y)*ow+x)*4:((oh-1-y)*ow+x)*4+4]=img[(y*sw+x)*4:(y*sw+x)*4+4]
    return o,ow,oh
# rotacao anti-horaria (a que dá texto na ordem certa)
def rot_ccw(img,sw,sh):
    ow,oh=sh,sw; o=bytearray(ow*oh*4)
    for y in range(sh):
        for x in range(sw):
            o[(x*ow+(ow-1-y))*4:(x*ow+(ow-1-y))*4+4]=img[(y*sw+x)*4:(y*sw+x)*4+4]
    return o,ow,oh
cc,ow,oh=rot_ccw(px,w,h); place(cc,ow,oh,'ccw')
