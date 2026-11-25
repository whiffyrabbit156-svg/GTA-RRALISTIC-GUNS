#pragma once
struct Vec { float x,y,z; };
struct Matrix { Vec x; unsigned flags; Vec y; unsigned pad1; Vec z; unsigned pad2; Vec pos; unsigned pad3; };
static_assert(sizeof(Matrix)==64);
inline Vec sum(Vec a,Vec b){ return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline Vec mul(Vec a,float b){ return {a.x*b,a.y*b,a.z*b}; }
inline Vec delta(Vec a,Vec b){ return sum(a,mul(b,-1)); }
inline float dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec cross(Vec a,Vec b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline bool normalize(Vec &v){
 float n=dot(v,v);
 if(!(n>.000001f && n<10000)) return false;
 v=mul(v,1.f/__builtin_sqrtf(n));return true;
}
inline Vec point(const Matrix& m,Vec p){
 return sum(m.pos,sum(mul(m.x,p.x),sum(mul(m.y,p.y),mul(m.z,p.z))));
}
struct Sight{Vec rear,front;float eyeDistance;};
struct Profile{float above,eye;};
// Sight height above the flash marker when no indexed mesh is available, and eye relief.
constexpr Profile profiles[]={
 {.030f,.255f},{.032f,.260f},{.042f,.270f},{.032f,.240f},{.025f,.245f},
 {.046f,.235f},{.035f,.235f},{.049f,.240f},{.053f,.226f},{.076f,.230f},
 {.035f,.235f},{.045f,.245f},{.084f,.265f}
};
inline bool fallback(int id,Vec muzzle,Sight& s){
 if(id<22||id>34||!(muzzle.x>.08f&&muzzle.x<2.f&&__builtin_fabsf(muzzle.y)<.4f&&__builtin_fabsf(muzzle.z)<.6f))return false;
 Profile p=profiles[id-22];float h=muzzle.z+p.above;
 s={{muzzle.x*.13f,muzzle.y,h},{muzzle.x*.86f,muzzle.y,h},p.eye};return true;
}
// Select two separate crests along the top of the mesh, rejecting off-centre accessories.
// Grip and flash atomics are never used as a reference for vertical alignment.
inline bool meshSight(const Vec* v,int n,Vec muzzle,Sight& s,const Matrix*toRoot=nullptr){
 struct Peak{float z=-100;float xmin=99,xmax=-99,ymin=99,ymax=-99;int count=0;};
 Peak peaks[2];
 auto vertex=[&](int i){return toRoot?point(*toRoot,v[i]):v[i];};
 float end=0;for(int i=0;i<n;++i){float x=vertex(i).x;if(x>end)end=x;}
 if(!(end>.08f&&end<2.1f))return false;
 for(int pass=0;pass<2;++pass){
  for(int i=0;i<n;++i){
   Vec p=vertex(i);float f=p.x/end;
   if(!(p.z>=muzzle.z-.14f && p.z<=muzzle.z+.19f && __builtin_fabsf(p.y-muzzle.y)<=.052f)) continue;
   int k;
   if(f>=-.08f && f<=.43f) k=0;
   else if(f>=.60f && f<=1.03f) k=1;
   else continue;
   Peak& a=peaks[k];
   if(pass==0){if(p.z>a.z)a.z=p.z;}
   else if(p.z>=a.z-.003f){if(p.x<a.xmin)a.xmin=p.x;if(p.x>a.xmax)a.xmax=p.x;
    if(p.y<a.ymin)a.ymin=p.y;if(p.y>a.ymax)a.ymax=p.y;++a.count;}
  }
 }
 if(!peaks[0].count || !peaks[1].count)return false;
 Vec p[2];for(int i=0;i<2;++i){const Peak&a=peaks[i];p[i]={(a.xmin+a.xmax)*.5f,(a.ymin+a.ymax)*.5f,a.z};}
 float span=p[1].x-p[0].x;
 if(!(span>.05f && __builtin_fabsf(p[0].z-p[1].z)<.36f*span && __builtin_fabsf(p[0].y-p[1].y)<.28f*span))return false;
 s.rear=p[0];s.front=p[1];return true;
}
// RenderWare camera axes are right, up, at. Preserve the original handedness.
inline bool place(const Matrix& hand,const Sight&s,const Matrix& previous,Matrix& out){
 Vec rear=point(hand,s.rear),front=point(hand,s.front),forward=delta(front,rear);
 Vec up=hand.z;
 if(!normalize(forward)||!normalize(up))return false;
 Vec side=cross(up,forward);
 if(!normalize(side))return false;
 up=cross(forward,side);
 float handed=dot(cross(previous.x,previous.y),previous.z);
 if(!( __builtin_fabsf(handed)>.80f && __builtin_fabsf(handed)<1.20f))return false;
 out=previous;out.x=mul(side,handed<0?-1.f:1.f);out.y=up;out.z=forward;
 out.pos=sum(rear,mul(forward,-s.eyeDistance));
 return true;
}
