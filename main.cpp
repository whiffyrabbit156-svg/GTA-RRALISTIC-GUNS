// RealWeapons - AML mod para GTA SA Android 2.00 (arm64)
// Balas fisicas (proyectiles), penetracion, rebote, dispersion por cadencia,
// retroceso (sacudida de camara + ascenso de punteria) y mas dano por arma.
//
// Los simbolos se buscan por nombre en libGTASA.so (no usa direcciones fijas).
// Todo lo que no se encuentra o no coincide con la firma esperada se DESACTIVA
// y queda escrito en RealWeapons.log (envialo si algo no funciona).

#include <mod/amlmod.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include <string>

MYMOD(net.realweapons.gtasa, RealWeapons, 1.0, RealWeapons)
BEGIN_DEPLIST()
END_DEPLIST()

// ------------------------------- AJUSTES -----------------------------------
#define ENABLE_PROJECTILES 1   // balas fisicas (no hitscan) para el jugador
#define ENABLE_PIERCE      1   // atravesar enemigos / objetos
#define ENABLE_BOUNCE      1   // rebote en angulos rasantes (opcional)
#define ENABLE_TRACER      1   // linea visible de la bala
#define ENABLE_DAMAGE      1   // multiplicar dano por arma
#define ENABLE_SHAKE       1   // sacudida de camara al disparar
static const float GRAVITY        = 9.81f;
static const float MAX_RANGE      = 300.0f;
static const float HEAT_MAX       = 3.0f;
static const float HEAT_DECAY     = 1.6f;   // por segundo
static const float CLIMB_MAX      = 8.0f;   // grados
static const float CLIMB_DECAY    = 6.0f;   // grados por segundo
static const float PIERCE_FALLOFF = 0.7f;   // dano restante tras atravesar
static const int   MAX_BULLETS    = 160;
// ---------------------------------------------------------------------------

struct V { float x, y, z; };
static inline V   operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V   operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V   operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float len(V a) { return sqrtf(dot(a, a)); }
static inline V   norm(V a) { float l = len(a); return l > 1e-6f ? a * (1.0f / l) : V{0, 0, 1}; }
static inline V   cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

struct alignas(16) ColPoint { float raw[16]; };  // CColPoint: punto en [0..2], normal en [4..6]
static inline V cpPoint(const ColPoint& c) { return {c.raw[0], c.raw[1], c.raw[2]}; }
static inline V cpNormal(const ColPoint& c) { return {c.raw[4], c.raw[5], c.raw[6]}; }

// id, mult. dano, sacudida, dispersion(grados), calor/disparo, velocidad(m/s), presupuesto de penetracion
struct WP { int id; float dmg; float shake; float spread; float heat; float speed; int pierce; };
static const WP WPS[] = {
    {22, 1.6f, 0.030f, 0.6f, 0.35f, 350.f, 0},  // pistola
    {23, 1.5f, 0.020f, 0.5f, 0.30f, 330.f, 0},  // silenciada
    {24, 1.5f, 0.120f, 1.2f, 0.90f, 450.f, 4},  // desert eagle
    {25, 1.4f, 0.140f, 2.2f, 1.10f, 380.f, 2},  // escopeta
    {26, 1.3f, 0.180f, 3.0f, 1.30f, 360.f, 1},  // recortada
    {27, 1.4f, 0.130f, 1.8f, 1.00f, 380.f, 2},  // escopeta de combate
    {28, 1.3f, 0.030f, 1.2f, 0.30f, 380.f, 0},  // uzi
    {29, 1.4f, 0.030f, 0.8f, 0.25f, 400.f, 0},  // mp5
    {32, 1.3f, 0.030f, 1.4f, 0.30f, 340.f, 0},  // tec-9
    {30, 1.5f, 0.060f, 1.0f, 0.40f, 715.f, 4},  // ak-47
    {31, 1.4f, 0.050f, 0.8f, 0.35f, 900.f, 4},  // m4
    {33, 2.0f, 0.080f, 0.5f, 1.00f, 850.f, 6},  // rifle
    {34, 2.5f, 0.140f, 0.3f, 1.50f, 950.f, 8},  // sniper
};
static const WP* findWP(int id) {
    for (const WP& w : WPS) if (w.id == id) return &w;
    return nullptr;
}

