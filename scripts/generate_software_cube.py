#!/usr/bin/env python3
import math, pathlib, sys
FRAMES,FOCAL=48,64.0
VERTS=[(-1,-1,-1),(1,-1,-1),(-1,1,-1),(1,1,-1),(-1,-1,1),(1,-1,1),(-1,1,1),(1,1,1)]
FACES=[(0,2,3,1),(4,5,7,6),(0,1,5,4),(2,6,7,3),(0,4,6,2),(1,3,7,5)];UV=((0,0),(31,0),(31,31),(0,31))
NORMALS=((0,0,-1),(0,0,1),(0,-1,0),(0,1,0),(-1,0,0),(1,0,0))
def rot(v,a):
 ay,ax=a,a*.73;cy,sy,cx,sx=math.cos(ay),math.sin(ay),math.cos(ax),math.sin(ax);x,y,z=v;y,z=y*cx-z*sx,y*sx+z*cx
 return x*cy+z*sy,y,-x*sy+z*cy+4
def plane(ps,vs):
 (x0,y0),(x1,y1),(x2,y2)=ps;a0,a1,a2=vs;d=(x1-x0)*(y2-y0)-(y1-y0)*(x2-x0)
 ax=((a1-a0)*(y2-y0)-(a2-a0)*(y1-y0))/d;ay=((x1-x0)*(a2-a0)-(x2-x0)*(a1-a0))/d
 return tuple(round(v*16) for v in (ax,ay,a0-ax*x0-ay*y0))
def emit(path):
 out=["/* Generated fixed-point software raster data. */","#ifndef SOFTWARE_CUBE_DATA_H","#define SOFTWARE_CUBE_DATA_H",f"enum{{SW_FRAME_COUNT={FRAMES}}};","static const uint32_t reciprocal_q16[2048]={",",".join("0" if q==0 else str((1<<16)//q) for q in range(2048)),"};","static const SoftwareFrame sw_frames[SW_FRAME_COUNT]={"]
 for fi in range(FRAMES):
  rv=[rot(v,2*math.pi*fi/FRAMES) for v in VERTS];pv=[(60+FOCAL*x/z,40+FOCAL*y/z,z) for x,y,z in rv];tris=[]
  for face,ids in enumerate(FACES):
   nx,ny,nz=rot(NORMALS[face],2*math.pi*fi/FRAMES);nz-=4
   cx,cy,cz=rot(NORMALS[face],2*math.pi*fi/FRAMES)
   if nx*cx+ny*cy+nz*cz>=0:continue
   for cs in ((0,1,2),(0,2,3)):
    vs=[]
    for ci in cs:
     vi=ids[ci];x,y,z=pv[vi];u,v=UV[ci];q=1/z;vs.append((x,y,q*4096,u*q*4096,v*q*4096))
    area=(vs[1][0]-vs[0][0])*(vs[2][1]-vs[0][1])-(vs[1][1]-vs[0][1])*(vs[2][0]-vs[0][0])
    if abs(area)<=1.0:continue
    if area>0:vs[1],vs[2]=vs[2],vs[1]
    ps=[(v[0],v[1]) for v in vs];coords=[n for v in vs for n in (round(v[0]*16),round(v[1]*16))];coeff=[n for a in range(2,5) for n in plane(ps,[v[a] for v in vs])];tris.append((face,coords,coeff))
  out.append(f"{{{len(tris)},{{")
  for face,coords,coeff in tris:out.append("{%d,{%s},{%s}},"%(face,",".join(map(str,coords)),",".join(map(str,coeff))))
  out.extend(["{0,{0},{0}},"]*(12-len(tris)));out.append("}},")
 out += ["};","#endif"];pathlib.Path(path).write_text("\n".join(out)+"\n")
if __name__=="__main__":emit(sys.argv[1])
