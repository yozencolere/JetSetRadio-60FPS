// Jet Set Radio (PC 2012) - d3d9.dll intermediaire : images intermediaires (30 -> 60 FPS).
//
// Methode « Ninja » : le jeu (portage Dreamcast, bibliotheque Ninja de Sega) dessine chaque modele
// avec la matrice courante (vue x modele) ; le processeur transforme les sommets et les range dans
// des listes, ecrites directement dans un grand tampon de sommets, dessinees en fin d'image.
//   1. Les 5 fonctions de dessin de modele sont interceptees (par leurs relais d'appel). Chaque modele
//      est dessine normalement, puis une seconde fois avec la matrice a mi-chemin entre l'image
//      precedente et celle-ci (rotation en quaternions, echelle et translation lineaires).
//      Le jeu calcule lui-meme les sommets intermediaires : faces cachees, decoupe, rotations correctes.
//   2. Les sommets de ces secondes copies sont notes par plages ; chaque dessin du tampon est classe
//      reel, intermediaire ou commun (2D, effets, tout ce qui n'est pas un modele).
//   3. Pendant l'image, les dessins intermediaires ne sont pas executes. Au Present, l'image est
//      rejouee deux fois : intermediaire + commun (presentee), puis reel + commun (presentee).
// Autres fonctions (v1.0) :
//   - flou de mouvement limite a la camera (copies supplementaires a t = 0.5 - s et 1 - s, moyennees ;
//     les objets qui bougent dans le monde restent nets) : F1 coupe, F2 leger, F3 intermediaire, F4 fort ;
//   - filtrage anisotrope x16 (le jeu ne regle jamais MAXANISOTROPY) ;
//   - corrections du jeu d'origine : couches des tags dessinees de la plus lointaine a la plus proche (voile
//     blanc), tags rapproches de la camera, transparence triee par le sommet le plus lointain, tri stable,
//     angle de vue 4:3 des objets elargi, blocs de ville manquants dessines (batiments qui surgissent).
// F8 : tout le mod coupe / remis (jeu d'origine). F10 : captures de diagnostic. Journal : jsr60-log.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <set>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <chrono>

namespace {
HMODULE real_d3d9 = nullptr;
using CreateFn = IDirect3D9* (WINAPI*)(UINT);
CreateFn real_create = nullptr;
FILE* logf = nullptr;
void log(const char* fmt, ...) {
    if (!logf) { logf = fopen("jsr60-log.txt", "w"); if (!logf) return; }
    va_list a; va_start(a, fmt); vfprintf(logf, fmt, a); va_end(a); fflush(logf);
}
void* patch(void* obj, int index, void* hook) {
    void** vt = *reinterpret_cast<void***>(obj);
    if (vt[index] == hook) return nullptr;
    DWORD old; VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    void* orig = vt[index]; vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    return orig;
}

// ================================================================ cote jeu (adresses de jetsetradio.exe)
// L'exe utilise l'ASLR : toutes les adresses (prises a la base 0x400000) sont recalees sur sa base reelle.
uintptr_t exe_delta = 0;
template <class T> T at(uint32_t va) { return reinterpret_cast<T>(uintptr_t(va) + exe_delta); }
float** kCurMatrix = nullptr;      // pointeur vers la matrice courante (4x4), 0x00DD5C40
uint32_t* kVertCount = nullptr;    // sommets ecrits dans le tampon de l'image, 0x00DD5FF0
void** kVertPtr = nullptr;         // tampon verrouille (nul hors image), 0x00DD5FEC
using EndStripFn = void (__cdecl*)();
EndStripFn end_strip = nullptr;    // ferme la bande en cours, 0x005B5C30
using ModelFn = int (__cdecl*)(void*);
struct ModelHook { uint32_t relay, target; };
constexpr ModelHook kModelHooks[5] = {
    {0x004013A7, 0x0059C280}, {0x00403792, 0x0059C6C0}, {0x004060AA, 0x0059C9C0}, {0x004024A5, 0x005A40C0}, {0x004036FC, 0x005A7610}};
ModelFn model_orig[5] = {};

bool enabled = true;
// Plages de sommets des copies intermediaires et reelles de l'image en cours.
struct Range { uint32_t a, b; };
std::vector<Range> interp_ranges, real_ranges;
// Flou de mouvement : copies supplementaires a t = 0.5 - s (classe 3, moyennee avec l'image intermediaire) et
// t = 1 - s (classe 4, moyennee avec l'image reelle). blur_level 2 : s = 0.25 (obturateur 180 degres), 1 : 0.125, 0 : coupe.
std::vector<Range> blur3_ranges, blur4_ranges;
// Chaque plage de sommets dit a quelles sous-images elle appartient (masque) : 1 = image reelle, 2 = intermediaire,
// 4 = flou de l'intermediaire (t = 0.5 - s), 8 = flou de la reelle (t = 1 - s). 0 = commun a toutes.
// Si une copie de flou ne produit rien (le jeu ne redessine pas : logo du titre...), la copie la plus proche la remplace.
struct MRange { uint32_t a, b, mask; };
std::vector<MRange> mranges;
enum : uint32_t { SUB_R = 1, SUB_I = 2, SUB_B3 = 4, SUB_B4 = 8, SUB_NONE = 0x100 };
std::vector<Range> mranges_dead;   // copies de flou rejetees (jamais dessinees)
// Niveaux (touches F1 a F4) : 0 coupe, 1 leger, 2 intermediaire, 3 fort (obturateur 180 degres, par defaut).
int blur_level = 3;
float blur_s() { static const float k[4] = {0.f, 0.0833f, 0.1667f, 0.25f}; return k[blur_level & 3]; }
// Matrices par (modele, rang d'apparition) : image precedente et image en cours.
struct KeyHash { size_t operator()(uint64_t k) const { return std::hash<uint64_t>()(k); } };
std::unordered_map<uint64_t, std::array<float, 16>, KeyHash>* mats_cur = nullptr;
std::unordered_map<uint64_t, std::array<float, 16>, KeyHash>* mats_prev = nullptr;
// Instances par modele : un meme modele (rambarde, fleche...) est dessine plusieurs fois par image et son rang
// change des qu'une instance sort de l'ecran. On apparie donc chaque instance a celle de l'image precedente
// la plus proche en position MONDE (matrice courante ramenee par la matrice de vue = bas de la pile Ninja).
struct Inst { std::array<float, 16> m; float w[3]; bool used; };
using InstMap = std::unordered_map<void*, std::vector<Inst>>;
InstMap* inst_cur = nullptr; InstMap* inst_prev = nullptr;
float** kViewMatrix = nullptr;     // bas de la pile de matrices (vue), 0x00DD5C48
// Flou de mouvement limite a la camera : vue de l'image en cours (derniere vue vue au dessin d'un modele) et de
// l'image precedente. Copie de flou = W * Vprec = M * V^-1 * Vprec (l'objet reste a sa place, seule la camera recule).
constexpr float kStaticEps = 0.02f;   // deplacement dans le monde au-dela duquel un objet est "mobile"
float view_cur[16], view_prev[16]; bool have_view_cur = false, have_view_prev = false;
// inverse d'une matrice affine 4x4 (vecteur ligne : lignes 0..2 = base, ligne 3 = translation)
bool affine_inverse(const float* m, float* o) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e*i - f*h, B = -(d*i - f*g), C = d*h - e*g, det = a*A + b*B + c*C;
    if (std::fabs(det) < 1e-12f) return false;
    const float r[9] = {A/det, -(b*i - c*h)/det, (b*f - c*e)/det, B/det, (a*i - c*g)/det, -(a*f - c*d)/det, C/det, -(a*h - b*g)/det, (a*e - b*d)/det};
    for (int k = 0; k < 3; k++) { o[k*4] = r[k*3]; o[k*4+1] = r[k*3+1]; o[k*4+2] = r[k*3+2]; o[k*4+3] = 0; }
    for (int k = 0; k < 3; k++) o[12+k] = -(m[12]*r[k] + m[13]*r[3+k] + m[14]*r[6+k]);
    o[15] = 1; return true;
}
void mat_mul(const float* a, const float* b, float* o) {   // o = a * b (4x4, vecteur ligne)
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        o[r*4+c] = a[r*4]*b[c] + a[r*4+1]*b[4+c] + a[r*4+2]*b[8+c] + a[r*4+3]*b[12+c];
}
void world_pos(const float* m, float w[3]) {
    const float* v = kViewMatrix ? *kViewMatrix : nullptr;
    if (!v || v == m) { w[0] = m[12]; w[1] = m[13]; w[2] = m[14]; return; }
    // camera = monde * R + t (vecteur ligne) -> monde = (camera - t) * R^-1
    const float a = v[0], b = v[1], c = v[2], d = v[4], e = v[5], f = v[6], g = v[8], h = v[9], i = v[10];
    const float A = e*i - f*h, B = -(d*i - f*g), C = d*h - e*g, det = a*A + b*B + c*C;
    if (std::fabs(det) < 1e-12f) { w[0] = m[12]; w[1] = m[13]; w[2] = m[14]; return; }
    const float inv[9] = {A/det, -(b*i - c*h)/det, (b*f - c*e)/det, B/det, (a*i - c*g)/det, -(a*f - c*d)/det, C/det, -(a*h - b*g)/det, (a*e - b*d)/det};
    const float x = m[12] - v[12], y = m[13] - v[13], z = m[14] - v[14];
    w[0] = x*inv[0] + y*inv[3] + z*inv[6]; w[1] = x*inv[1] + y*inv[4] + z*inv[7]; w[2] = x*inv[2] + y*inv[5] + z*inv[8];
}
struct Stats { unsigned models = 0, interp = 0, same = 0, jump = 0, empty = 0, hud = 0, blur_rejected = 0; } st;
bool in_model = false;
FILE* model_log = nullptr;          // F10 : detail des modeles des images capturees
// Tampon de travail des sommets du moteur ([0xDD5CE8], 16 octets par sommet, indice 16 bits) : les morceaux
// d'un personnage y lisent les sommets ecrits par les morceaux precedents (articulations ponderees).
// La passe intermediaire a donc son propre tampon, sinon reel et intermediaire se melangent aux genoux.
void** kWorkBuf = nullptr;
alignas(16) static uint8_t g_work_interp[0x10000 * 16];
alignas(16) static uint8_t g_work_b3[0x10000 * 16];
alignas(16) static uint8_t g_work_b4[0x10000 * 16];
bool logged_work = false;