// ------------------------------- LOG ---------------------------------------
static FILE* g_log = nullptr;
static std::string g_dir;
static bool g_dirTried = false;
static void pickDir() {
    g_dirTried = true;
    static const char* DIRS[] = {"/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/",
                                 "/sdcard/Android/data/com.rockstargames.gtasa/files/",
                                 "/storage/emulated/0/Android/data/com.rockstargames.gtasa/"};
    for (const char* d : DIRS) {
        FILE* f = fopen((std::string(d) + "RealWeapons.log").c_str(), "w");
        if (f) { g_log = f; g_dir = d; return; }
    }
}
static void LOG(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "RealWeapons", "%s", buf);
    if (!g_dirTried) pickDir();
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

// RealWeapons.cfg (opcional, una linea por opcion):  frame_hook=game|hud|timer|idle   projectiles=0|1   damage=0|1
static std::string g_cfgHook = "game";
static int g_cfgProj = 1, g_cfgDmg = 1;
static void readCfg() {
    if (g_dir.empty()) return;
    FILE* f = fopen((g_dir + "RealWeapons.cfg").c_str(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char k[64] = {0}, v[64] = {0};
        if (sscanf(line, " %63[^= ] = %63s", k, v) == 2) {
            if (!strcmp(k, "frame_hook")) g_cfgHook = v;
            else if (!strcmp(k, "projectiles")) g_cfgProj = atoi(v);
            else if (!strcmp(k, "damage")) g_cfgDmg = atoi(v);
        }
    }
    fclose(f);
}
// Guardia de arranque: si el juego se cuelga durante el arranque, la siguiente vez el mod
// no se engancha a nada (el juego arranca normal) hasta que borres RealWeapons.boot.
static std::string bootPath() { return g_dir + "RealWeapons.boot"; }
static bool bootGuardTripped() {
    FILE* f = fopen(bootPath().c_str(), "r");
    if (f) { fclose(f); return true; }
    return false;
}
static void bootGuardArm() { FILE* f = fopen(bootPath().c_str(), "w"); if (f) { fputs("arrancando", f); fclose(f); } }
static void bootGuardClear() { remove(bootPath().c_str()); }

// ------------------------- BUSCADOR DE SIMBOLOS ----------------------------
struct Sym { std::string name; uintptr_t addr; };
static std::vector<Sym> g_syms;

static int phdrCb(struct dl_phdr_info* info, size_t, void*) {
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libGTASA.so")) return 0;
    uintptr_t base = info->dlpi_addr;
    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) { dyn = (const ElfW(Dyn)*)(base + info->dlpi_phdr[i].p_vaddr); break; }
    if (!dyn) return 1;
    auto fix = [&](uintptr_t p) { return p >= base ? p : base + p; };
    const ElfW(Sym)* symtab = nullptr; const char* strtab = nullptr;
    const uint32_t* hash = nullptr; const uint32_t* gnu = nullptr;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_SYMTAB) symtab = (const ElfW(Sym)*)fix(d->d_un.d_ptr);
        else if (d->d_tag == DT_STRTAB) strtab = (const char*)fix(d->d_un.d_ptr);
        else if (d->d_tag == DT_HASH) hash = (const uint32_t*)fix(d->d_un.d_ptr);
        else if (d->d_tag == DT_GNU_HASH) gnu = (const uint32_t*)fix(d->d_un.d_ptr);
    }
    if (!symtab || !strtab) return 1;
    uint32_t count = 0;
    if (hash) count = hash[1];
    else if (gnu) {
        uint32_t nb = gnu[0], off = gnu[1], bloom = gnu[2];
        const uint32_t* buckets = gnu + 4 + bloom * 2;  // bloom de 64 bits = 2 palabras
        const uint32_t* chain = buckets + nb;
        uint32_t mx = 0;
        for (uint32_t i = 0; i < nb; i++) if (buckets[i] > mx) mx = buckets[i];
        if (mx < off) count = off;
        else { while (!(chain[mx - off] & 1)) mx++; count = mx + 1; }
    } else if ((uintptr_t)strtab > (uintptr_t)symtab) {
        count = ((uintptr_t)strtab - (uintptr_t)symtab) / sizeof(ElfW(Sym));
    }
    if (count > 400000) count = 400000;
    for (uint32_t i = 0; i < count; i++) {
        const ElfW(Sym)& s = symtab[i];
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0 || !s.st_name) continue;
        g_syms.push_back({strtab + s.st_name, base + s.st_value});
    }
    return 1;
}
static const Sym* findPrefix(const char* prefix, const char* mustContain = nullptr) {
    size_t n = strlen(prefix);
    for (const Sym& s : g_syms)
        if (s.name.compare(0, n, prefix) == 0 && (!mustContain || s.name.find(mustContain) != std::string::npos)) return &s;
    return nullptr;
}
static bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// --------------------------- FUNCIONES DEL JUEGO ---------------------------
typedef bool  (*FIH_t)(void*, void*, V*, V*, void*, V*, V*, bool, bool);
typedef bool  (*LOS_t)(const V&, const V&, ColPoint&, void*&, bool, bool, bool, bool, bool, bool, bool, bool);
typedef void* (*FindPlayerPed_t)(int);
typedef void* (*GetWeaponInfo_t)(int, int);
typedef void  (*CamShake_t)(void*, float, float, float, float);
typedef void  (*AddTrace_t)(V*, V*, float, uint32_t, uint8_t);
typedef void  (*Idle_t)(void*, bool);

