#include "mira.h"
using U=unsigned;
using Byte=unsigned char;
static_assert(sizeof(void*)==4);
extern "C" void* dlopen(const char*,int);
extern "C" void* dlsym(void*,const char*);
#define EXPORT extern "C" __attribute__((visibility("default")))
struct Loader {
 virtual const char* GetCurrentGame();
 virtual const char* GetConfigPath();
 virtual bool HasMod(const char*);
 virtual bool HasModOfVersion(const char*,const char*);
 virtual U GetLib(const char*);
 virtual U GetSym(void*,const char*);
 virtual bool Hook(void*,void*,void**);
};
struct Info {char id[48],name[48],version[24],author[48];unsigned short parts[4];};
static Info info={"mod.whiffyrabbit156.lineademira","Linea de Mira Android","2.1.1","whiffyrabbit156",{2,1,1,0}};
EXPORT Info* __GetModInfo(){return &info;}
EXPORT const char* __INeedASpecificGame(){return "com.rockstargames.gtasa";}
template<class T> static T& field(void* p,U n){return *reinterpret_cast<T*>(static_cast<Byte*>(p)+n);}
template<class T> static T sym(void* h,const char*n){return reinterpret_cast<T>(dlsym(h,n));}
static void **threads,**sceneData;
static Byte *cutscene,*mirror;
static void *(*player)(int),*(*vehicle)(int,bool),*(*hierarchy)(void*);
static bool (*alive)(void*);
static int (*indexOf)(void*,int),*(*local)(void*,U);
static int (*skill)(void*);
static void* (*weaponInfo)(int,signed char);
static float *timeStep;
static Matrix *(*bones)(void*);
static void *(*dirty)(void*),*(*updateMatrix)(Matrix*);
using Each=void*(*)(void*,void*);
static void* (*atomics)(void*,Each,void*);
static void (*idleOriginal)(void*,bool);
static int (*beginOriginal)(void*);
// The stock crosshair is a separate HUD pass. Hide it for the same on-foot
// weapon set covered by Reticula_Centro.csa; that CLEO script draws our own
// centered reticle. Keep the original pass intact for vehicles and other tools.
static void (*crosshairOriginal)();
static bool customReticleReady;
static bool (*fireOriginal)(void*,void*,Vec*,Vec*,void*,Vec*,Vec*);
static bool fireAimReady;
static void* (*setNear)(void*,float);
static float previousNear;
static bool clipped;
static U gameBase;
static bool inside;
static void* savedFrame;
static Matrix savedMatrix;
static char reportPath[512];
static void* (*openFile)(const char*,const char*);
static int (*printFile)(void*,const char*,...),(*closeFile)(void*);
static const char* lastStatus;
static int lastWeapon=-1,events;
static void report(const char* state,int id=0){
 if(state==lastStatus && id==lastWeapon)return;
 lastStatus=state;lastWeapon=id;
 if(!reportPath[0] || events>=150)return;
 if(void*f=openFile(reportPath,events?"a":"w")){
  printFile(f,"%s | arma %d\n",state,id);closeFile(f);++events;
 }
}
static void prepareReport(const char*dir){
 void* c=dlopen("libc.so",2);if(!c||!dir)return;
 openFile=sym<decltype(openFile)>(c,"fopen");closeFile=sym<decltype(closeFile)>(c,"fclose");
 printFile=sym<decltype(printFile)>(c,"fprintf");
 auto format=sym<int(*)(char*,U,const char*,...)>(c,"snprintf");
 if(openFile&&closeFile&&printFile&&format){
  int n=format(reportPath,sizeof(reportPath),"%s/LineaDeMira_estado.txt",dir);
  if(n<=0||n>=int(sizeof(reportPath)))reportPath[0]=0;
 }
}
static void* activeScript(){
 void*t=*threads;
 for(int count=0;t && count<512;++count,t=field<void*>(t,0)){
  const char*name=&field<char>(t,8);
  constexpr char tag[]="fpsg2";
  int n=0;while(n<5 && (name[n]|32)==tag[n])++n;
  if(n==5 && name[5]==0)return t;
 }
 return nullptr;
}
struct Pick{void*root;void*flash;const Vec*vertices;int count;Matrix transform;bool child;};
static bool modelTransform(void*f,const Pick&p,Matrix&out){
 out={};out.x.x=out.y.y=out.z.z=1.f;
 for(int i=0;i<8 && f && f!=p.root;++i){
  if(f==p.flash)return false;
  const Matrix&m=field<Matrix>(f,0x10);
  auto vector=[&](Vec a){return sum(mul(m.x,a.x),sum(mul(m.y,a.y),mul(m.z,a.z)));};
  out.x=vector(out.x);out.y=vector(out.y);out.z=vector(out.z);out.pos=point(m,out.pos);
  f=field<void*>(f,4);
 }
 return f==p.root;
}
static void* findMesh(void* a,void* context){
 Pick& p=*static_cast<Pick*>(context);
 if(!a)return a;
 void*g=field<void*>(a,0x18);
 if(!g||field<Byte>(g,0)!=8||field<int>(g,0x18)<1)return a;
 int n=field<int>(g,0x14);void*m=field<void*>(g,0x60);
 if(!m||n<=p.count||n>65535)return a;
 const Vec*v=field<Vec*>(m,0x14);
 void*f=field<void*>(a,4);Matrix transform;
 if(v && modelTransform(f,p,transform)){p.vertices=v;p.count=n;p.transform=transform;p.child=f!=p.root;}
 return a;
}
// Model-local corrections: positive lateral numbers move the point to screen right
// on an upright GTA weapon (local -Y). Units are game-world metres.
struct Trim {float rearRight,rearUp,frontRight,frontUp;};
static Trim trims[12]={{},{.031f,-.048f,0,-.009f},{},{},{},{},{},{},
                         {-.044f,-.045f,0,-.009f},{},{},{}};