// --- interpolation de matrice (lignes 0..2 = base mise a l'echelle, ligne 3 = translation)
struct Quat { float w, x, y, z; };
Quat to_quat(const float r[3][3]) {
    Quat q; const float t = r[0][0] + r[1][1] + r[2][2];
    if (t > 0) { const float s = std::sqrt(t + 1.f) * 2; q.w = .25f * s; q.x = (r[1][2] - r[2][1]) / s; q.y = (r[2][0] - r[0][2]) / s; q.z = (r[0][1] - r[1][0]) / s; }
    else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) { const float s = std::sqrt(1.f + r[0][0] - r[1][1] - r[2][2]) * 2; q.w = (r[1][2] - r[2][1]) / s; q.x = .25f * s; q.y = (r[1][0] + r[0][1]) / s; q.z = (r[2][0] + r[0][2]) / s; }
    else if (r[1][1] > r[2][2]) { const float s = std::sqrt(1.f + r[1][1] - r[0][0] - r[2][2]) * 2; q.w = (r[2][0] - r[0][2]) / s; q.x = (r[1][0] + r[0][1]) / s; q.y = .25f * s; q.z = (r[2][1] + r[1][2]) / s; }
    else { const float s = std::sqrt(1.f + r[2][2] - r[0][0] - r[1][1]) * 2; q.w = (r[0][1] - r[1][0]) / s; q.x = (r[2][0] + r[0][2]) / s; q.y = (r[2][1] + r[1][2]) / s; q.z = .25f * s; }
    return q;
}
void from_quat(Quat q, float r[3][3]) {
    const float n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z); q.w /= n; q.x /= n; q.y /= n; q.z /= n;
    r[0][0] = 1 - 2*(q.y*q.y + q.z*q.z); r[0][1] = 2*(q.x*q.y + q.z*q.w); r[0][2] = 2*(q.x*q.z - q.y*q.w);
    r[1][0] = 2*(q.x*q.y - q.z*q.w); r[1][1] = 1 - 2*(q.x*q.x + q.z*q.z); r[1][2] = 2*(q.y*q.z + q.x*q.w);
    r[2][0] = 2*(q.x*q.z + q.y*q.w); r[2][1] = 2*(q.y*q.z - q.x*q.w); r[2][2] = 1 - 2*(q.x*q.x + q.y*q.y);
}
float det3(const float* m) {
    return m[0]*(m[5]*m[10] - m[6]*m[9]) - m[1]*(m[4]*m[10] - m[6]*m[8]) + m[2]*(m[4]*m[9] - m[5]*m[8]);
}
// Renvoie false si l'ecart est trop grand (coupure de plan, objet teleporte) : on garde alors b.
bool interp_matrix(const float* a, const float* b, float t, float* out) {
    const float ta = std::sqrt(a[12]*a[12] + a[13]*a[13] + a[14]*a[14]), tb = std::sqrt(b[12]*b[12] + b[13]*b[13] + b[14]*b[14]);
    const float d = std::sqrt((a[12]-b[12])*(a[12]-b[12]) + (a[13]-b[13])*(a[13]-b[13]) + (a[14]-b[14])*(a[14]-b[14]));
    if (d > 0.35f * std::max(ta, tb) + 1e-3f) return false;
    float sa[3], sb[3], ra[3][3], rb[3][3];
    for (int i = 0; i < 3; i++) {
        sa[i] = std::sqrt(a[i*4]*a[i*4] + a[i*4+1]*a[i*4+1] + a[i*4+2]*a[i*4+2]);
        sb[i] = std::sqrt(b[i*4]*b[i*4] + b[i*4+1]*b[i*4+1] + b[i*4+2]*b[i*4+2]);
        if (sa[i] < 1e-12f || sb[i] < 1e-12f) return false;
        for (int j = 0; j < 3; j++) { ra[i][j] = a[i*4+j] / sa[i]; rb[i][j] = b[i*4+j] / sb[i]; }
    }
    memcpy(out, b, 64);
    if (det3(a) * det3(b) <= 0 || det3(b) < 0) {               // reflexion : interpolation lineaire simple
        for (int i = 0; i < 16; i++) out[i] = a[i] + (b[i] - a[i]) * t;
        return true;
    }
    Quat qa = to_quat(ra), qb = to_quat(rb);
    float dot = qa.w*qb.w + qa.x*qb.x + qa.y*qb.y + qa.z*qb.z;
    if (dot < 0) { qb.w = -qb.w; qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; dot = -dot; }
    Quat q;
    if (dot > 0.9995f) { q = {qa.w + (qb.w-qa.w)*t, qa.x + (qb.x-qa.x)*t, qa.y + (qb.y-qa.y)*t, qa.z + (qb.z-qa.z)*t}; }
    else {
        const float th = std::acos(std::min(1.f, dot)), s = std::sin(th), wa = std::sin((1-t)*th) / s, wb = std::sin(t*th) / s;
        q = {qa.w*wa + qb.w*wb, qa.x*wa + qb.x*wb, qa.y*wa + qb.y*wb, qa.z*wa + qb.z*wb};
    }
    float r[3][3]; from_quat(q, r);
    for (int i = 0; i < 3; i++) { const float s = sa[i] + (sb[i] - sa[i]) * t; for (int j = 0; j < 3; j++) out[i*4+j] = r[i][j] * s; }
    for (int i = 12; i < 15; i++) out[i] = a[i] + (b[i] - a[i]) * t;
    return true;
}