static FIH_t           orig_FIH = nullptr;
static Idle_t          orig_Idle = nullptr;
static LOS_t           g_LOS = nullptr;
static FindPlayerPed_t g_FindPlayerPed = nullptr;
static GetWeaponInfo_t g_GetWeaponInfo = nullptr;
static CamShake_t      g_CamShake = nullptr;
static AddTrace_t      g_AddTrace = nullptr;
static void*           g_TheCamera = nullptr;
static void**          g_pIgnore = nullptr;     // CWorld::pIgnoreEntity
static bool g_proj = false, g_dmgOK = false;
static bool g_replay = false;

// ------------------------------- ESTADO ------------------------------------
struct Bullet {
    V pos, vel;
    float traveled, scale, life;
    int type, pierceLeft, bouncesLeft;
    void* weapon; void* shooter;
};
static std::vector<Bullet> g_bullets;
static float  g_heat = 0, g_climb = 0;
static double g_lastShot = 0, g_lastTick = 0;
static unsigned g_rng = 2463534242u;
static float frand() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng & 0xFFFFFF) / 16777216.0f; }
static double nowSec() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ----------------------- DANO (CWeaponInfo, layout PC) ---------------------
struct InfoPatch { int type; uint8_t* info; uint16_t base; };
static std::vector<InfoPatch> g_infos;
static const int OFF_DAMAGE = 0x22;  // uint16 m_nDamage
static uint16_t& dmgOf(uint8_t* info) { return *(uint16_t*)(info + OFF_DAMAGE); }

static void applyDamage() {
    if (!g_GetWeaponInfo) return;
    // validacion con la pistola: debe parecer un CWeaponInfo valido
    uint8_t* pist = (uint8_t*)g_GetWeaponInfo(22, 1);
    if (!pist) { LOG("DANO: GetWeaponInfo(22) devolvio null, desactivado"); return; }
    int fire = *(int*)pist; float tr = *(float*)(pist + 4), wr = *(float*)(pist + 8); uint16_t d = dmgOf(pist);
    LOG("DANO: pistola fire=%d targetRange=%.1f weaponRange=%.1f damage=%u", fire, tr, wr, d);
    if (fire != 1 || tr < 5 || tr > 400 || wr < 5 || wr > 800 || d < 1 || d > 1000) {
        LOG("DANO: el layout de CWeaponInfo no coincide, desactivado para no corromper memoria");
        return;
    }
    for (const WP& w : WPS) {
        for (int skill = 0; skill < 4; skill++) {
            uint8_t* info = (uint8_t*)g_GetWeaponInfo(w.id, skill);
            if (!info) continue;
            bool dup = false;
            for (auto& p : g_infos) if (p.info == info) dup = true;
            if (dup) continue;
            uint16_t base = dmgOf(info);
            if (base < 1 || base > 1000) continue;
            uint32_t nv = (uint32_t)(base * w.dmg + 0.5f);
            if (nv > 65000) nv = 65000;
            dmgOf(info) = (uint16_t)nv;
            g_infos.push_back({w.id, info, (uint16_t)nv});
            LOG("DANO: arma %d skill %d: %u -> %u", w.id, skill, base, nv);
        }
    }
    g_dmgOK = !g_infos.empty();
}