static void loadTrims(const char*dir){
 if(!dir||!openFile||!closeFile)return;
 void*c=dlopen("libc.so",2);if(!c)return;
 auto fmt=sym<int(*)(char*,U,const char*,...)>(c,"snprintf");
 auto line=sym<char*(*)(char*,int,void*)>(c,"fgets");
 auto parse=sym<int(*)(const char*,const char*,...)>(c,"sscanf");
 if(!fmt||!line||!parse)return;
 char buf[512];int n=fmt(buf,sizeof(buf),"%s/LineaDeMira.ini",dir);
 if(n<=0||n>=int(sizeof(buf)))return;
 void*f=openFile(buf,"r");if(!f)return;
 while(line(buf,sizeof(buf),f)){
  Trim t;int id;
  if(parse(buf,"%d %f %f %f %f",&id,&t.rearRight,&t.rearUp,&t.frontRight,&t.frontUp)!=5 || id<22||id>33)continue;
  if(__builtin_fabsf(t.rearRight)<=.15f&&__builtin_fabsf(t.rearUp)<=.15f&&
     __builtin_fabsf(t.frontRight)<=.15f&&__builtin_fabsf(t.frontUp)<=.15f)trims[id-22]=t;
 }
 closeFile(f);
}
struct Cache{void*weapon;void*root;void*flash;int id;Sight line;bool geometric;};
static Cache cache;
static bool marker(void*flash,void*root,Vec& position){
 if(!flash)return false;
 position={};
 for(int n=0;n<8 && flash && flash!=root;++n){
  position=point(field<Matrix>(flash,0x10),position);
  flash=field<void*>(flash,4);
 }
 return flash==root;
}
static bool meshTip(const Pick&p,Vec& tip){
 if(!p.vertices)return false;
 float end=0;
 auto vertex=[&](int i){return p.child?point(p.transform,p.vertices[i]):p.vertices[i];};
 for(int i=0;i<p.count;++i){float x=vertex(i).x;if(x>end)end=x;}
 if(!(end>.08f&&end<2.f))return false;
 float ymin=99,ymax=-99,zmin=99,zmax=-99;
 for(int i=0;i<p.count;++i){
  Vec a=vertex(i);if(!(a.x>=end*.95f&&a.x<=end))continue;
  if(a.y<ymin)ymin=a.y;
  if(a.y>ymax)ymax=a.y;
  if(a.z<zmin)zmin=a.z;
  if(a.z>zmax)zmax=a.z;
 }
 if(!(ymax>=ymin&&zmax>=zmin&&ymax-ymin<.16f&&zmax-zmin<.20f))return false;
 tip={end,(ymin+ymax)*.5f,(zmin+zmax)*.5f};return true;
}
static bool getLine(void*weapon,void*root,void*flash,int id){
 if(cache.weapon==weapon&&cache.root==root&&cache.flash==flash&&cache.id==id)return true;
 Pick p;p.root=root;p.flash=flash;p.vertices=nullptr;p.count=0;p.child=false;atomics(weapon,findMesh,&p);
 Vec muzzle;Sight s;
 if(!marker(flash,root,muzzle)||!fallback(id,muzzle,s)){
  if(!meshTip(p,muzzle)||!fallback(id,muzzle,s))return false;
 }
 bool geometric=p.vertices&&meshSight(p.vertices,p.count,muzzle,s,p.child?&p.transform:nullptr);
 Trim trim=trims[id-22];
 s.rear.y-=trim.rearRight;s.rear.z+=trim.rearUp;
 s.front.y-=trim.frontRight;s.front.z+=trim.frontUp;
 cache={weapon,root,flash,id,s,geometric};return true;
}
static bool eligible(void*ped,int id){
 if(id<22 || id>33)return false;
 // Same flag tested before the stock renderer draws the left-hand copy.
 void*info=weaponInfo(id,static_cast<signed char>(skill(ped)));
 return info && !(field<Byte>(info,0x19)&8);
}
static bool firstPersonAiming(int id){
 if(*cutscene||*mirror)return false;
 void*ped=player(-1);
 if(!ped||vehicle(-1,false)||!eligible(ped,id))return false;
 void*t=activeScript();
 return t && *local(t,0)==1 && *local(t,31)>0 && *local(t,7)==id;
}
static int currentWeaponId(void*ped){
 if(!ped)return -1;
 int slot=field<Byte>(ped,0x71c);
 return slot<13?field<int>(ped,0x5a4+28*slot):-1;
}
// Keep the game hit test and muzzle effects, but make the target point follow
// the center ray shown by our custom reticle during supported first-person aim.
static bool fireFromCamera(void*weapon,void*shooter,Vec*start,Vec*barrel,void*target,Vec*targetPos,Vec*driveByPos){
 if(!fireOriginal)return false;
 void*ped=player(-1);
 int id=currentWeaponId(ped);
 if(fireAimReady && shooter==ped && start && targetPos && firstPersonAiming(id)){
  void*rw=sceneData?sceneData[1]:nullptr;
  void*f=rw?field<void*>(rw,4):nullptr;
  if(f && !field<void*>(f,4)){
   Matrix&camera=field<Matrix>(f,0x10);
   Vec forward=camera.z;
   if(normalize(forward)){
    Vec oldTarget=*targetPos;
    *targetPos=sum(*start,mul(forward,500.f));
    bool fired=fireOriginal(weapon,shooter,start,barrel,target,targetPos,driveByPos);
    *targetPos=oldTarget;
    return fired;
   }
  }
 }
 return fireOriginal(weapon,shooter,start,barrel,target,targetPos,driveByPos);
}
static bool customReticleActive(){
 if(!customReticleReady)return false;
 void*ped=player(-1);
 if(!ped||!alive(ped)||vehicle(-1,false))return false;
 int id=currentWeaponId(ped);
 return firstPersonAiming(id);
}
static void drawCrosshair(){
 if(customReticleActive())return;
 if(crosshairOriginal)crosshairOriginal();
}
static void apply(void*rw,void*f,const Matrix&out){
 Matrix&frame=field<Matrix>(f,0x10);
 savedMatrix=frame;savedFrame=f;frame=out;updateMatrix(&frame);dirty(f);
 previousNear=field<float>(rw,0x80);
 clipped=previousNear>.10f && previousNear<20.f;
 if(clipped)setNear(rw,.06f);
}
static bool closeTo(Vec a,Vec b,float radius){
 Vec d=delta(a,b);return dot(d,d)<radius*radius;
}
static void align(void*rw){
 if(*cutscene||*mirror||!rw||rw!=sceneData[1])return;
 void*t=activeScript();
 if(!t || *local(t,0)!=1){cache.weapon=nullptr;report("Primera persona desactivada");return;}
 void*ped=player(-1);
 if(!ped||!alive(ped))return;
 bool inCar=vehicle(-1,false)!=nullptr;
 int slot=field<Byte>(ped,0x71c);
 int id=slot<13?field<int>(ped,0x5a4+28*slot):-1;
 int permission=!inCar && eligible(ped,id)?id:0;
 int&last=*local(t,30),&zoom=*local(t,31);
 if(last!=permission||!permission){zoom=0;*local(t,8)=0;}
 last=permission;
 void*f=field<void*>(rw,4);
 if(!f||field<void*>(f,4))return;
 const Matrix&frame=field<Matrix>(f,0x10);
 void*body=field<void*>(ped,0x14);
 Vec origin=body?field<Matrix>(body,0).pos:field<Vec>(ped,4);
 Vec head=sum(origin,{0,0,.62f});
 void*clump=field<void*>(ped,0x18);
 void*h=clump&&field<Byte>(clump,0)==2?hierarchy(clump):nullptr;
 Matrix*m=nullptr;int count=0;
 if(h && field<void*>(h,16)){
  count=field<int>(h,4);
  if(count>0 && count<=256)m=bones(h);
 }
 if(m){
  int i=indexOf(h,5);
  if(U(i)<U(count) && closeTo(m[i].pos,origin,2.5f))head=m[i].pos;
 }
 bool aiming=permission && zoom>0 && id==*local(t,7);
 if(aiming){
  // Keep the camera's live axes from the game. Basing them on the animated
  // weapon sights made the view lag behind touch input and stay locked when
  // CJ moved. The Fire hook sends shots along this same camera axis.
  Matrix out=frame;
  Vec eye=sum(sum(head,mul(frame.z,.10f)),{0,0,.035f});
  if(closeTo(eye,head,1.25f)){
   out.pos=eye;apply(rw,f,out);
   report("Camara libre: reticula alineada",id);return;
  }
 }
 // Re-anchor every frame. This avoids carrying the prior aiming eye point into
 // movement, weapon changes, or release of the touch aim control.
 Vec ahead=frame.z;
 if(normalize(ahead)){
  Vec eye=sum(sum(head,mul(ahead,.10f)),{0,0,.035f});
  Matrix out=frame;out.pos=eye;apply(rw,f,out);
 }
 report(permission?"Primera persona a la altura de CJ":"Primera persona, control de mira bloqueado",id);
}
static void restore(){
 if(!savedFrame)return;
 void*rw=sceneData[1];
 if(rw&&field<void*>(rw,4)==savedFrame){
  field<Matrix>(savedFrame,0x10)=savedMatrix;
  dirty(savedFrame);
  if(clipped && field<float>(rw,0x80)==.06f)setNear(rw,previousNear);
 }
 savedFrame=nullptr;
}
static int beginFrame(void*rw){
 U caller=(reinterpret_cast<U>(__builtin_return_address(0))&~1u)-gameBase;
 if(inside&&!savedFrame&&(caller==0x3f5906||caller==0x3f6b00))align(rw);
 int ok=beginOriginal(rw);
 if(!ok)restore();
 return ok;
}
static void idle(void*data,bool render){
 if(inside){idleOriginal(data,render);return;}
 inside=true;idleOriginal(data,render);restore();inside=false;
}
EXPORT void OnModLoad(){
 void*a=dlopen("libAML.so",2),*g=dlopen("libGTASA.so",2);
 if(!a||!g)return;
 auto get=sym<void*(*)(const char*)>(a,"GetInterface");
 if(!get)return;
 auto aml=static_cast<Loader*>(get("AMLInterface"));if(!aml)return;
 prepareReport(aml->GetConfigPath());
 loadTrims(aml->GetConfigPath());
 void*idleFn=dlsym(g,"_Z4IdlePvb"),*beginFn=dlsym(g,"_Z19psCameraBeginUpdateP8RwCamera");
 void*cam=dlsym(g,"TheCamera");
 if(!cam)return;
 gameBase=U(cam)-0x951fa8;
 if(U(idleFn)-gameBase!=0x3f68f9||U(beginFn)-gameBase!=0x5e4d95){report("Edicion de GTA distinta");return;}
#define BIND(v,n) v=sym<decltype(v)>(g,n);if(!v){report("Falta " n);return;}
 BIND(player,"_Z13FindPlayerPedi")
 BIND(vehicle,"_Z17FindPlayerVehicleib")
 BIND(alive,"_ZNK4CPed7IsAliveEv")
 BIND(hierarchy,"_Z29GetAnimHierarchyFromSkinClumpP7RpClump")
 BIND(skill,"_ZN4CPed14GetWeaponSkillEv")
 BIND(weaponInfo,"_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponTypea")
 BIND(timeStep,"_ZN6CTimer12ms_fTimeStepE")
 BIND(bones,"_Z30RpHAnimHierarchyGetMatrixArrayP16RpHAnimHierarchy")
 BIND(indexOf,"_Z17RpHAnimIDGetIndexP16RpHAnimHierarchyi")
 BIND(dirty,"_Z20RwFrameUpdateObjectsP7RwFrame")
 BIND(updateMatrix,"_Z14RwMatrixUpdateP11RwMatrixTag")
 BIND(setNear,"_Z24RwCameraSetNearClipPlaneP8RwCameraf")
 BIND(atomics,"_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_")
 BIND(local,"_ZN14CRunningScript25GetPointerToLocalVariableEj")
 BIND(threads,"_ZN11CTheScripts14pActiveScriptsE")
 BIND(sceneData,"Scene")
 BIND(cutscene,"_ZN12CCutsceneMgr10ms_runningE")
 BIND(mirror,"_ZN8CMirrors20bRenderingReflectionE")
 if(!aml->Hook(idleFn,(void*)idle,(void**)&idleOriginal)||!aml->Hook(beginFn,(void*)beginFrame,(void**)&beginOriginal)){
  report("AML no pudo registrar la camara");return;
 }
 void* fireFn=dlsym(g,"_ZN7CWeapon4FireEP7CEntityP7CVectorS3_S1_S3_S3_");
 if(fireFn){
  fireAimReady=aml->Hook(fireFn,(void*)fireFromCamera,(void**)&fireOriginal);
  report(fireAimReady?"Disparo centrado en la reticula":"No se pudo alinear el disparo");
 }else{
  report("Simbolo de disparo no encontrado");
 }
 void* crosshairFn=dlsym(g,"_ZN4CHud14DrawCrossHairsEv");
 if(crosshairFn){
  customReticleReady=aml->Hook(crosshairFn,(void*)drawCrosshair,(void**)&crosshairOriginal);
  report(customReticleReady?"Reticula propia activa":"No se pudo sustituir la reticula");
 }else{
  report("Simbolo de reticula original no encontrado");
 }
 report("Modulo cargado, esperando partida");
}