// Dessin d'un modele : copie reelle puis copie a mi-chemin.
// Vue "sans camera" (interface : fleches de tag, icones 3D) : la vue en bas de la pile n'a aucune translation
// (camera a l'origine exacte, jamais le cas en jeu ou la ville est a des centaines d'unites). Pas d'interpolation.
bool hud_view() {
    const float* v = kViewMatrix ? *kViewMatrix : nullptr;
    return v && std::fabs(v[12]) < 1e-3f && std::fabs(v[13]) < 1e-3f && std::fabs(v[14]) < 1e-3f;
}
// Journal F10 : rectangle ecran couvert par les sommets [a, b) du tampon verrouille.
void log_bbox(const char* what, uint32_t a, uint32_t b) {
    if (!model_log || !*kVertPtr || b <= a) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = a; i < b; i++) {
        const float* v = reinterpret_cast<const float*>(static_cast<uint8_t*>(*kVertPtr) + size_t(i) * 32);
        x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]); y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]);
    }
    fprintf(model_log, "   %s sommets %u-%u ecran x %.0f..%.0f y %.0f..%.0f\n", what, a, b, x0, x1, y0, y1);
}
// Tags peints (objet dessine par 0x0042BA20, appel 0x0042BB16 -> 0x00404BD3) : image plate posee sur un mur
// ondule dont les cretes passent devant elle (bandes de voile blanc selon l'angle). Pendant cet appel, la matrice
// est rapprochee de l'oeil d'une distance fixe : position a l'ecran inchangee (x/z, y/z), seule la profondeur baisse.
bool drawing_tag = false; unsigned tags_pulled = 0;
std::vector<Range> tag_ranges;   // sommets des tags de l'image (reels et intermediaires)
constexpr float kTagPull = 0.3f;
using DrawObjFn = int (__cdecl*)(void*);
DrawObjFn tag_draw_orig = nullptr;
int __cdecl tag_draw_hook(void* obj) { drawing_tag = true; const int r = tag_draw_orig(obj); drawing_tag = false; return r; }
void pull_matrix(float* m) {
    const float d = std::sqrt(m[12]*m[12] + m[13]*m[13] + m[14]*m[14]);
    if (d <= kTagPull * 4) return;
    const float k = (d - kTagPull) / d;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) m[r*4 + c] *= k;
    tags_pulled++;
}
// Rectangle ecran des sommets [a, b) ; false si vide ou si des sommets sont tres hors ecran (decoupe : pas de controle).
bool screen_box(uint32_t a, uint32_t b, float* o) {
    if (b <= a || !*kVertPtr) return false;
    o[0] = o[2] = 1e9f; o[1] = o[3] = -1e9f;
    for (uint32_t i = a; i < b; i++) {
        const float* v = reinterpret_cast<const float*>(static_cast<uint8_t*>(*kVertPtr) + size_t(i) * 32);
        o[0] = std::min(o[0], v[0]); o[1] = std::max(o[1], v[0]); o[2] = std::min(o[2], v[1]); o[3] = std::max(o[3], v[1]);
    }
    return o[0] > -8000.f && o[1] < 10000.f && o[2] > -8000.f && o[3] < 9000.f;
}
int model_common(int which, void* model) {
    const ModelFn orig = model_orig[which];
    if (enabled && drawing_tag && !in_model && *kCurMatrix) {   // tag : rapproche (les deux copies partent de la)
        float* tm = *kCurMatrix; float save[16]; memcpy(save, tm, 64); pull_matrix(tm);
        const bool was = drawing_tag; drawing_tag = false;
        end_strip(); const uint32_t t0 = *kVertCount;
        const int r = model_common(which, model);
        end_strip(); if (*kVertCount > t0) tag_ranges.push_back({t0, *kVertCount});
        drawing_tag = was; memcpy(tm, save, 64);
        return r;
    }
    {   // diagnostic : limites proche/lointaine du test de visibilite des modeles (0xDD5C74 / 0xDD5D3C)
        static std::set<int> seen_far; const float fz = *at<float*>(0x00DD5D3C), nz = *at<float*>(0x00DD5C74);
        if (false && seen_far.insert(int(fz)).second) log("limite de visibilite : proche %.2f lointaine %.1f\n", nz, fz);
        static std::set<int> seen_rect; const float* R = at<float*>(0x00DD5C84);
        const int key = int(R[2]) * 10000 + int(R[3]);
        if (false && seen_rect.insert(key).second)
            log("rectangle de visibilite : x %.1f..%.1f y %.1f..%.1f centre %.1f %.1f echelles %.3f %.3f %.3f\n", R[0], R[2], R[1], R[3],
                *at<float*>(0x00DD5DEC), *at<float*>(0x00DD5DF0), *at<float*>(0x00DD5C68), *at<float*>(0x00DD5D78), *at<float*>(0x00DD5DE0));
    }
    if (!enabled || in_model || !*kVertPtr || !*kCurMatrix || !inst_cur) return orig(model);
    if (hud_view()) {                       // interface : dessinee une fois, commune aux deux images
        st.hud++; if (model_log) fprintf(model_log, "%p f%d interface (pas d'interpolation)\n", model, which);
        return orig(model);
    }
    in_model = true;
    if (kViewMatrix && *kViewMatrix) { memcpy(view_cur, *kViewMatrix, 64); have_view_cur = true; }
    float* m = *kCurMatrix; std::array<float, 16> cur; memcpy(cur.data(), m, 64);
    Inst inst; inst.m = cur; inst.used = false; world_pos(m, inst.w);
    (*inst_cur)[model].push_back(inst);
    end_strip(); const uint32_t r0 = *kVertCount;
    const int ret = orig(model);
    end_strip(); const uint32_t r1 = *kVertCount;
    float mid[16]; bool moved = false; const float* prevm = nullptr; bool obj_moving = false;
    auto it = inst_prev->find(model);
    if (it != inst_prev->end()) {
        Inst* best = nullptr; float bd = 1e30f;
        for (Inst& p : it->second) {
            if (p.used) continue;
            const float dx = p.w[0] - inst.w[0], dy = p.w[1] - inst.w[1], dz = p.w[2] - inst.w[2], dd = dx*dx + dy*dy + dz*dz;
            if (dd < bd) { bd = dd; best = &p; }
        }
        if (best) {
            best->used = true;
            const float dx = best->w[0] - inst.w[0], dy = best->w[1] - inst.w[1], dz = best->w[2] - inst.w[2];
            obj_moving = dx*dx + dy*dy + dz*dz > kStaticEps * kStaticEps;
            if (interp_matrix(best->m.data(), cur.data(), 0.5f, mid)) { moved = true; prevm = best->m.data(); } else st.jump++;
        }
    }
    if (!moved) { memcpy(mid, cur.data(), 64); st.same++; } else st.interp++;
    if (model_log) fprintf(model_log, "%p f%d monde(%.2f %.2f %.2f) cam(%.2f %.2f %.2f) %s\n", model, which, inst.w[0], inst.w[1], inst.w[2],
        cur[12], cur[13], cur[14], moved ? "mi-chemin" : (it == inst_prev->end() ? "nouveau" : "saut/sans"));
    memcpy(m, mid, 64);
    const uint32_t i0 = *kVertCount;
    if (!logged_work) { logged_work = true; log("tampon de travail des sommets : %p\n", *kWorkBuf); }
    void* const work_real = *kWorkBuf; *kWorkBuf = g_work_interp;
    orig(model);
    *kWorkBuf = work_real;
    end_strip(); const uint32_t i1 = *kVertCount;
    // copies du flou de mouvement (chacune avec son tampon de travail, comme la copie intermediaire)
    uint32_t b30 = 0, b31 = 0, b40 = 0, b41 = 0;
    if (blur_level > 0 && i1 > i0) {
        const float sh = blur_s();
        float bm[16];
        auto copy_at = [&](float t, uint8_t* work, uint32_t& c0, uint32_t& c1) {
            // Flou limite a la camera : un objet qui bouge dans le monde (perso, passants, voitures, membres) garde
            // dans la copie de flou la meme position que dans son image (intermediaire ou reelle) -> reste net.
            // Un objet fixe suit l'interpolation complete, qui ne contient alors que le mouvement de la camera.
            if (obj_moving) memcpy(bm, t < 0.5f ? mid : cur.data(), 64);
            else if (!(prevm && interp_matrix(prevm, cur.data(), t, bm))) memcpy(bm, cur.data(), 64);
            memcpy(m, bm, 64);
            c0 = *kVertCount;
            void* const wr = *kWorkBuf; *kWorkBuf = work; orig(model); *kWorkBuf = wr;
            end_strip(); c1 = *kVertCount;
        };
        copy_at(0.5f - sh, g_work_b3, b30, b31);
        copy_at(1.0f - sh, g_work_b4, b40, b41);
        // Controle : une copie de flou doit rester dans la zone balayee par le mouvement (reel <-> intermediaire,
        // prolonge d'autant). Certains modeles changent d'etat a chaque appel (logo du titre) : copie rejetee.
        float R[4], I[4], B[4];
        if (screen_box(r0, r1, R) && screen_box(i0, i1, I)) {
            const float dx = std::max(std::fabs(R[0] - I[0]), std::fabs(R[1] - I[1])) + 48.f;
            const float dy = std::max(std::fabs(R[2] - I[2]), std::fabs(R[3] - I[3])) + 48.f;
            auto ok = [&](uint32_t c0, uint32_t c1) {
                if (c1 <= c0) return true;
                if (!screen_box(c0, c1, B)) return false;
                return B[0] >= std::min(R[0], I[0]) - dx && B[1] <= std::max(R[1], I[1]) + dx &&
                       B[2] >= std::min(R[2], I[2]) - dy && B[3] <= std::max(R[3], I[3]) + dy;
            };
            if (!ok(b30, b31)) { st.blur_rejected++; mranges_dead.push_back({b30, b31}); b31 = b30; }
            if (!ok(b40, b41)) { st.blur_rejected++; mranges_dead.push_back({b40, b41}); b41 = b40; }
        }
    }
    memcpy(m, cur.data(), 64);
    log_bbox("reel", r0, r1); log_bbox("interm", i0, i1); log_bbox("flou3", b30, b31); log_bbox("flou4", b40, b41);
    // Recherche du dessin des tags : chaque modele f3 (0x5A40C0) est note une fois avec sa chaine d'appel et sa zone ecran.
    if (which == 3 && r1 > r0 && *kVertPtr) {
        static std::unordered_map<void*, int> seen_f3; static int noted = 0;
        if (noted < 0 && !seen_f3.count(model)) {   // diagnostic coupe
            seen_f3[model] = 1; noted++;
            void* fr[10] = {}; const USHORT nfr = CaptureStackBackTrace(0, 10, fr, nullptr);
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (uint32_t i = r0; i < r1; i++) {
                const float* v = reinterpret_cast<const float*>(static_cast<uint8_t*>(*kVertPtr) + size_t(i) * 32);
                x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]); y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]);
            }
            char buf[400]; int k = sprintf(buf, "modele f3 %p cam(%.1f %.1f %.1f) ecran x %.0f..%.0f y %.0f..%.0f pile", model, cur[12], cur[13], cur[14], x0, x1, y0, y1);
            for (USHORT i = 0; i < nfr && k < 380; i++) {
                const uintptr_t a = reinterpret_cast<uintptr_t>(fr[i]);
                if (a >= 0x400000 + exe_delta && a < 0x1000000 + exe_delta) k += sprintf(buf + k, " %06X", unsigned(a - exe_delta));
            }
            log("%s\n", buf);
        }
    }
    // Copie intermediaire vide (le jeu n'a rien redessine : fleche de tag...) : l'original sert aux deux images.
    if (i1 > i0) {
        interp_ranges.push_back({i0, i1}); if (r1 > r0) real_ranges.push_back({r0, r1});
        if (b31 > b30) blur3_ranges.push_back({b30, b31});
        if (b41 > b40) blur4_ranges.push_back({b40, b41});
        const bool blur_on = blur_level > 0;
        if (r1 > r0) mranges.push_back({r0, r1, SUB_R | ((blur_on && b41 <= b40) ? SUB_B4 : 0u)});
        mranges.push_back({i0, i1, SUB_I | ((blur_on && b31 <= b30) ? SUB_B3 : 0u)});
        if (b31 > b30) mranges.push_back({b30, b31, SUB_B3});
        if (b41 > b40) mranges.push_back({b40, b41, SUB_B4});
        for (const Range& dr : mranges_dead) mranges.push_back({dr.a, dr.b, SUB_NONE});
        std::sort(mranges.end() - std::min<size_t>(mranges.size(), 4 + mranges_dead.size()), mranges.end(),
                  [](const MRange& x, const MRange& y) { return x.a < y.a; });
        mranges_dead.clear();
    }
    else if (r1 > r0) { st.empty++; if (model_log) fprintf(model_log, "%p f%d : copie intermediaire vide (%u sommets reels)\n", model, which, r1 - r0); }
    st.models++; in_model = false;
    return ret;
}
int __cdecl hook0(void* m) { return model_common(0, m); }
int __cdecl hook1(void* m) { return model_common(1, m); }
int __cdecl hook2(void* m) { return model_common(2, m); }
int __cdecl hook3(void* m) { return model_common(3, m); }
int __cdecl hook4(void* m) { return model_common(4, m); }
// Comparaison du tri des listes de dessin (qsort, entrees de 0x24 octets), reprise de 0x005B6060 :
// priorite (octet +0x20), puis profondeur (+0x1C, ramenee a 0.01 si < 0.01). En cas d'egalite l'original rend 0
// et qsort (instable) melange l'ordre ; avec nos copies intermediaires dans la liste, cet ordre changeait d'une
// image a l'autre (grillage qui laisse voir tantot le ciel, tantot le mur). On departage par l'ordre de soumission
// (premier sommet, +0x14), stable d'une image a l'autre.
int __cdecl sort_compare(const void* pa, const void* pb) {
    const uint8_t* a = static_cast<const uint8_t*>(pa); const uint8_t* b = static_cast<const uint8_t*>(pb);
    const int8_t ca = int8_t(a[0x20]), cb = int8_t(b[0x20]);
    if (ca != cb) return int(cb) - int(ca);
    float da = *reinterpret_cast<const float*>(a + 0x1C), db = *reinterpret_cast<const float*>(b + 0x1C);
    if (double(da) < 0.01) da = 0.0099999998f;
    if (double(db) < 0.01) db = 0.0099999998f;
    const float d = db - da;
    if (d > 0) return 1;
    if (d < 0) return -1;
    if (!enabled) return 0;                 // F8 : comportement d'origine exact
    const uint32_t sa = *reinterpret_cast<const uint32_t*>(a + 0x14), sb = *reinterpret_cast<const uint32_t*>(b + 0x14);
    return sa < sb ? -1 : (sa > sb ? 1 : 0);
}
// Tri de la liste transparente (appel 0x005B6331 -> qsort 0x005D52C0). Le jeu classe chaque morceau par la
// profondeur de son sommet le PLUS PROCHE (+0x1C, cf. 0x005B5DE0) : un grand mur qui avance vers la camera passe
// alors apres le tag peint dessus et le recouvre (voile blanc, decor qui disparait selon l'angle). La Dreamcast
// triait la transparence au pixel. On classe par le sommet le PLUS LOINTAIN : un tag contenu dans son mur passe
// toujours apres lui. Sommets lus dans la memoire du tampon de l'image (pool managed, encore valide apres Unlock).
extern const uint8_t* last_vb_data; extern uint32_t last_vb_bytes; extern FILE* model_log;
using QsortFn = void (__cdecl*)(void*, size_t, size_t, int (__cdecl*)(const void*, const void*));
QsortFn game_qsort = nullptr; unsigned far_keys = 0;
// Releve automatique (sans touche) quand un grand tag est a l'ecran : sommets du tag et des morceaux transparents
// qui le recouvrent, pour comprendre le voile blanc. 5 releves au plus, espaces de 3 s ; image sauvee au Present.
extern std::vector<Range> tag_ranges; int voile_dumps = 0; bool voile_shot = false; DWORD voile_next = 0;
void dump_verts(FILE* f, uint32_t a, uint32_t b) {
    for (uint32_t v = a; v < b; v++) {
        const float* q = reinterpret_cast<const float*>(last_vb_data + size_t(v) * 32);
        fprintf(f, "   %u %.2f %.2f %.4f %.6f\n", v, q[0], q[1], q[2], q[3]);
    }
}
constexpr bool kAutoDumpVoile = false;
constexpr bool kStatsLog = false;
void auto_dump_voile(void* base, size_t n, size_t sz) {
    if (!last_vb_data || sz != 0x24 || tag_ranges.empty() || GetTickCount() < voile_next) return;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const Range& r : tag_ranges) for (uint32_t v = r.a; v < r.b; v++) {
        if (uint64_t(v + 1) * 32 > last_vb_bytes) break;
        const float* q = reinterpret_cast<const float*>(last_vb_data + size_t(v) * 32);
        if (q[0] < -4000 || q[0] > 6000 || q[1] < -4000 || q[1] > 5000) continue;
        if (q[3] <= 0 || 1.f / q[3] > 80.f) continue;               // tags proches seulement
        x0 = std::min(x0, q[0]); x1 = std::max(x1, q[0]); y0 = std::min(y0, q[1]); y1 = std::max(y1, q[1]);
    }
    if (x1 - x0 < 600 || x1 < 0 || x0 > 1920) return;
    voile_next = GetTickCount() + 4000;
    char name[64]; sprintf(name, "jsr60-voile-%d.txt", voile_dumps % 10); FILE* f = fopen(name, "w"); if (!f) return;
    fprintf(f, "TAG ecran x %.0f..%.0f y %.0f..%.0f\n", x0, x1, y0, y1);
    for (const Range& r : tag_ranges) { fprintf(f, "TAG sommets %u-%u\n", r.a, r.b); if (uint64_t(r.b) * 32 <= last_vb_bytes) dump_verts(f, r.a, r.b); }
    const uint8_t* e = static_cast<const uint8_t*>(base);
    for (size_t i = 0; i < n; i++, e += 0x24) {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(e); const uint32_t a = w[5], c = w[6];
        if (!c || (uint64_t(a) + c) * 32 > last_vb_bytes) continue;
        float ex0 = 1e9f, ex1 = -1e9f, ey0 = 1e9f, ey1 = -1e9f;
        for (uint32_t v = a; v < a + c; v++) { const float* q = reinterpret_cast<const float*>(last_vb_data + size_t(v) * 32);
            ex0 = std::min(ex0, q[0]); ex1 = std::max(ex1, q[0]); ey0 = std::min(ey0, q[1]); ey1 = std::max(ey1, q[1]); }
        if (ex1 < x0 || ex0 > x1 || ey1 < y0 || ey0 > y1) continue;
        fprintf(f, "T%u %08X %08X deb %u nb %u cle %.3f\n", unsigned(i), w[0], w[1], a, c, *reinterpret_cast<const float*>(e + 0x1C));
        dump_verts(f, a, a + c);
    }
    fclose(f); voile_shot = true;
}
// Couches d'un meme tag (detail + grand fond juste derriere) : la cle de tri ne les departage pas (96.1 contre 96.0)
// et la couche avant, dessinee la premiere, ecrit sa profondeur meme la ou elle est transparente -> la couche
// arriere est rejetee partout (voile blanc). On compare leurs profondeurs reelles aux memes points de l'ecran
// (grille 48x27) et on dessine la plus lointaine d'abord.
constexpr int kGW = 48, kGH = 27;
void sample_rhw(uint32_t start, uint32_t count, std::vector<float>& out) {
    out.assign(kGW * kGH, -1.f);
    auto V = [&](uint32_t i) { return reinterpret_cast<const float*>(last_vb_data + size_t(start + i) * 32); };
    for (uint32_t i = 0; i + 2 < count; i++) {
        const float *a = V(i), *b = V(i + 1), *c = V(i + 2);
        const float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::fabs(d) < 1e-6f) continue;
        const float minx = std::min({a[0], b[0], c[0]}), maxx = std::max({a[0], b[0], c[0]});
        const float miny = std::min({a[1], b[1], c[1]}), maxy = std::max({a[1], b[1], c[1]});
        const int gx0 = std::max(0, int(minx / 40.f)), gx1 = std::min(kGW - 1, int(maxx / 40.f));
        const int gy0 = std::max(0, int(miny / 40.f)), gy1 = std::min(kGH - 1, int(maxy / 40.f));
        for (int gy = gy0; gy <= gy1; gy++) for (int gx = gx0; gx <= gx1; gx++) {
            const float X = gx * 40.f + 20.f, Y = gy * 40.f + 20.f;
            const float l0 = ((b[1] - c[1]) * (X - c[0]) + (c[0] - b[0]) * (Y - c[1])) / d;
            const float l1 = ((c[1] - a[1]) * (X - c[0]) + (a[0] - c[0]) * (Y - c[1])) / d;
            const float l2 = 1.f - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const float w = l0 * a[3] + l1 * b[3] + l2 * c[3];
            float& o = out[gy * kGW + gx]; if (w > o) o = w;
        }
    }
}
unsigned tag_layers_fixed = 0;
void order_tag_layers(uint8_t* base, size_t n) {
    std::vector<size_t> pos;
    for (size_t i = 0; i < n; i++) {
        const uint32_t start = *reinterpret_cast<uint32_t*>(base + i * 0x24 + 0x14), count = *reinterpret_cast<uint32_t*>(base + i * 0x24 + 0x18);
        if (!count || (uint64_t(start) + count) * 32 > last_vb_bytes) continue;
        for (const Range& r : tag_ranges) if (start >= r.a && start < r.b) { pos.push_back(i); break; }
    }
    if (pos.size() < 2 || pos.size() > 64) return;
    std::vector<std::vector<float>> smp(pos.size());
    for (size_t k = 0; k < pos.size(); k++) {
        const uint8_t* e = base + pos[k] * 0x24;
        sample_rhw(*reinterpret_cast<const uint32_t*>(e + 0x14), *reinterpret_cast<const uint32_t*>(e + 0x18), smp[k]);
    }
    // ordre : tri par insertion, "a avant b" si a est plus loin que b sur la majorite des points communs
    std::vector<size_t> ord(pos.size()); for (size_t k = 0; k < ord.size(); k++) ord[k] = k;
    auto behind = [&](size_t a, size_t b) {
        int fa = 0, fb = 0;
        for (int i = 0; i < kGW * kGH; i++) if (smp[a][i] > 0 && smp[b][i] > 0) { if (smp[a][i] < smp[b][i]) fa++; else if (smp[b][i] < smp[a][i]) fb++; }
        return fa > fb;
    };
    for (size_t i = 1; i < ord.size(); i++) {
        size_t j = i;
        while (j > 0 && behind(ord[j], ord[j - 1])) { std::swap(ord[j], ord[j - 1]); j--; }
    }
    bool changed = false; for (size_t k = 0; k < ord.size(); k++) if (ord[k] != k) changed = true;
    if (!changed) return;
    std::vector<uint8_t> tmp(pos.size() * 0x24);
    for (size_t k = 0; k < pos.size(); k++) memcpy(&tmp[k * 0x24], base + pos[ord[k]] * 0x24, 0x24);
    for (size_t k = 0; k < pos.size(); k++) memcpy(base + pos[k] * 0x24, &tmp[k * 0x24], 0x24);
    tag_layers_fixed++;
}
void __cdecl sort_translucent(void* base, size_t n, size_t sz, int (__cdecl* cmp)(const void*, const void*)) {
    if (enabled && last_vb_data && sz == 0x24) {
        uint8_t* e = static_cast<uint8_t*>(base);
        for (size_t i = 0; i < n; i++, e += 0x24) {
            const uint32_t start = *reinterpret_cast<uint32_t*>(e + 0x14), count = *reinterpret_cast<uint32_t*>(e + 0x18);
            if (!count || (uint64_t(start) + count) * 32 > last_vb_bytes) continue;
            float far_d = 0;
            for (uint32_t v = 0; v < count; v++) {
                const float rhw = *reinterpret_cast<const float*>(last_vb_data + size_t(start + v) * 32 + 0x0C);
                if (rhw > 0) far_d = std::max(far_d, 1.f / rhw);
            }
            if (far_d > 0) { *reinterpret_cast<float*>(e + 0x1C) = far_d; far_keys++; }
        }
    }
    game_qsort(base, n, sz, cmp);
    if (enabled && last_vb_data && sz == 0x24) order_tag_layers(static_cast<uint8_t*>(base), n);
    if (kAutoDumpVoile) auto_dump_voile(base, n, sz);   // releve de diagnostic (fige le jeu ~0,1 s : coupe)
    if (model_log && last_vb_data && sz == 0x24) {   // F10 : contenu de la liste transparente triee
        const uint8_t* e = static_cast<const uint8_t*>(base);
        fprintf(model_log, "LISTE TRANSPARENTE %u entrees\n", unsigned(n));
        for (size_t i = 0; i < n; i++, e += 0x24) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(e);
            const uint32_t start = w[5], count = w[6];
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, dn = 1e9f, df = 0;
            if (count && (uint64_t(start) + count) * 32 <= last_vb_bytes)
                for (uint32_t v = 0; v < count; v++) {
                    const float* q = reinterpret_cast<const float*>(last_vb_data + size_t(start + v) * 32);
                    x0 = std::min(x0, q[0]); x1 = std::max(x1, q[0]); y0 = std::min(y0, q[1]); y1 = std::max(y1, q[1]);
                    if (q[3] > 0) { dn = std::min(dn, 1.f / q[3]); df = std::max(df, 1.f / q[3]); }
                }
            fprintf(model_log, " T%u %08X %08X %08X %08X %08X deb %u nb %u cle %.3f prio %d | x %.0f..%.0f y %.0f..%.0f prof %.3f..%.3f\n",
                unsigned(i), w[0], w[1], w[2], w[3], w[4], start, count, *reinterpret_cast<const float*>(e + 0x1C), int(int8_t(e[0x20])),
                x0, x1, y0, y1, dn, df);
        }
    }
}
bool patch_call(uint32_t call_va, uint32_t target_va, void* hook) {
    uint8_t* c = at<uint8_t*>(call_va);
    const uintptr_t target = uintptr_t(target_va) + exe_delta;
    int32_t rel; memcpy(&rel, c + 1, 4);
    if (c[0] != 0xE8 || uintptr_t(c) + 5 + rel != target) { log("appel %08X inattendu\n", call_va); return false; }
    DWORD old; VirtualProtect(c, 5, PAGE_EXECUTE_READWRITE, &old);
    const int32_t nrel = int32_t(reinterpret_cast<uintptr_t>(hook) - (uintptr_t(c) + 5));
    memcpy(c + 1, &nrel, 4);
    VirtualProtect(c, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), c, 5);
    return true;
}
bool patch_relay(uint32_t relay_va, uint32_t target_va, void* hook) {
    uint8_t* relay = at<uint8_t*>(relay_va);
    const uintptr_t target = uintptr_t(target_va) + exe_delta;
    int32_t rel; memcpy(&rel, relay + 1, 4);
    if (relay[0] != 0xE9 || uintptr_t(relay) + 5 + rel != target) { log("relais %08X inattendu\n", relay_va); return false; }
    DWORD old; VirtualProtect(relay, 5, PAGE_EXECUTE_READWRITE, &old);
    const int32_t nrel = int32_t(reinterpret_cast<uintptr_t>(hook) - (uintptr_t(relay) + 5));
    memcpy(relay + 1, &nrel, 4);
    VirtualProtect(relay, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), relay, 5);
    return true;
}
// ---- Ville : 0x0043F890 dessine le bloc de la zone de la camera ([0xDF9D6C]) puis la liste de blocs visibles
// de cette zone (table [0xDFD0A0] + zone*8 + 4, terminee par un negatif). Un bloc n'est dessine que s'il est charge
// (0x00406DC5(0x004011CC(id)) == 2). Diagnostic : combien de blocs charges ne sont pas dans la liste.
using IntFn = int (__cdecl*)(int);
IntFn blk_ptr = nullptr, blk_state = nullptr;
using VoidIntFn = void (__cdecl*)(int);
VoidIntFn city_draw_orig = nullptr;
IntFn find_task = nullptr; VoidIntFn draw_block = nullptr; bool city_extend = true; unsigned city_extra_drawn = 0;
void __cdecl city_draw_hook(int arg) {
    city_draw_orig(arg);
    // Distance d'affichage : les autres blocs du niveau charge (identifiants 0..62) sont dessines aussi.
    // Sauf mode special (tache 0x6A active : le jeu utilise alors sa propre liste).
    if (enabled && city_extend) {
        const int task = find_task(0x6A);
        const bool special = task && *reinterpret_cast<const int*>(task + 0x864) != 0;
        const int area = *at<int*>(0x00DF9D6C);
        const uint8_t* tbl = *at<uint8_t**>(0x00DFD0A0);
        if (!special && area >= 0 && tbl) {
            bool listed[64] = {}; listed[area & 63] = true;
            for (const int* l = *reinterpret_cast<int* const*>(tbl + area * 8 + 4); l && *l >= 0; l++) if (*l < 64) listed[*l] = true;
            // blocs du niveau = reunion des listes de toutes les zones du meme niveau (y compris blocs communs 0 / 5)
            const int stage = blk_ptr(area);
            bool level[64] = {};
            for (int z = 0; z < 63; z++) {
                if (blk_ptr(z) != stage) continue;
                level[z] = true;
                for (const int* l = *reinterpret_cast<int* const*>(tbl + z * 8 + 4); l && *l >= 0; l++) if (*l < 64) level[*l] = true;
            }
            for (int id = 0; id < 63; id++)
                if (level[id] && !listed[id] && blk_state(blk_ptr(id)) == 2) { draw_block(id); city_extra_drawn++; }
            if (model_log) {
                fprintf(model_log, "VILLE zone %d niveau %d ; liste :", area, stage);
                for (int id = 0; id < 63; id++) if (listed[id]) fprintf(model_log, " %d", id);
                fprintf(model_log, " ; ajoutes :");
                for (int id = 0; id < 63; id++) if (level[id] && !listed[id] && blk_state(blk_ptr(id)) == 2) fprintf(model_log, " %d", id);
                fprintf(model_log, " ; non charges :");
                for (int id = 0; id < 63; id++) if (level[id] && blk_state(blk_ptr(id)) != 2) fprintf(model_log, " %d", id);
                fprintf(model_log, "\n");
            }
        }
    }
    static int logged = 0; static DWORD next = 0;
    if (false && logged < 10 && GetTickCount() >= next) {
        next = GetTickCount() + 5000;
        const int area = *at<int*>(0x00DF9D6C);
        const uint8_t* tbl = *at<uint8_t**>(0x00DFD0A0);
        std::set<int> listed; if (area >= 0) listed.insert(area);
        if (tbl && area >= 0) { const int* l = *reinterpret_cast<int* const*>(tbl + area * 8 + 4); for (; l && *l >= 0; l++) listed.insert(*l); }
        int loaded = 0, extra = 0; char ex[200] = ""; int k = 0;
        for (int id = 0; id < 64; id++) if (blk_state(blk_ptr(id)) == 2) { loaded++; if (!listed.count(id)) { extra++; if (k < 180) k += sprintf(ex + k, " %d", id); } }
        log("ville : zone %d, liste %u blocs, blocs charges %d dont %d hors liste :%s\n", area, unsigned(listed.size()), loaded, extra, ex);
        logged++;
    }
}
// Angle de vue horizontal des objets de la ville (0x004B29D0 via 0x00402063, 0x004B2920 via 0x0040205E) : coin calcule
// pour l'ecran 4:3 de la Dreamcast ; en 16:9 les objets des bords sont ecartes puis surgissent quand la camera tourne.
// On les declare toujours visibles : le test de chaque modele (sphere englobante, 0x00598D00) ecarte le hors-ecran.
using ConeFn = int (__cdecl*)(const float*);
ConeFn cone_a_orig = nullptr, cone_b_orig = nullptr;
int __cdecl cone_a_hook(const float* p) { return enabled ? 1 : cone_a_orig(p); }
int __cdecl cone_b_hook(const float* p) { return enabled ? 1 : cone_b_orig(p); }
void install_model_hooks() {
    exe_delta = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) - 0x400000;
    kCurMatrix = at<float**>(0x00DD5C40); kVertCount = at<uint32_t*>(0x00DD5FF0); kVertPtr = at<void**>(0x00DD5FEC); kWorkBuf = at<void**>(0x00DD5CE8); kViewMatrix = at<float**>(0x00DD5C48);
    end_strip = at<EndStripFn>(0x005B5C30);
    log("base de l'exe : %08X (decalage %08X)\n", unsigned(0x400000 + exe_delta), unsigned(exe_delta));
    void* hooks[5] = {(void*)hook0, (void*)hook1, (void*)hook2, (void*)hook3, (void*)hook4};
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t* relay = at<uint8_t*>(kModelHooks[i].relay);
        const uintptr_t target = uintptr_t(kModelHooks[i].target) + exe_delta;
        model_orig[i] = reinterpret_cast<ModelFn>(target);
        int32_t rel; memcpy(&rel, relay + 1, 4);
        if (relay[0] != 0xE9 || uintptr_t(relay) + 5 + rel != target) {
            log("relais %08X inattendu, interception abandonnee\n", kModelHooks[i].relay); continue;
        }
        DWORD old; VirtualProtect(relay, 5, PAGE_EXECUTE_READWRITE, &old);
        const int32_t nrel = int32_t(reinterpret_cast<uintptr_t>(hooks[i]) - (uintptr_t(relay) + 5));
        memcpy(relay + 1, &nrel, 4);
        VirtualProtect(relay, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), relay, 5);
        ok++;
    }
    log("fonctions de dessin de modele interceptees : %d sur 5\n", ok);
    log("tri des listes de dessin stabilise : %s\n", patch_relay(0x0040575E, 0x005B6060, (void*)sort_compare) ? "oui" : "non");
    game_qsort = at<QsortFn>(0x005D52C0);
    blk_ptr = at<IntFn>(0x004011CC); blk_state = at<IntFn>(0x00406DC5); city_draw_orig = at<VoidIntFn>(0x00401E6A);
    find_task = at<IntFn>(0x00405425); draw_block = at<VoidIntFn>(0x0043F5C0);
    cone_a_orig = at<ConeFn>(0x004B29D0); cone_b_orig = at<ConeFn>(0x004B2920);
    log("angle de vue des objets elargi : %s / %s\n", patch_relay(0x00402063, 0x004B29D0, (void*)cone_a_hook) ? "oui" : "non",
        patch_relay(0x0040205E, 0x004B2920, (void*)cone_b_hook) ? "oui" : "non");
    log("dessin de la ville intercepte : %s\n", patch_call(0x0043D15B, 0x00401E6A, (void*)city_draw_hook) ? "oui" : "non");
    tag_draw_orig = at<DrawObjFn>(0x00404BD3);
    log("tags rapproches de la camera : %s\n", patch_call(0x0042BB16, 0x00404BD3, (void*)tag_draw_hook) ? "oui" : "non");
    log("transparence classee par le sommet le plus lointain : %s\n", patch_call(0x005B6331, 0x005D52C0, (void*)sort_translucent) ? "oui" : "non");
}
// 0 commun, 1 reel, 2 intermediaire (d'apres le premier sommet du dessin)
int classify(uint32_t start) {
    auto in = [&](const std::vector<Range>& v) {
        auto it = std::upper_bound(v.begin(), v.end(), start, [](uint32_t s, const Range& r) { return s < r.a; });
        return it != v.begin() && start < (it - 1)->b;
    };
    auto it = std::upper_bound(mranges.begin(), mranges.end(), start, [](uint32_t s2, const MRange& r) { return s2 < r.a; });
    if (it != mranges.begin() && start < (it - 1)->b) return int((it - 1)->mask);
    return 0;
}