// --------------------------- SIMULACION DE BALAS ---------------------------
static void impactDamage(Bullet& b, V P, V dir) {
    if (!orig_FIH) return;
    V s = P - dir * 0.3f, e = P + dir * 0.15f;
    int* wt = (int*)b.weapon;
    int oldType = *wt;
    *wt = b.type;
    if (g_dmgOK && b.scale != 1.0f)
        for (auto& p : g_infos) if (p.type == b.type) { uint32_t v = (uint32_t)(p.base * b.scale + 0.5f); dmgOf(p.info) = v < 1 ? 1 : (uint16_t)v; }
    g_replay = true;
    orig_FIH(b.weapon, b.shooter, &s, &s, nullptr, &e, nullptr, false, false);
    g_replay = false;
    if (g_dmgOK && b.scale != 1.0f)
        for (auto& p : g_infos) if (p.type == b.type) dmgOf(p.info) = p.base;
    *wt = oldType;
}

// clase: 0 ped, 1 vehiculo, 2 objeto, 3 edificio
static bool traceNearest(V a, V b, void* ignore, ColPoint& out, int& cls) {
    static const bool F[4][5] = {  // buildings, vehicles, peds, objects, dummies
        {false, false, true,  false, false},
        {false, true,  false, false, false},
        {false, false, false, true,  false},
        {true,  false, false, false, true}};
    bool hit = false; float best = 1e30f;
    if (g_pIgnore) *g_pIgnore = ignore;
    for (int c = 0; c < 4; c++) {
        ColPoint cp; memset(&cp, 0, sizeof(cp)); void* ent = nullptr;
        if (g_LOS(a, b, cp, ent, F[c][0], F[c][1], F[c][2], F[c][3], F[c][4], true, false, true)) {
            V d = cpPoint(cp) - a; float l = dot(d, d);
            if (l < best) { best = l; out = cp; cls = c; hit = true; }
        }
    }
    if (g_pIgnore) *g_pIgnore = nullptr;
    return hit;
}

static int pierceCost(int cls) { return cls == 1 || cls == 3 ? 3 : 1; }

static void simulate(float dt) {
    for (size_t i = 0; i < g_bullets.size();) {
        Bullet& b = g_bullets[i];
        bool dead = false;
        b.life += dt;
        b.vel.z -= GRAVITY * dt;
        V step = b.vel * dt;
        V to = b.pos + step;
        ColPoint cp; int cls = 0;
        if (traceNearest(b.pos, to, b.shooter, cp, cls)) {
            V P = cpPoint(cp), N = cpNormal(cp), D = norm(b.vel);
            impactDamage(b, P, D);
            float cost = (float)pierceCost(cls);
#if ENABLE_PIERCE
            if (b.pierceLeft >= (int)cost) {
                b.pierceLeft -= (int)cost;
                b.scale *= PIERCE_FALLOFF;
                b.vel = b.vel * 0.85f;
                b.pos = P + D * 0.12f;
                b.traveled += len(P - b.pos);
                ++i; continue;
            }
#endif
#if ENABLE_BOUNCE
            if ((cls == 1 || cls == 3) && b.bouncesLeft > 0 && fabsf(dot(D, N)) < 0.35f && b.type != 25 && b.type != 26 && b.type != 27) {
                float dn = dot(D, N);
                V R = norm(D - N * (2.0f * dn));
                b.vel = R * (len(b.vel) * 0.6f);
                b.pos = P + N * 0.06f;
                b.scale *= 0.5f;
                b.bouncesLeft--;
                ++i; continue;
            }
#endif
            dead = true;
        } else {
#if ENABLE_TRACER
            if (g_AddTrace) { V a = b.pos, c = to; g_AddTrace(&a, &c, 0.02f, 70, 170); }
#endif
            b.traveled += len(step);
            b.pos = to;
            if (b.traveled > MAX_RANGE || b.life > 4.0f || b.pos.z < -100.0f) dead = true;
        }
        if (dead) { g_bullets[i] = g_bullets.back(); g_bullets.pop_back(); }
        else ++i;
    }
}

