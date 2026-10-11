import struct, sys
sys.path.insert(0,'tools')
from raw2png import HDR, to_rgb, write_png
d=open('debug/dbg_src.raw','rb').read()
_,_,src_width,src_height,_,_=HDR.unpack_from(d,0); px=d[HDR.size:]
fb_w,fb_h=240,400
rows=src_height; row_pixels=src_width; off=0
# footprint: 239 colunas (x) x 256 linhas (y)
dst_w=min(row_pixels,fb_w)   # 239 colunas
dst_h=min(rows,fb_h)         # 256 linhas  <-- errado antes
# CORRECTO para rot_cw de src(256x239): destino = 239 wide x 256 tall
#   logo dst_w=239 (=src height), dst_h=256 (=src width)
dst_w=min(src_height,fb_w)   # 239
dst_h=min(src_width,fb_h)    # 256
ox=(fb_w-dst_w)//2; oy=(fb_h-dst_h)//2
fb=bytearray(fb_w*fb_h*2)
for dy in range(dst_h):      # 0..255  destino linhas <- fonte colunas
    for dx in range(dst_w):  # 0..238  destino col   <- fonte linhas
        sy=dx; sx=dy         # rot_cw: (sx_fonte,sy_fonte) -> (dst)
        # (x=sy, y=sx) -> (src_h-1-sy, sx)... mapeia: dst_coluna=src_linha
        # reference: img[(nYY*ow+nxx)] com nxx=h-1-y, nYY=x  => nYY=dst_linha=x_fonte=sy
        # logo dst_linha = sy? nao, nYY=x e x itera nyYY(linhas dst)=0..255, x e a coord de fonte
        # Simplificado correcto: dst[dy][dx] = src[linha=dst_h-1-dy? ]
        pass
    # placeholder
# refaz com a formula da referencia (rot_cw) directamente
ow,oh=src_height,src_width
img=bytearray(ow*oh*4)
for sy_src in range(src_height):
    for sx_src in range(src_width):
        nxx=src_height-1-sy_src; nYY=sx_src
        img[(nYY*ow+nxx)*4:(nYY*ow+nxx)*4+4]=px[(sy_src*src_width+sx_src)*4:(sy_src*src_width+sx_src)*4+4]
fb=bytearray(fb_w*fb_h*2)
for y in range(min(oh,fb_h)):
    for x in range(min(ow,fb_w)):
        p=img[(y*ow+x)*4:(y*ow+x)*4+4]
        r5,g6,b5=(p[2]>>3)&31,(p[1]>>2)&63,(p[0]>>3)&31
        struct.pack_into('<H',fb,((oy+y)*fb_w+(ox+x))*2,(r5<<11)|(g6<<5)|b5)
# painel CCW
SW,SH=fb_w,fb_h; seen=bytearray(SW*SH*4)
for y in range(SH):
    for x in range(SW):
        v=struct.unpack_from('<H',fb,(y*SW+x)*2)[0]
        r=(v>>11)&31; g=(v>>5)&63; b=v&31
        nxx,nYY=SW-1-y,x
        seen[(nYY*SW+nxx)*4:(nYY*SW+nxx)*4+4]=bytes([(b*255)//31,(g*255)//63,(r*255)//31,255])
out_w,out_h=400,240; rgb=bytearray(out_w*out_h*3)
dw=min(SW,out_w); dh=min(SH,out_h); ox2=(out_w-dw)//2; oy2=(out_h-dh)//2
for y in range(dh):
    for x in range(dw):
        o=(y*SW+x)*4; q=((oy2+y)*out_w+(ox2+x))*3
        rgb[q:q+3]=seen[o:o+3]
write_png('debug/PAINEL_FINAL.png',out_w,out_h,bytes(rgb))
print(f"PAINEL_FINAL.png  footprint {ow}x{oh} rot_cw -> painel {out_w}x{out_h}")