// ================================================================ cote Direct3D : enregistrement et rejeu
using PresentFn = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using BeginFn = HRESULT (WINAPI*)(IDirect3DDevice9*);
using ClearFn = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using ViewportFn = HRESULT (WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using RSFn = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SSFn = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using TexFn = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using DrawFn = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawUPFn = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DeclFn = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using FVFFn = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD);
using VSFn = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using PSFn = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using ConstFn = HRESULT (WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using StreamFn = HRESULT (WINAPI*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
using CreateVBFn = HRESULT (WINAPI*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using CreateDevFn = HRESULT (WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
PresentFn o_present; ResetFn o_reset; BeginFn o_begin, o_end; ClearFn o_clear; ViewportFn o_viewport; RSFn o_rs; SSFn o_ss;
TexFn o_tex; DrawFn o_draw; DrawUPFn o_drawup; DeclFn o_decl; FVFFn o_fvf; VSFn o_vs; PSFn o_ps; ConstFn o_vsc, o_psc;
StreamFn o_stream; CreateVBFn o_createvb; CreateDevFn o_createdev;

enum Op : uint8_t { OP_CLEAR, OP_VIEWPORT, OP_RS, OP_SS, OP_TEX, OP_DRAW, OP_DRAWUP, OP_DECL, OP_FVF, OP_VS, OP_PS, OP_VSC, OP_PSC, OP_STREAM, OP_STRETCH };
struct Cmd { Op op; uint32_t a = 0, b = 0, c = 0, d = 0; void* p = nullptr; float f = 0; uint32_t blob = 0, blob_n = 0; };
struct Frame { std::vector<Cmd> cmds; std::vector<uint8_t> blobs; bool valid = false; unsigned interp_draws = 0, blur_draws = 0; };
Frame frame;
void new_frame_backups(); void release_backups();
std::vector<IUnknown*> held;   // surfaces des copies d'ecran gardees vivantes jusqu'a la prochaine image
bool recording = false, in_replay = false;
IDirect3DVertexBuffer9* cur_vb = nullptr; UINT cur_stride = 0;
IDirect3DVertexBuffer9* game_vbs[3] = {}; int game_vb_count = 0;
IDirect3DStateBlock9* sb_begin = nullptr; IDirect3DStateBlock9* sb_end = nullptr;
uint32_t blob(const void* p, size_t n) { const uint32_t at = (uint32_t)frame.blobs.size(); frame.blobs.insert(frame.blobs.end(), (const uint8_t*)p, (const uint8_t*)p + n); return at; }
void push(const Cmd& c) { if (recording && !in_replay) frame.cmds.push_back(c); }
unsigned prim_vertices(UINT t, UINT n) {
    switch (t) { case D3DPT_TRIANGLELIST: return n*3; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n+2;
        case D3DPT_LINELIST: return n*2; case D3DPT_LINESTRIP: return n+1; default: return n; }
}
bool is_game_vb(IDirect3DVertexBuffer9* vb) { for (int i = 0; i < game_vb_count; i++) if (game_vbs[i] == vb) return true; return false; }

HRESULT WINAPI h_begin(IDirect3DDevice9* d) {
    const HRESULT hr = o_begin(d);
    if (!in_replay) {
        frame.cmds.clear(); frame.blobs.clear(); frame.valid = false; frame.interp_draws = 0; frame.blur_draws = 0;
        for (IUnknown* u : held) u->Release(); held.clear();
        new_frame_backups();
        if (!sb_begin) d->CreateStateBlock(D3DSBT_ALL, &sb_begin); else sb_begin->Capture();
        recording = sb_begin != nullptr;
    }
    return hr;
}
HRESULT WINAPI h_end(IDirect3DDevice9* d) { if (!in_replay && recording) { frame.valid = true; recording = false; } return o_end(d); }
HRESULT WINAPI h_clear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD fl, D3DCOLOR col, float z, DWORD s) {
    if (recording && !in_replay) { Cmd c{OP_CLEAR}; c.a = n; c.b = fl; c.c = col; c.d = s; c.f = z; if (n && r) { c.blob = blob(r, n * sizeof(D3DRECT)); c.blob_n = n; } push(c); }
    return o_clear(d, n, r, fl, col, z, s);
}
HRESULT WINAPI h_viewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    if (recording && !in_replay && v) { Cmd c{OP_VIEWPORT}; c.blob = blob(v, sizeof *v); push(c); }
    return o_viewport(d, v);
}
HRESULT WINAPI h_rs(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v) { Cmd c{OP_RS}; c.a = s; c.b = v; push(c); return o_rs(d, s, v); }
HRESULT WINAPI h_ss(IDirect3DDevice9* d, DWORD st_, D3DSAMPLERSTATETYPE t, DWORD v) {
    {   // diagnostic : combinaisons d'etats d'echantillonnage demandees par le jeu
        static std::set<uint32_t> seen;
        const uint32_t key = (st_ << 24) | (uint32_t(t) << 16) | (v & 0xFFFF);
        if (false && !in_replay && seen.size() < 80 && seen.insert(key).second) log("echantillonneur %u etat %d = %u\n", st_, int(t), v);
    } Cmd c{OP_SS}; c.a = st_; c.b = t; c.c = v; push(c); return o_ss(d, st_, t, v); }
IDirect3DBaseTexture9* cur_tex0 = nullptr;
HRESULT WINAPI h_tex(IDirect3DDevice9* d, DWORD st_, IDirect3DBaseTexture9* t) {
    if (false && t && !in_replay) {   // diagnostic : niveaux de reduction des textures
        static std::set<IDirect3DBaseTexture9*> seen; static unsigned with = 0, without = 0, logged = 0;
        if (seen.size() < 4000 && seen.insert(t).second) {
            if (t->GetLevelCount() > 1) with++; else without++;
            if ((with + without) % 200 == 0 && logged < 20) { logged++; log("textures vues : %u avec niveaux de reduction, %u sans\n", with, without); }
        }
    } if (st_ == 0 && !in_replay) cur_tex0 = t; Cmd c{OP_TEX}; c.a = st_; c.p = t; push(c); return o_tex(d, st_, t); }
HRESULT WINAPI h_decl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* v) { Cmd c{OP_DECL}; c.p = v; push(c); return o_decl(d, v); }
HRESULT WINAPI h_fvf(IDirect3DDevice9* d, DWORD f) { Cmd c{OP_FVF}; c.a = f; push(c); return o_fvf(d, f); }
HRESULT WINAPI h_vs(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { Cmd c{OP_VS}; c.p = s; push(c); return o_vs(d, s); }
HRESULT WINAPI h_ps(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { Cmd c{OP_PS}; c.p = s; push(c); return o_ps(d, s); }
HRESULT WINAPI h_vsc(IDirect3DDevice9* d, UINT r, const float* v, UINT n) {
    if (recording && !in_replay && v) { Cmd c{OP_VSC}; c.a = r; c.b = n; c.blob = blob(v, n * 16); push(c); } return o_vsc(d, r, v, n);
}
HRESULT WINAPI h_psc(IDirect3DDevice9* d, UINT r, const float* v, UINT n) {
    if (recording && !in_replay && v) { Cmd c{OP_PSC}; c.a = r; c.b = n; c.blob = blob(v, n * 16); push(c); } return o_psc(d, r, v, n);
}
// Journal F10 : operations de copie / changement de cible (effets d'ecran du jeu).
using StretchFn = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
using SetRTFn = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using UpdSurfFn = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*);
using UpdTexFn = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
using GetRTDFn = HRESULT (WINAPI*)(IDirect3DDevice9*, IDirect3DSurface9*, IDirect3DSurface9*);
StretchFn o_stretch; SetRTFn o_setrt; UpdSurfFn o_updsurf; UpdTexFn o_updtex; GetRTDFn o_getrtd;
struct Backup { IDirect3DSurface9* dst; IDirect3DSurface9* copy; bool saved; };
std::vector<Backup> backups;   // une entree par surface de destination rencontree
void backup_surface(IDirect3DDevice9* d, IDirect3DSurface9* dst) {
    for (Backup& b : backups) if (b.dst == dst) {
        if (!b.saved && b.copy) b.saved = SUCCEEDED(o_stretch(d, dst, nullptr, b.copy, nullptr, D3DTEXF_NONE));
        return;
    }
    D3DSURFACE_DESC desc; if (FAILED(dst->GetDesc(&desc))) return;
    Backup nb{dst, nullptr, false}; dst->AddRef();
    if (SUCCEEDED(d->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &nb.copy, nullptr)))
        nb.saved = SUCCEEDED(o_stretch(d, dst, nullptr, nb.copy, nullptr, D3DTEXF_NONE));
    log("sauvegarde de surface de flou %p (%ux%u) : %s\n", (void*)dst, desc.Width, desc.Height, nb.copy ? "ok" : "echec");
    backups.push_back(nb);
}
void restore_backups(IDirect3DDevice9* d) {   // avant l'image intermediaire
    for (Backup& b : backups) if (b.saved && b.copy) o_stretch(d, b.copy, nullptr, b.dst, nullptr, D3DTEXF_NONE);
}
void new_frame_backups() { for (Backup& b : backups) b.saved = false; }
void release_backups() { for (Backup& b : backups) { if (b.copy) b.copy->Release(); b.dst->Release(); } backups.clear(); }
HRESULT WINAPI h_stretch(IDirect3DDevice9* d, IDirect3DSurface9* a, const RECT* ra, IDirect3DSurface9* b, const RECT* rb, D3DTEXTUREFILTERTYPE f) {
    // Copie d'ecran du jeu (effet de flou du spray...) : enregistree pour etre refaite dans chaque image rejouee.
    // Avant la premiere ecriture de l'image dans une surface, on sauvegarde son contenu (= copie faite pour l'image
    // affichee precedemment) : l'image intermediaire doit partir de cet etat, pas de celui calcule pour l'image suivante.
    if (recording && !in_replay && b) backup_surface(d, b);
    if (recording && !in_replay && a && b) {
        Cmd c{OP_STRETCH}; c.p = a; c.a = (uint32_t)(uintptr_t)b; c.b = f; c.c = (ra ? 1u : 0u) | (rb ? 2u : 0u);
        RECT rr[2] = {}; if (ra) rr[0] = *ra; if (rb) rr[1] = *rb; c.blob = blob(rr, sizeof rr);
        a->AddRef(); b->AddRef(); held.push_back(a); held.push_back(b); push(c);
    }
    if (model_log && !in_replay) fprintf(model_log, "StretchRect %p -> %p (cmd %u)\n", (void*)a, (void*)b, (unsigned)frame.cmds.size());
    return o_stretch(d, a, ra, b, rb, f);
}
HRESULT WINAPI h_setrt(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* rt) {
    if (model_log && !in_replay) fprintf(model_log, "SetRenderTarget %u %p (cmd %u)\n", (unsigned)i, (void*)rt, (unsigned)frame.cmds.size());
    return o_setrt(d, i, rt);
}
HRESULT WINAPI h_updsurf(IDirect3DDevice9* d, IDirect3DSurface9* a, const RECT* r, IDirect3DSurface9* b, const POINT* pt) {
    if (model_log && !in_replay) fprintf(model_log, "UpdateSurface %p -> %p (cmd %u)\n", (void*)a, (void*)b, (unsigned)frame.cmds.size());
    return o_updsurf(d, a, r, b, pt);
}
HRESULT WINAPI h_updtex(IDirect3DDevice9* d, IDirect3DBaseTexture9* a, IDirect3DBaseTexture9* b) {
    if (model_log && !in_replay) fprintf(model_log, "UpdateTexture %p -> %p (cmd %u)\n", (void*)a, (void*)b, (unsigned)frame.cmds.size());
    return o_updtex(d, a, b);
}
HRESULT WINAPI h_getrtd(IDirect3DDevice9* d, IDirect3DSurface9* a, IDirect3DSurface9* b) {
    if (model_log && !in_replay) fprintf(model_log, "GetRenderTargetData %p -> %p (cmd %u)\n", (void*)a, (void*)b, (unsigned)frame.cmds.size());
    return o_getrtd(d, a, b);
}
HRESULT WINAPI h_stream(IDirect3DDevice9* d, UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    if (n == 0) { cur_vb = vb; cur_stride = stride; }
    Cmd c{OP_STREAM}; c.a = n; c.b = off; c.c = stride; c.p = vb; push(c); return o_stream(d, n, vb, off, stride);
}
HRESULT WINAPI h_draw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT n) {
    // Dessins du grand tampon du jeu : classes d'apres les plages notees pendant le dessin des modeles.
    const int kind = (!in_replay && is_game_vb(cur_vb)) ? classify(start) : 0;
    if (recording && !in_replay) { Cmd c{OP_DRAW}; c.a = t; c.b = start; c.c = n; c.d = (uint32_t)kind; push(c); if (kind & SUB_I) frame.interp_draws++; if (kind & (SUB_B3 | SUB_B4)) frame.blur_draws++; }
    {   // etats de rendu des dessins de tags (30 premiers) : liste, ecriture de profondeur, alpha
        static int tag_states_logged = 0;
        if (false && !in_replay && tag_states_logged < 30 && is_game_vb(cur_vb))
            for (const Range& r : tag_ranges) if (start >= r.a && start < r.b) {
                DWORD zw = 0, zf = 0, at = 0, ab = 0, ar = 0, sb = 0, db = 0, zb = 0;
                d->GetRenderState(D3DRS_ZWRITEENABLE, &zw); d->GetRenderState(D3DRS_ZFUNC, &zf); d->GetRenderState(D3DRS_ZENABLE, &zb);
                d->GetRenderState(D3DRS_ALPHATESTENABLE, &at); d->GetRenderState(D3DRS_ALPHAREF, &ar);
                d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab); d->GetRenderState(D3DRS_SRCBLEND, &sb); d->GetRenderState(D3DRS_DESTBLEND, &db);
                log("dessin de tag %u+%u : zenable %u zwrite %u zfunc %u alphatest %u ref %u blend %u src %u dst %u\n", start, n, zb, zw, zf, at, ar, ab, sb, db);
                tag_states_logged++; break;
            }
    }
    if (model_log && !in_replay && is_game_vb(cur_vb)) fprintf(model_log, "dessin %u+%u type %d classe %d tex %p\n", start, n, int(t), kind, (void*)cur_tex0);
    if (kind && !(kind & SUB_R) && !in_replay) return D3D_OK;      // copies intermediaire / flou : pas dans l'image en direct
    return o_draw(d, t, start, n);
}
HRESULT WINAPI h_drawup(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* data, UINT stride) {
    if (recording && !in_replay && data) { Cmd c{OP_DRAWUP}; c.a = t; c.b = n; c.c = stride; c.blob = blob(data, size_t(prim_vertices(t, n)) * stride); push(c); }
    return o_drawup(d, t, n, data, stride);
}
// Chaque verrouillage d'un tampon du jeu commence un nouveau lot de sommets (le compteur repart de 0) :
// le jeu vide ses listes plusieurs fois par image (3D puis interface). Les plages notees pendant le lot
// precedent ne doivent pas classer les dessins du lot suivant (sinon la fleche de tag passe pour "reelle").
using VBLockFn = HRESULT (WINAPI*)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
VBLockFn o_vblock = nullptr; unsigned vb_locks = 0;
const uint8_t* last_vb_data = nullptr; uint32_t last_vb_bytes = 0;   // memoire systeme du tampon (pool managed)
HRESULT WINAPI h_vblock(IDirect3DVertexBuffer9* vb, UINT off, UINT len, void** p, DWORD fl) {
    if (!in_replay && is_game_vb(vb)) { interp_ranges.clear(); real_ranges.clear(); blur3_ranges.clear(); blur4_ranges.clear(); mranges.clear(); vb_locks++; }
    const HRESULT hr = o_vblock(vb, off, len, p, fl);
    if (SUCCEEDED(hr) && p && *p && !in_replay && is_game_vb(vb)) {
        D3DVERTEXBUFFER_DESC desc; vb->GetDesc(&desc);
        last_vb_data = static_cast<const uint8_t*>(*p); last_vb_bytes = desc.Size - off;
    }
    return hr;
}
HRESULT WINAPI h_createvb(IDirect3DDevice9* d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* sh) {
    // Les grands tampons du jeu (4 160 000 octets) sont agrandis x3 : place pour les copies intermediaires
    // (le jeu verrouille le tampon entier et n'en verifie pas la capacite).
    const bool game = !in_replay && len == 4160000;
    const HRESULT hr = o_createvb(d, game ? len * 5 : len, usage, fvf, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out && game && game_vb_count < 3) {
        game_vbs[game_vb_count++] = *out;
        if (!o_vblock) o_vblock = (VBLockFn)patch(*out, 11, (void*)h_vblock);
    }
    if (game) log("tampon de sommets du jeu agrandi : %u -> %u octets (%s)\n", len, len * 5, SUCCEEDED(hr) ? "ok" : "echec");
    return hr;
}