// --------------------------------- HOOKS -----------------------------------
static bool hk_FIH(void* self, void* shooter, V* origin, V* muzzle, void* tgtEnt, V* target, V* drive, bool a6, bool a7) {
    if (g_replay || !g_proj || !self || !origin || !target || !g_FindPlayerPed)
        return orig_FIH(self, shooter, origin, muzzle, tgtEnt, target, drive, a6, a7);
    int type = *(int*)self;
    const WP* w = findWP(type);
    if (!w || shooter != g_FindPlayerPed(0) || g_bullets.size() >= (size_t)MAX_BULLETS)
        return orig_FIH(self, shooter, origin, muzzle, tgtEnt, target, drive, a6, a7);
    V dir = *target - *origin;
    if (len(dir) < 0.5f) return orig_FIH(self, shooter, origin, muzzle, tgtEnt, target, drive, a6, a7);
    dir = norm(dir);

    double now = nowSec();
    double since = now - g_lastShot;
    if (since > 0.03) {  // un nuevo disparo (los perdigones de escopeta no suman calor extra)
        g_heat = fmaxf(0.0f, g_heat - HEAT_DECAY * (float)since) + w->heat;
        if (g_heat > HEAT_MAX) g_heat = HEAT_MAX;
        g_climb = fmaxf(0.0f, g_climb - CLIMB_DECAY * (float)since) + w->shake * 10.0f;
        if (g_climb > CLIMB_MAX) g_climb = CLIMB_MAX;
#if ENABLE_SHAKE
        if (g_CamShake && g_TheCamera) g_CamShake(g_TheCamera, w->shake * (1.0f + g_heat * 0.5f), origin->x, origin->y, origin->z);
#endif
    }
    g_lastShot = now;

    // dispersion: cono que crece con calor (cadencia) y potencia
    float cone = w->spread * (1.0f + g_heat) * (float)(M_PI / 180.0);
    V up = fabsf(dir.z) < 0.95f ? V{0, 0, 1} : V{1, 0, 0};
    V u = norm(cross(dir, up)), v = cross(u, dir);
    float ang = frand() * 6.2831853f, rad = sqrtf(frand()) * tanf(cone);
    V d = dir + u * (cosf(ang) * rad) + v * (sinf(ang) * rad);
    d.z += tanf(g_climb * (float)(M_PI / 180.0));  // ascenso de punteria (retroceso)
    d = norm(d);

    Bullet b;
    b.pos = *origin; b.vel = d * w->speed; b.traveled = 0; b.scale = 1.0f; b.life = 0;
    b.type = type;
#if ENABLE_PIERCE
    b.pierceLeft = w->pierce;
#else
    b.pierceLeft = 0;
#endif
    b.bouncesLeft = 1; b.weapon = self; b.shooter = shooter;
    g_bullets.push_back(b);
    return true;
}

static int g_frames = 0; static bool g_dmgDone = false, g_guardCleared = false;
typedef void (*Frame0_t)();
static Frame0_t orig_Frame0 = nullptr;
static void frameTick();
static void hk_Frame0() { orig_Frame0(); frameTick(); }
static void hk_Idle(void* a, bool b) { orig_Idle(a, b); frameTick(); }
static void frameTick() {
    double now = nowSec();
    float dt = (float)(now - g_lastTick);
    g_lastTick = now;
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;
    g_frames++;
    if (!g_guardCleared && g_frames > 900) { g_guardCleared = true; bootGuardClear(); LOG("arranque correcto, guardia retirada"); }
#if ENABLE_DAMAGE
    if (g_cfgDmg && !g_dmgDone && g_frames > 600) { g_dmgDone = true; applyDamage(); }  // espera a que cargue weapon.dat
#endif
    if (g_proj && !g_bullets.empty()) simulate(dt);
}

static bool hookAt(const Sym* s, void* repl, void** orig, const char* tag) {
    if (!s) { LOG("HOOK %s: simbolo no encontrado", tag); return false; }
    bool ok = aml->Hook((void*)s->addr, repl, orig);
    LOG("HOOK %s: %s (%s)", tag, ok ? "ok" : "FALLO", s->name.c_str());
    return ok;
}