// filtre : 1 = image reelle (reel + commun), 2 = image intermediaire (intermediaire + commun)
void replay(IDirect3DDevice9* d, int filter) {
    in_replay = true;
    sb_begin->Apply();
    o_begin(d);
    for (const Cmd& c : frame.cmds) {
        switch (c.op) {
        case OP_CLEAR: o_clear(d, c.a, c.blob_n ? (const D3DRECT*)(frame.blobs.data() + c.blob) : nullptr, c.b, c.c, c.f, c.d); break;
        case OP_VIEWPORT: o_viewport(d, (const D3DVIEWPORT9*)(frame.blobs.data() + c.blob)); break;
        case OP_RS: o_rs(d, (D3DRENDERSTATETYPE)c.a, c.b); break;
        case OP_SS: o_ss(d, c.a, (D3DSAMPLERSTATETYPE)c.b, c.c); break;
        case OP_TEX: o_tex(d, c.a, (IDirect3DBaseTexture9*)c.p); break;
        case OP_DECL: o_decl(d, (IDirect3DVertexDeclaration9*)c.p); break;
        case OP_FVF: o_fvf(d, c.a); break;
        case OP_VS: o_vs(d, (IDirect3DVertexShader9*)c.p); break;
        case OP_PS: o_ps(d, (IDirect3DPixelShader9*)c.p); break;
        case OP_VSC: o_vsc(d, c.a, (const float*)(frame.blobs.data() + c.blob), c.b); break;
        case OP_PSC: o_psc(d, c.a, (const float*)(frame.blobs.data() + c.blob), c.b); break;
        case OP_STREAM: o_stream(d, c.a, (IDirect3DVertexBuffer9*)c.p, c.b, c.c); break;
        case OP_DRAW: if (c.d == 0 || (c.d & uint32_t(filter))) o_draw(d, (D3DPRIMITIVETYPE)c.a, c.b, c.c); break;
        case OP_STRETCH: {
            const RECT* rr = (const RECT*)(frame.blobs.data() + c.blob);
            o_stretch(d, (IDirect3DSurface9*)c.p, (c.c & 1) ? &rr[0] : nullptr, (IDirect3DSurface9*)(uintptr_t)c.a, (c.c & 2) ? &rr[1] : nullptr, (D3DTEXTUREFILTERTYPE)c.b);
            break; }
        case OP_DRAWUP: o_drawup(d, (D3DPRIMITIVETYPE)c.a, c.b, frame.blobs.data() + c.blob, c.c); break;
        }
    }
    o_end(d);
    in_replay = false;
}

unsigned long frame_no = 0; bool f8_down = false;
// Captures automatiques de controle (series de 4 images : intermediaire, reelle, intermediaire, reelle).
int set_phase = 0, sets_done = 0, sets_max = 0; unsigned long next_set = 600;
using SaveFn = HRESULT (WINAPI*)(LPCSTR, DWORD, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
void save_bb(IDirect3DDevice9* d, const char* what) {
    static SaveFn save = nullptr;
    if (!save) { HMODULE m = LoadLibraryA("d3dx9_43.dll"); if (m) save = (SaveFn)GetProcAddress(m, "D3DXSaveSurfaceToFileA"); }
    IDirect3DSurface9* bb = nullptr; IDirect3DSurface9* rt = nullptr;
    if (save && SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        D3DSURFACE_DESC desc; bb->GetDesc(&desc);
        if (SUCCEEDED(d->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
            SUCCEEDED(d->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE))) {
            char name[80]; if (!strncmp(what, "voile", 5)) sprintf(name, "jsr60-%s.png", what); else sprintf(name, "jsr60-serie-%d-image%lu-%s.png", sets_done, frame_no, what);
            save(name, 3 /* PNG */, rt, nullptr, nullptr);
        }
    }
    if (rt) rt->Release(); if (bb) bb->Release();
}
std::chrono::steady_clock::time_point t_last;
void new_game_frame() {
    if (have_view_cur) { memcpy(view_prev, view_cur, 64); have_view_prev = true; have_view_cur = false; }
    std::swap(inst_cur, inst_prev); inst_cur->clear();
    interp_ranges.clear(); real_ranges.clear(); blur3_ranges.clear(); blur4_ranges.clear(); mranges.clear(); tag_ranges.clear();
}
// Moyenne de deux sous-images : la premiere est recopiee (resolue) dans une texture, la seconde est rejouee dans
// le tampon arriere, puis la texture est posee par-dessus a 50 %.
IDirect3DTexture9* blur_tex = nullptr; UINT blur_w = 0, blur_h = 0;
bool resolve_to_blur(IDirect3DDevice9* d) {
    IDirect3DSurface9* bb = nullptr; if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC desc; bb->GetDesc(&desc);
    if (blur_tex && (blur_w != desc.Width || blur_h != desc.Height)) { blur_tex->Release(); blur_tex = nullptr; }
    if (!blur_tex && SUCCEEDED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &blur_tex, nullptr))) {
        blur_w = desc.Width; blur_h = desc.Height; log("texture du flou de mouvement : %ux%u\n", blur_w, blur_h);
    }
    bool ok = false;
    if (blur_tex) { IDirect3DSurface9* sf = nullptr; if (SUCCEEDED(blur_tex->GetSurfaceLevel(0, &sf))) { ok = SUCCEEDED(o_stretch(d, bb, nullptr, sf, nullptr, D3DTEXF_NONE)); sf->Release(); } }
    bb->Release(); return ok;
}
void blend_blur(IDirect3DDevice9* d) {
    in_replay = true;
    o_begin(d);
    D3DVIEWPORT9 vp = {0, 0, blur_w, blur_h, 0.f, 1.f}; o_viewport(d, &vp);
    o_vs(d, nullptr); o_ps(d, nullptr); o_fvf(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    o_rs(d, D3DRS_ZENABLE, FALSE); o_rs(d, D3DRS_ZWRITEENABLE, FALSE); o_rs(d, D3DRS_ALPHATESTENABLE, FALSE);
    o_rs(d, D3DRS_CULLMODE, D3DCULL_NONE); o_rs(d, D3DRS_FOGENABLE, FALSE); o_rs(d, D3DRS_STENCILENABLE, FALSE);
    o_rs(d, D3DRS_SCISSORTESTENABLE, FALSE); o_rs(d, D3DRS_COLORWRITEENABLE, 0xF); o_rs(d, D3DRS_LIGHTING, FALSE);
    o_rs(d, D3DRS_ALPHABLENDENABLE, TRUE); o_rs(d, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); o_rs(d, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    o_rs(d, D3DRS_BLENDOP, D3DBLENDOP_ADD); o_rs(d, D3DRS_TEXTUREFACTOR, 0x80FFFFFF);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    o_ss(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    o_ss(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_SRGBTEXTURE, 0);
    o_tex(d, 0, blur_tex); o_tex(d, 1, nullptr);
    const float W = float(blur_w) - 0.5f, H = float(blur_h) - 0.5f;
    struct V { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {W, -0.5f, 0, 1, 1, 0}, {-0.5f, H, 0, 1, 0, 1}, {W, H, 0, 1, 1, 1}};
    o_drawup(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    o_end(d);
    in_replay = false;
}
HRESULT WINAPI h_present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    static bool f10_down = false; static int f10_series = 0;
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !f10_down && set_phase == 0) { sets_max = sets_done + 3; next_set = 0; f10_series++; log("F10 : captures (series %d a %d)\n", sets_done, sets_done + 2); }
    f10_down = f10;
    {   // F1 coupe, F2 leger, F3 intermediaire, F4 fort
        static bool fk_down[4] = {};
        static const char* names[4] = {"coupe", "leger", "intermediaire", "fort"};
        for (int k = 0; k < 4; k++) {
            const bool dn = (GetAsyncKeyState(VK_F1 + k) & 0x8000) != 0;
            if (dn && !fk_down[k] && blur_level != k) { blur_level = k; log("F%d : flou de mouvement %s\n", k + 1, names[k]); }
            fk_down[k] = dn;
        }
    }
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !f8_down) { enabled = !enabled; log("F8 : interpolation %s\n", enabled ? "activee" : "coupee"); }
    f8_down = f8;
    bool two = false;
    if (frame.valid && frame.interp_draws > 0 && sb_begin) {
        if (!sb_end) d->CreateStateBlock(D3DSBT_ALL, &sb_end); else sb_end->Capture();
        if (sb_end) {
            if (set_phase == 0 && sets_done < sets_max && frame_no >= next_set) { set_phase = 1; next_set = frame_no + 2; }
            const bool blur = frame.blur_draws > 0;
            bool mixA = false, mixB = false;
            if (blur) { restore_backups(d); replay(d, SUB_B3); mixA = resolve_to_blur(d); }
            restore_backups(d);
            replay(d, SUB_I);                                    // image intermediaire
            if (mixA) blend_blur(d);
            if (set_phase) save_bb(d, set_phase == 1 ? "1-intermediaire" : "3-intermediaire");
            o_present(d, a, b, w, r);
            if (blur) { replay(d, SUB_B4); mixB = resolve_to_blur(d); }
            replay(d, SUB_R);                                    // image reelle
            if (mixB) blend_blur(d);
            if (set_phase) { save_bb(d, set_phase == 1 ? "2-reelle" : "4-reelle"); if (++set_phase > 2) { set_phase = 0; sets_done++; } }
            sb_end->Apply(); two = true;
        }
    }
    if (voile_shot) {
        voile_shot = false; char nm[32]; sprintf(nm, "voile%d", voile_dumps % 10);
        const int keep = sets_done; sets_done = 99; save_bb(d, nm); sets_done = keep; voile_dumps++;
    }
    const HRESULT hr = o_present(d, a, b, w, r);
    if (kStatsLog && frame_no % 300 == 0) {   // statistiques periodiques : coupees (ecriture disque toutes les 10 s)
        const auto now = std::chrono::steady_clock::now();
        log("image %lu : %s, modeles %u (a mi-chemin %u, sans precedent %u, sauts %u, copies vides %u, interface %u), dessins intermediaires %u, verrous %u (%.1f s)\n",
            frame_no, two ? "2 images" : "1 image", st.models, st.interp, st.same, st.jump, st.empty, st.hud, frame.interp_draws, vb_locks,
            std::chrono::duration<double>(now - t_last).count());
        if (tags_pulled) log("  modeles de tags rapproches : %u, couches de tags reordonnees : %u\n", tags_pulled, tag_layers_fixed); tags_pulled = 0; tag_layers_fixed = 0; t_last = now; st = Stats{}; vb_locks = 0;
    }
    frame_no++; frame.valid = false;
    if (model_log) { fclose(model_log); model_log = nullptr; }
    if (sets_done < sets_max) {   // images capturees sera capturee : journal des modeles
        char name[80]; sprintf(name, "jsr60-modeles-%lu.txt", frame_no); model_log = fopen(name, "w");
    }
    new_game_frame();
    return hr;
}
void upgrade_depth(D3DPRESENT_PARAMETERS* pp); void set_anisotropy(IDirect3DDevice9* d);
HRESULT WINAPI h_reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    if (sb_begin) { sb_begin->Release(); sb_begin = nullptr; } if (sb_end) { sb_end->Release(); sb_end = nullptr; }
    frame.valid = false; frame.cmds.clear(); for (IUnknown* u : held) u->Release(); held.clear(); release_backups();
    if (blur_tex) { blur_tex->Release(); blur_tex = nullptr; }
    upgrade_depth(pp);
    const HRESULT hr = o_reset(d, pp);
    log("Reset : %s\n", SUCCEEDED(hr) ? "ok" : "echec");
    if (SUCCEEDED(hr)) set_anisotropy(d);
    return hr;
}
// Tampon de profondeur : le shader du jeu y range 0.9*(1-1/profondeur) au-dela d'une unite. En 16 bits, un tag
// et le mur sur lequel il est peint ne se distinguent plus a distance (voile blanc qui change avec l'angle).
// On passe le tampon automatique en 24 bits (meme nombre d'octets en pratique, aucun cout notable).
const char* fmt_name(D3DFORMAT f) {
    switch (f) { case D3DFMT_D16: return "D16"; case D3DFMT_D16_LOCKABLE: return "D16_LOCKABLE"; case D3DFMT_D15S1: return "D15S1";
        case D3DFMT_D24S8: return "D24S8"; case D3DFMT_D24X8: return "D24X8"; case D3DFMT_D32: return "D32"; default: return "autre"; }
}
void upgrade_depth(D3DPRESENT_PARAMETERS* pp) {
    if (!pp) return;
    log("tampon de profondeur demande : auto=%d format=%s (%d)\n", pp->EnableAutoDepthStencil, fmt_name(pp->AutoDepthStencilFormat), int(pp->AutoDepthStencilFormat));
    if (pp->EnableAutoDepthStencil && (pp->AutoDepthStencilFormat == D3DFMT_D16 || pp->AutoDepthStencilFormat == D3DFMT_D16_LOCKABLE ||
                                       pp->AutoDepthStencilFormat == D3DFMT_D15S1)) {
        pp->AutoDepthStencilFormat = D3DFMT_D24S8; log("  -> passe en D24S8\n");
    }
}
// Filtrage anisotrope : le jeu demande MINFILTER = ANISOTROPIC mais ne regle jamais MAXANISOTROPY (reste a 1,
// soit un simple filtrage lineaire -> scintillement au loin). On le met au maximum de la carte (16 au plus).
void set_anisotropy(IDirect3DDevice9* d) {
    D3DCAPS9 caps; DWORD a = 16;
    if (SUCCEEDED(d->GetDeviceCaps(&caps))) a = std::min<DWORD>(16, std::max<DWORD>(1, caps.MaxAnisotropy));
    for (DWORD st_ = 0; st_ < 8; st_++) o_ss(d, st_, D3DSAMP_MAXANISOTROPY, a);
    log("filtrage anisotrope regle a x%u\n", a);
}
HRESULT WINAPI h_createdev(IDirect3D9* self, UINT ad, D3DDEVTYPE ty, HWND w, DWORD fl, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const D3DFORMAT asked = pp ? pp->AutoDepthStencilFormat : D3DFMT_UNKNOWN;
    upgrade_depth(pp);
    HRESULT hr = o_createdev(self, ad, ty, w, fl, pp, out);
    if (FAILED(hr) && pp && pp->AutoDepthStencilFormat != asked) {   // refuse par la carte : format d'origine
        log("  D24S8 refuse, retour au format d'origine\n"); pp->AutoDepthStencilFormat = asked;
        hr = o_createdev(self, ad, ty, w, fl, pp, out);
    }
    if (SUCCEEDED(hr) && out && *out && !o_present) {
        IDirect3DDevice9* d = *out;
        o_reset = (ResetFn)patch(d, 16, (void*)h_reset);
        o_present = (PresentFn)patch(d, 17, (void*)h_present);
        o_createvb = (CreateVBFn)patch(d, 26, (void*)h_createvb);
        o_begin = (BeginFn)patch(d, 41, (void*)h_begin);
        o_end = (BeginFn)patch(d, 42, (void*)h_end);
        o_clear = (ClearFn)patch(d, 43, (void*)h_clear);
        o_viewport = (ViewportFn)patch(d, 47, (void*)h_viewport);
        o_rs = (RSFn)patch(d, 57, (void*)h_rs);
        o_tex = (TexFn)patch(d, 65, (void*)h_tex);
        o_ss = (SSFn)patch(d, 69, (void*)h_ss);
        o_draw = (DrawFn)patch(d, 81, (void*)h_draw);
        o_drawup = (DrawUPFn)patch(d, 83, (void*)h_drawup);
        o_decl = (DeclFn)patch(d, 87, (void*)h_decl);
        o_fvf = (FVFFn)patch(d, 89, (void*)h_fvf);
        o_vs = (VSFn)patch(d, 92, (void*)h_vs);
        o_vsc = (ConstFn)patch(d, 94, (void*)h_vsc);
        o_stream = (StreamFn)patch(d, 100, (void*)h_stream);
        o_ps = (PSFn)patch(d, 107, (void*)h_ps);
        o_psc = (ConstFn)patch(d, 109, (void*)h_psc);
        o_updsurf = (UpdSurfFn)patch(d, 30, (void*)h_updsurf);
        o_updtex = (UpdTexFn)patch(d, 31, (void*)h_updtex);
        o_getrtd = (GetRTDFn)patch(d, 32, (void*)h_getrtd);
        o_stretch = (StretchFn)patch(d, 34, (void*)h_stretch);
        o_setrt = (SetRTFn)patch(d, 37, (void*)h_setrt);
        set_anisotropy(d);
        log("peripherique : %ux%u, fenetre=%d, intervalle=%u, multisample=%d\n", pp->BackBufferWidth, pp->BackBufferHeight,
            pp->Windowed, pp->PresentationInterval, pp->MultiSampleType);
        t_last = std::chrono::steady_clock::now();
    }
    return hr;
}
bool load_real() {
    if (real_d3d9) return true;
    wchar_t path[MAX_PATH]; GetSystemDirectoryW(path, MAX_PATH); wcscat_s(path, L"\\d3d9.dll");
    real_d3d9 = LoadLibraryW(path);
    if (real_d3d9) real_create = (CreateFn)GetProcAddress(real_d3d9, "Direct3DCreate9");
    return real_create != nullptr;
}
}
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (!load_real()) return nullptr;
    static bool once = false;
    if (!once) {
        once = true;
        inst_cur = new InstMap(); inst_prev = new InstMap();
        install_model_hooks();
    }
    IDirect3D9* d3d = real_create(sdk);
    if (d3d && !o_createdev) o_createdev = (CreateDevFn)patch(d3d, 16, (void*)h_createdev);
    return d3d;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