extern "C" void OnModLoad() {
    LOG("RealWeapons 1.1 iniciando");
    readCfg();
    if (bootGuardTripped()) {
        LOG("MODO SEGURO: el arranque anterior no termino. El mod no se activa. Borra RealWeapons.boot para reintentar.");
        return;
    }
    bootGuardArm();
    dl_iterate_phdr(phdrCb, nullptr);
    LOG("simbolos leidos de libGTASA.so: %zu", g_syms.size());
    if (g_syms.empty()) { LOG("No se pudo leer la tabla de simbolos. Mod desactivado."); return; }

    // volcado de diagnostico de simbolos relevantes (para poder corregir firmas)
    for (const Sym& s : g_syms)
        if (s.name.find("FireInstantHit") != std::string::npos || s.name.find("ProcessLineOfSight") != std::string::npos ||
            s.name.find("GetWeaponInfo") != std::string::npos || s.name.find("CamShake") != std::string::npos ||
            s.name.find("AddTrace") != std::string::npos || s.name.find("FindPlayerPed") != std::string::npos ||
            s.name.find("FireSniper") != std::string::npos)
            LOG("SIM %s", s.name.c_str());

    const Sym* sIdle  = findPrefix("_Z4IdlePvb");
    const Sym* sGame  = findPrefix("_ZN5CGame7ProcessEv");
    const Sym* sHud   = findPrefix("_ZN4CHud4DrawEv");
    const Sym* sTimer = findPrefix("_ZN6CTimer6UpdateEv");
    const Sym* sFIH  = findPrefix("_ZN7CWeapon14FireInstantHit");
    const Sym* sLOS  = findPrefix("_ZN6CWorld18ProcessLineOfSight");
    const Sym* sFPP  = findPrefix("_Z13FindPlayerPed");
    const Sym* sGWI  = findPrefix("_ZN11CWeaponInfo13GetWeaponInfo");
    const Sym* sCS   = findPrefix("_ZN7CCamera8CamShake");
    const Sym* sTR   = findPrefix("_ZN13CBulletTraces8AddTrace");
    const Sym* sCam  = nullptr;
    for (const Sym& s : g_syms) if (s.name == "TheCamera") { sCam = &s; break; }
    const Sym* sIgn  = findPrefix("_ZN6CWorld13pIgnoreEntity");

    if (sGWI) g_GetWeaponInfo = (GetWeaponInfo_t)sGWI->addr;
    if (sCS && endsWith(sCS->name, "CamShakeEffff") && sCam) { g_CamShake = (CamShake_t)sCS->addr; g_TheCamera = (void*)sCam->addr; }
    else LOG("CamShake: no disponible (sacudida desactivada)");
    if (sTR && endsWith(sTR->name, "AddTraceEP7CVectorS1_fjh")) g_AddTrace = (AddTrace_t)sTR->addr;
    else LOG("AddTrace: no disponible (sin trazador visible)");
    if (sIgn) g_pIgnore = (void**)sIgn->addr;

#if ENABLE_PROJECTILES
    if (!g_cfgProj) LOG("projectiles=0 en cfg");
    bool okFIH = sFIH && endsWith(sFIH->name, "EP7CEntityP7CVectorS3_S1_S3_S3_bb");
    bool okLOS = sLOS && endsWith(sLOS->name, "R9CColPointRP7CEntitybbbbbbbb");
    bool okFPP = sFPP && endsWith(sFPP->name, "FindPlayerPedi");
    if (!okFIH) LOG("FireInstantHit: firma inesperada o ausente, proyectiles desactivados");
    if (!okLOS) LOG("ProcessLineOfSight: firma inesperada o ausente, proyectiles desactivados");
    if (!okFPP) LOG("FindPlayerPed: firma inesperada o ausente, proyectiles desactivados");
    if (g_cfgProj && okFIH && okLOS && okFPP) {
        g_LOS = (LOS_t)sLOS->addr;
        g_FindPlayerPed = (FindPlayerPed_t)sFPP->addr;
        if (hookAt(sFIH, (void*)hk_FIH, (void**)&orig_FIH, "FireInstantHit")) g_proj = true;
    }
#endif
    bool hooked = false;
    if (g_cfgHook == "idle") hooked = hookAt(sIdle, (void*)hk_Idle, (void**)&orig_Idle, "Idle");
    else if (g_cfgHook == "hud") hooked = hookAt(sHud, (void*)hk_Frame0, (void**)&orig_Frame0, "CHud::Draw");
    else if (g_cfgHook == "timer") hooked = hookAt(sTimer, (void*)hk_Frame0, (void**)&orig_Frame0, "CTimer::Update");
    else hooked = hookAt(sGame, (void*)hk_Frame0, (void**)&orig_Frame0, "CGame::Process");
    if (!hooked) { LOG("sin hook por frame: se desactivan proyectiles y dano"); g_proj = false; }
    LOG("proyectiles=%d dano=%d hook=%s", g_proj ? 1 : 0, g_cfgDmg, g_cfgHook.c_str());
}
