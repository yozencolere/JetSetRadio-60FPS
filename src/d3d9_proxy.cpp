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
#include <mmsystem.h>
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

bool host_is_game();
namespace {
void tick_present();
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
bool my_sort = true;
bool tag_fixes = true;   // F5 (essai) : rapprochement + ordre des couches des objets dessines par 0x42BA20   // F6 (essai) : tri des transparents d'origine
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
// ---- Une image par rafraichissement de l'ecran, quelle que soit sa frequence.
// Pour chaque image du jeu, nsub images sont affichees, une par rafraichissement de l'ecran, chacune montrant la
// scene au moment exact ou elle apparait : t = 0 (etat precedent du jeu) .. 1 (etat actuel). Masques : bit j = image
// j, bit 8+j = copie de flou de l'image j (seulement si nsub <= 2), SUB_LIVE = copie reelle (dessin en direct).
constexpr int kMaxSub = 6;
enum : uint32_t { SUB_LIVE = 1u << 16, SUB_DEAD = 1u << 17 };
int nsub = 2; float sub_t[kMaxSub] = {0.5f, 1.f}; bool sub_blur = true;   // fixes pour l'image en cours de calcul
int64_t sub_when[kMaxSub] = {};   // date d'affichage visee de chaque image (horloge QPC)
std::vector<Range> mranges_dead;   // copies de flou rejetees (jamais dessinees)
// Niveaux (touches F1 a F4) : 0 coupe, 1 leger, 2 intermediaire, 3 fort (obturateur 180 degres, par defaut).
int blur_level = 2;   // flou par defaut : intermediaire (F3) ; F1 coupe, F2 leger, F4 fort
float blur_s() { static const float k[4] = {0.f, 0.0833f, 0.1667f, 0.25f}; return k[blur_level & 3]; }
// Matrices par (modele, rang d'apparition) : image precedente et image en cours.
struct KeyHash { size_t operator()(uint64_t k) const { return std::hash<uint64_t>()(k); } };
std::unordered_map<uint64_t, std::array<float, 16>, KeyHash>* mats_cur = nullptr;
std::unordered_map<uint64_t, std::array<float, 16>, KeyHash>* mats_prev = nullptr;
// Instances par modele : un meme modele (rambarde, fleche...) est dessine plusieurs fois par image et son rang
// change des qu'une instance sort de l'ecran. On apparie donc chaque instance a celle de l'image precedente
// la plus proche en position MONDE (matrice courante ramenee par la matrice de vue = bas de la pile Ninja).
struct Inst { std::array<float, 16> m; float w[3]; bool used; std::array<float, 16> v; bool has_v; };   // v = vue au dessin
using InstMap = std::unordered_map<void*, std::vector<Inst>>;
InstMap* inst_cur = nullptr; InstMap* inst_prev = nullptr;
float** kViewMatrix = nullptr;     // bas de la pile de matrices (vue), 0x00DD5C48
// Flou de mouvement limite a la camera : vue de l'image en cours (derniere vue vue au dessin d'un modele) et de
// l'image precedente. Copie de flou = W * Vprec = M * V^-1 * Vprec (l'objet reste a sa place, seule la camera recule).
constexpr float kStaticEps = 0.02f;
constexpr float kMatchDist = 30.f;    // ecart monde maximal entre deux images pour relier deux exemplaires d'un objet   // deplacement dans le monde au-dela duquel un objet est "mobile"
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
unsigned frame_models = 0;   // modeles dessines dans l'image du jeu en cours
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
alignas(16) static uint8_t g_work_sub[2 * kMaxSub][0x10000 * 16];   // tampons de travail des copies
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

// Objet immobile dans le monde : on n'interpole que la camera (position monde en ligne droite, rotation spherique)
// et l'objet garde sa matrice monde W = M * V^-1. Sinon chaque objet tourne autour de sa propre origine et deux
// objets fixes voisins (bloc de trottoir a l'origine lointaine, jardiniere) s'ecartent de quelques pixels dans
// l'image intermediaire : le decor tremble. Renvoie false si l'objet a bouge (ou tourne) : methode par objet.
extern unsigned long frame_no;
unsigned long g_title_start = 0;   // copie de title_start (debut de la sequence 3D)
std::vector<std::vector<uint8_t>> shadow_cur, shadow_prev; bool shadow_interp_on = true;
// Pieces du volume d'ombre de l'image en cours : interpolees toutes ensemble ou pas du tout (une piece interpolee
// a cote d'une piece restee a sa place ouvre une fissure dans le volume : long trait noir oblique).
constexpr float kShTol = 0.2f;   // ecart relatif admis (avant projection) entre deux images pour un sommet d'ombre
struct ShPiece { std::vector<uint8_t> prev, real; std::vector<uint8_t*> dst; std::vector<float> t; bool ok; };
std::vector<ShPiece> sh_group;
struct ShWeld { float r[4], o[4], t; }; std::vector<ShWeld> sh_weld;   // points d'ombre deja places dans l'image
void sh_flush() {
    if (sh_group.empty()) return;
    bool all = true; for (const ShPiece& p : sh_group) if (!p.ok) { all = false; break; }
    if (all)
        for (const ShPiece& p : sh_group)
            for (size_t k = 0; k < p.dst.size(); k++) {
                const float t = p.t[k];
                for (size_t o = 0; o + 32 <= p.real.size(); o += 32) {
                    const float* a = reinterpret_cast<const float*>(p.prev.data() + o); const float* b = reinterpret_cast<const float*>(p.real.data() + o);
                    float* d = reinterpret_cast<float*>(p.dst[k] + o);
                    const float wa = 1.f / a[3], wb = 1.f / b[3], w = wa + (wb - wa) * t;
                    if (std::fabs(w) < 1e-12f) continue;
                    for (int q = 0; q < 3; q++) d[q] = (a[q] * wa + (b[q] * wb - a[q] * wa) * t) / w;
                    d[3] = 1.f / w;
                }
                // un meme point repete dans plusieurs triangles doit rester un seul point (sinon le volume se fend)
                // (aussi entre pieces de la meme image : table des points deja places pour cette date t)
                const size_t nv = p.real.size() / 32;
                for (size_t v = 0; v < nv; v++) {
                    const float* rp = reinterpret_cast<const float*>(p.real.data() + v * 32); float* d = reinterpret_cast<float*>(p.dst[k] + v * 32);
                    bool found = false;
                    for (const ShWeld& w : sh_weld) if (w.t == t && !memcmp(w.r, rp, 16)) { memcpy(d, w.o, 16); found = true; break; }
                    if (!found) { ShWeld w; memcpy(w.r, rp, 16); memcpy(w.o, d, 16); w.t = t; sh_weld.push_back(w); }
                }
            }
    sh_group.clear();
}
IDirect3DDevice9* dev = nullptr;
unsigned stencil_draws = 0, stencil_volumes = 0;
std::vector<Range> suivi_ranges;   // test : sommets du modele suivi (JSR60_SUIVI)
bool g_ff = false;   // avance rapide en cours
bool frame_cut = false;   // image du jeu qui commence un nouveau plan de camera
constexpr float kCutDist = 12.f, kCutAngle = 30.f;
void f9_on_cut();   // avance rapide en cours
bool static_interp(const Inst& a, const Inst& b, float t, float* out) {
    if (!a.has_v || !b.has_v) return false;
    float ia[16], ib[16], Wa[16], Wb[16], Vt[16];
    if (!affine_inverse(a.v.data(), ia) || !affine_inverse(b.v.data(), ib)) return false;
    mat_mul(a.m.data(), ia, Wa); mat_mul(b.m.data(), ib, Wb);
    float sc = 0; for (int i = 0; i < 12; i++) sc = std::max(sc, std::fabs(Wb[i]));
    for (int i = 0; i < 12; i++) if (std::fabs(Wa[i] - Wb[i]) > 2e-3f * sc + 1e-6f) return false;     // a tourne
    for (int i = 12; i < 15; i++) if (std::fabs(Wa[i] - Wb[i]) > kStaticEps) return false;          // s'est deplace
    if (!interp_matrix(a.v.data(), b.v.data(), t, Vt)) return false;                               // coupure de plan
    float c[3]; for (int k = 0; k < 3; k++) c[k] = ia[12 + k] + (ib[12 + k] - ia[12 + k]) * t;   // camera (monde)
    for (int k = 0; k < 3; k++) Vt[12 + k] = -(c[0] * Vt[k] + c[1] * Vt[4 + k] + c[2] * Vt[8 + k]);
    mat_mul(Wb, Vt, out); return true;
}
bool cam_interp_on = true;   // F6 (diagnostic) : decor interpole par la camera
unsigned st_cam = 0;
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
// Objet dessine par le code des menus du jeu (0x0045B000..0x0045BA00 dans la pile d'appels) : panneau et textes du menu
// Start, logo et options du menu principal. Places juste devant la camera : sans ca ils suivent son mouvement et sont floutes.
bool menu_model() {
    void* fr[10] = {}; const USHORT nf = CaptureStackBackTrace(2, 10, fr, nullptr);
    for (USHORT i = 0; i < nf; i++) { const uintptr_t a = uintptr_t(fr[i]) - exe_delta; if (a >= 0x0045B000 && a < 0x0045BA00) return true; }
    return false;
}
int model_common(int which, void* model) {
    if (which != 1 && !in_model) sh_flush();
    const ModelFn orig = model_orig[which];
    if (enabled && tag_fixes && drawing_tag && !in_model && *kCurMatrix) {   // tag : rapproche (les deux copies partent de la)
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
    if (!in_model) tick_present();
    if ((!enabled || g_ff) && !in_model) frame_models++;   // repere du debut de cinematique, meme mod coupe / avance rapide
    if (g_ff && !in_model) return orig(model);   // avance rapide : aucun calcul d'interpolation
    if (!enabled || in_model || !*kVertPtr || !*kCurMatrix || !inst_cur) return orig(model);
    if (menu_model()) {                     // menus (Start, menu principal) : objets colles a la camera, ni interpoles ni floutes
        st.hud++; return orig(model);
    }
    if (hud_view()) {                       // interface : dessinee une fois, commune aux deux images
        st.hud++; if (model_log) fprintf(model_log, "%p f%d interface (pas d'interpolation)\n", model, which);
        return orig(model);
    }
    in_model = true;
    if (kViewMatrix && *kViewMatrix) { memcpy(view_cur, *kViewMatrix, 64); have_view_cur = true; }
    float* m = *kCurMatrix; std::array<float, 16> cur; memcpy(cur.data(), m, 64);
    Inst inst; inst.m = cur; inst.used = false; world_pos(m, inst.w);
    {   // camera principale de l'image (vue du premier modele du monde) : certains objets (pancartes, velos) sont
        // dessines avec une autre matrice en bas de pile ; M = W * Vcamera reste vrai pour tout objet du monde.
        static unsigned long fv_frame = ~0ul; static float fv[16]; static bool fv_ok = false;
        if (fv_frame != frame_no) {
            fv_frame = frame_no; const bool had = fv_ok; float pv[16]; memcpy(pv, fv, 64);
            fv_ok = kViewMatrix && *kViewMatrix && *kViewMatrix != m; if (fv_ok) memcpy(fv, *kViewMatrix, 64);
            // changement de plan : la camera saute (position ou orientation) -> aucune interpolation pour cette image
            frame_cut = false;
            float ia[16], ib[16];
            if (had && fv_ok && affine_inverse(pv, ia) && affine_inverse(fv, ib)) {
                const float dx = ib[12] - ia[12], dy = ib[13] - ia[13], dz = ib[14] - ia[14], dp = std::sqrt(dx*dx + dy*dy + dz*dz);
                float dot = 0; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) dot += ia[r*4+c] * ib[r*4+c];   // trace(Ra^T Rb) (echelle ~1)
                float n2 = 0; for (int i = 0; i < 12; i++) if (i % 4 != 3) n2 += ib[i] * ib[i];
                const float cosang = std::max(-1.f, std::min(1.f, (dot * 3.f / std::max(n2, 1e-6f) - 1.f) / 2.f));
                const float ang = std::acos(cosang) * 57.29578f;
                frame_cut = dp > kCutDist || ang > kCutAngle;
                if (frame_cut) f9_on_cut();   // test : capture de chaque debut de plan
                if (frame_cut) log("camera image %lu : saut %.2f unites, %.1f degres%s\n", frame_no, dp, ang, frame_cut ? " -> changement de plan" : "");
            }
        }
        inst.has_v = fv_ok; if (fv_ok) memcpy(inst.v.data(), fv, 64);
    }
    (*inst_cur)[model].push_back(inst);
    end_strip(); const uint32_t r0 = *kVertCount;
    const int ret = orig(model);
    end_strip(); const uint32_t r1 = *kVertCount;
    float mid[16]; bool moved = false; const float* prevm = nullptr; bool obj_moving = false; const Inst* prev_inst = nullptr;
    int dbg_why = 0; float dbg_d = 0, dbg_t = 0; int dbg_n = 0;   // 1 aucune instance libre, 2 ecart trop grand
    auto interp_at = [&](float t, float* out) -> bool {
        if (!prevm) return false;
        if (cam_interp_on && prev_inst && static_interp(*prev_inst, inst, t, out)) { st_cam++; return true; }
        return interp_matrix(prevm, cur.data(), t, out);
    };
    auto it = inst_prev->find(model);
    if (it != inst_prev->end()) {
        Inst* best = nullptr; float bd = 1e30f;
        for (Inst& p : it->second) {
            if (p.used) continue;
            const float dx = p.w[0] - inst.w[0], dy = p.w[1] - inst.w[1], dz = p.w[2] - inst.w[2], dd = dx*dx + dy*dy + dz*dz;
            if (dd < bd) { bd = dd; best = &p; }
        }
        // Un exemplaire qui vient d'apparaitre ne doit pas prendre la place d'un autre exemplaire du meme objet, loin
        // de lui (sinon le vrai partenaire n'en trouve plus et toute une zone du trottoir n'est pas interpolee).
        if (best && bd > kMatchDist * kMatchDist) { dbg_d = std::sqrt(bd); best = nullptr; dbg_why = 3; }
        dbg_n = int(it->second.size()); if (!best && !dbg_why) dbg_why = 1;
        if (best) {
            dbg_d = std::sqrt(bd);
            dbg_why = 0;
            best->used = true;
            const float dx = best->w[0] - inst.w[0], dy = best->w[1] - inst.w[1], dz = best->w[2] - inst.w[2];
            obj_moving = dx*dx + dy*dy + dz*dz > kStaticEps * kStaticEps;
            if (!frame_cut && !interp_matrix(best->m.data(), cur.data(), 0.5f, mid)) {
                dbg_why = 2; const float* a = best->m.data(); const float* b = cur.data();
                dbg_t = std::sqrt((a[12]-b[12])*(a[12]-b[12]) + (a[13]-b[13])*(a[13]-b[13]) + (a[14]-b[14])*(a[14]-b[14]));
            }
            if (!frame_cut && interp_matrix(best->m.data(), cur.data(), 0.5f, mid)) { moved = true; prevm = best->m.data(); prev_inst = best; interp_at(0.5f, mid); } else st.jump++;
        }
    }
    if (!moved) { memcpy(mid, cur.data(), 64); st.same++; } else st.interp++;
    if (model_log) {
        fprintf(model_log, "%p f%d monde(%.2f %.2f %.2f) cam(%.2f %.2f %.2f) %s pile", model, which, inst.w[0], inst.w[1], inst.w[2],
            cur[12], cur[13], cur[14], moved ? "mi-chemin" : (it == inst_prev->end() ? "nouveau" : "saut/sans"));
        void* fr[12] = {}; const USHORT nf = CaptureStackBackTrace(1, 12, fr, nullptr);
        for (USHORT i = 0; i < nf; i++) { const uintptr_t ad = uintptr_t(fr[i]); if (ad >= 0x400000 + exe_delta && ad < 0x600000 + exe_delta) fprintf(model_log, " %06X", unsigned(ad - exe_delta)); }
        fprintf(model_log, "\n");
    }
    // ---- copies : une par image affichee (t < 1), plus une copie de flou par image si nsub <= 2
    if (!logged_work) { logged_work = true; log("tampon de travail des sommets : %p\n", *kWorkBuf); }
    struct Copy { uint32_t a, b, mask; float t; };
    Copy cp[2 * kMaxSub]; int ncp = 0;
    uint32_t live_mask = SUB_LIVE;
    auto draw_copy = [&](const float* mat, uint8_t* work, uint32_t mask, float t) {
        memcpy(m, mat, 64);
        Copy c; c.mask = mask; c.t = t; c.a = *kVertCount;
        void* const wr = *kWorkBuf; *kWorkBuf = work; orig(model); *kWorkBuf = wr;
        end_strip(); c.b = *kVertCount; cp[ncp++] = c;
        if (model_log) { char w[32]; sprintf(w, "copie t=%.2f", t); log_bbox(w, c.a, c.b); }
    };
    float tm[16];
    for (int j = 0; j < nsub; j++) {
        if (sub_t[j] >= 0.999f) { live_mask |= 1u << j; continue; }        // image a t = 1 : la copie reelle
        if (!interp_at(sub_t[j], tm)) memcpy(tm, cur.data(), 64);
        draw_copy(tm, g_work_sub[j], 1u << j, sub_t[j]);
    }
    if (sub_blur && blur_level > 0)
        for (int j = 0; j < nsub; j++) {
            // flou limite a la camera : objet mobile = meme position que son image (net) ; objet fixe = un peu avant
            const float tb = sub_t[j] - blur_s() * (2.f / nsub);
            if (obj_moving || !interp_at(tb, tm)) {
                if (!interp_at(sub_t[j], tm)) memcpy(tm, cur.data(), 64);
            }
            draw_copy(tm, g_work_sub[kMaxSub + j], 1u << (8 + j), tb);
        }
    memcpy(m, cur.data(), 64);
    // Controle : certains modeles changent d'etat a chaque appel (logo du titre) et leurs copies suivantes partent
    // loin hors ecran. Une copie doit rester dans la zone balayee reel <-> 1re copie (agrandie) ; sinon elle est
    // ecartee et l'image prend la copie valable la plus proche. Une copie vide (rien redessine) est traitee pareil.
    float R[4], C[4], B[4]; bool boxes = false; float dx = 0, dy = 0;
    // Objet immense (une partie passe derriere la camera : rectangle aberrant, ex. grande ombre du decor) : pas de controle.
    if (ncp > 0 && screen_box(r0, r1, R) && screen_box(cp[0].a, cp[0].b, C) && R[1] - R[0] < 3000.f && R[3] - R[2] < 3000.f) {
        boxes = true;
        dx = std::max(std::fabs(R[0] - C[0]), std::fabs(R[1] - C[1])) * 2.f + 48.f;
        dy = std::max(std::fabs(R[2] - C[2]), std::fabs(R[3] - C[3])) * 2.f + 48.f;
    }
    for (int k = 0; k < ncp; k++) {
        Copy& c = cp[k];
        bool ok = c.b > c.a;
        if (ok && k > 0 && boxes) {
            ok = screen_box(c.a, c.b, B) && B[0] >= std::min(R[0], C[0]) - dx && B[1] <= std::max(R[1], C[1]) + dx &&
                 B[2] >= std::min(R[2], C[2]) - dy && B[3] <= std::max(R[3], C[3]) + dy;
            if (!ok) st.blur_rejected++;
        }
        if (!ok && c.b > c.a) c.mask |= SUB_DEAD;   // ecartee : jamais dessinee
    }
    // reattribution des images orphelines (copie vide ou ecartee) a la copie reelle
    for (int k = 0; k < ncp; k++) {
        Copy& c = cp[k];
        const bool ok = c.b > c.a && !(c.mask & SUB_DEAD);
        if (ok) continue;
        uint32_t bits = 0;
        for (int j = 0; j < nsub; j++) {   // quelles images cette copie servait-elle ? (d'apres sa date t)
            if (std::fabs(sub_t[j] - c.t) < 1e-4f) bits |= 1u << j;
            if (sub_blur && std::fabs(sub_t[j] - blur_s() * (2.f / nsub) - c.t) < 1e-4f) bits |= 1u << (8 + j);
        }
        live_mask |= bits;
        if (c.b <= c.a) st.empty++;
    }
    log_bbox("reel", r0, r1);
    if (model_log && (getenv("JSR60_SUIVI") && uintptr_t(model) == uintptr_t(strtoul(getenv("JSR60_SUIVI"), nullptr, 16)))) {
        IDirect3DDevice9* d = dev; DWORD v[10] = {};
        const D3DRENDERSTATETYPE rs[10] = {D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILPASS, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_TEXTUREFACTOR, D3DRS_ZFUNC};
        if (d) for (int k = 0; k < 10; k++) d->GetRenderState(rs[k], &v[k]);
        suivi_ranges.push_back({r0, r1});
        fprintf(model_log, "ETATS panneau : stencil %u func %u ref %u mask %X pass %u | blend %u src %u dst %u | tfactor %08X zfunc %u ; pile", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        void* fr[8] = {}; const USHORT nf = CaptureStackBackTrace(1, 8, fr, nullptr);
        for (USHORT i = 0; i < nf; i++) { const uintptr_t ad = uintptr_t(fr[i]); if (ad >= 0x400000 + exe_delta && ad < 0x600000 + exe_delta) fprintf(model_log, " %06X", unsigned(ad - exe_delta)); }
        fprintf(model_log, "\n");
    }
    {   // test (JSR60_SUIVI=adresse du modele) : couleurs des sommets reels
        static uintptr_t suivi = ~uintptr_t(0); if (suivi == ~uintptr_t(0)) { const char* e = getenv("JSR60_SUIVI"); suivi = e ? uintptr_t(strtoul(e, nullptr, 16)) : 0; }
        if (model_log && suivi && uintptr_t(model) == suivi && *kVertPtr) {
            fprintf(model_log, "SUIVI %p sommets %u :", model, r1 - r0);
            for (uint32_t i = r0; i < r1 && i < r0 + 4; i++) { const uint32_t* v = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(*kVertPtr) + size_t(i) * 32); fprintf(model_log, " [%08X %08X %08X %08X %08X %08X %08X %08X]", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]); }
            fprintf(model_log, "\n");
        }
    }
    {   // diagnostic (JSR60_DIAG) : objets visibles non interpoles entre 36 et 44 s de la sequence
        static int diag = -1; if (diag < 0) diag = getenv("JSR60_DIAG") ? 1 : 0;

        if (diag && g_title_start && frame_no >= g_title_start + 1080 && frame_no <= g_title_start + 1320) {
            float RB[4];
            if (screen_box(r0, r1, RB) && RB[1] > 0 && RB[0] < 1920 && RB[3] > 0 && RB[2] < 1080) {
                const char* why = moved ? nullptr : (frame_cut ? nullptr : (it == inst_prev->end() ? "nouveau" : (dbg_why == 1 ? "aucune-instance-libre" : (dbg_why == 3 ? "trop-loin-nouveau" : "ecart-trop-grand"))));
                bool dead = false; for (int k = 0; k < ncp; k++) if (cp[k].mask & SUB_DEAD) dead = true;
                if (dead) why = "copie-ecartee";
                if (why) log("DIAG image %lu (%.2f s) : %p f%d %s (prec %d inst, dist monde %.2f, ecart cam %.1f) ecran x %.0f..%.0f y %.0f..%.0f monde %.1f %.1f %.1f\n", frame_no, (frame_no - g_title_start) / 30.0, model, which, why, dbg_n, dbg_d, dbg_t, RB[0], RB[1], RB[2], RB[3], inst.w[0], inst.w[1], inst.w[2]);
            }
        }
    }
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
    // Volume d'ombre du personnage (fonction f1) : pieces temporaires dont le jeu recalcule les sommets a chaque image
    // autour de la camera ; interpoler la matrice ne les deplace pas. On place les sommets ecran de chaque copie entre
    // ceux de la meme piece (meme rang dans l'image, meme nombre de sommets) a l'image precedente et a l'image actuelle.
    if (which == 1 && r1 > r0 && *kVertPtr) {   // (pieces toujours notees ; appliquees si shadow_interp_on)
        const size_t bytes = size_t(r1 - r0) * 32;
        const uint8_t* real = static_cast<const uint8_t*>(*kVertPtr) + size_t(r0) * 32;
        const size_t idx = shadow_cur.size(); shadow_cur.emplace_back(real, real + bytes);
        ShPiece pc; pc.real.assign(real, real + bytes); pc.ok = false;
        if (!frame_cut && idx < shadow_prev.size() && shadow_prev[idx].size() == bytes) {
            pc.prev = shadow_prev[idx];
            // meme piece ? tous les sommets, y compris ceux projetes tres loin : comparaison avant projection
            pc.ok = true; float emax = 0; unsigned nbad = 0;
            for (uint32_t v = 0; v < r1 - r0; v++) {
                const float* a = reinterpret_cast<const float*>(pc.prev.data() + size_t(v) * 32);
                const float* b = reinterpret_cast<const float*>(real + size_t(v) * 32);
                if (std::fabs(a[3]) < 1e-12f || std::fabs(b[3]) < 1e-12f) { pc.ok = false; break; }
                const float wa = 1.f / a[3], wb = 1.f / b[3];
                const float dx = a[0] * wa - b[0] * wb, dy = a[1] * wa - b[1] * wb, dw = wa - wb;
                const float mb = std::sqrt(b[0] * wb * b[0] * wb + b[1] * wb * b[1] * wb + wb * wb);
                const float e = std::sqrt(dx * dx + dy * dy + dw * dw) / std::max(mb, 1e-6f);
                emax = std::max(emax, e); if (e > kShTol) { pc.ok = false; nbad++; }
            }
            if (!pc.ok) {   // piece reconstruite dans un autre ordre : chaque sommet cherche son plus proche a l'image precedente
                const uint32_t nv = r1 - r0; std::vector<uint8_t> re(pc.prev.size()); bool all = true;
                auto clip = [](const uint8_t* p, float* o) { const float* f = reinterpret_cast<const float*>(p); const float w = 1.f / f[3]; o[0] = f[0] * w; o[1] = f[1] * w; o[2] = w; };
                for (uint32_t v = 0; v < nv && all; v++) {
                    float B[3]; clip(real + size_t(v) * 32, B); const float mb = std::sqrt(B[0]*B[0] + B[1]*B[1] + B[2]*B[2]);
                    float best = 1e30f; uint32_t bi = 0;
                    for (uint32_t u = 0; u < nv; u++) {
                        const float* fa = reinterpret_cast<const float*>(pc.prev.data() + size_t(u) * 32); if (std::fabs(fa[3]) < 1e-12f) continue;
                        float A[3]; clip(pc.prev.data() + size_t(u) * 32, A);
                        const float e = (A[0]-B[0])*(A[0]-B[0]) + (A[1]-B[1])*(A[1]-B[1]) + (A[2]-B[2])*(A[2]-B[2]);
                        if (e < best) { best = e; bi = u; }
                    }
                    if (std::sqrt(best) > kShTol * mb) all = false; else memcpy(re.data() + size_t(v) * 32, pc.prev.data() + size_t(bi) * 32, 32);
                }
                if (all) { pc.prev.swap(re); pc.ok = true; }
            }
            if (getenv("JSR60_DIAG") && g_title_start && frame_no >= g_title_start + 1050 && frame_no <= g_title_start + 1080)
                log("OMBRE image %lu piece %u : %u sommets, erreur max %.3f, %u fautifs%s\n", frame_no, unsigned(idx), unsigned(r1 - r0), emax, nbad, pc.ok ? "" : " -> REJET");
        }
        for (int k = 0; k < ncp; k++) {
            const Copy& c = cp[k];
            if (c.b - c.a != r1 - r0 || (c.mask & SUB_DEAD)) continue;
            // l'ombre suit le personnage (objet mobile) : sa copie de flou garde la date de son image (nette, pas
            // dedoublee ni eclaircie), comme le personnage lui-meme
            float t = c.t;
            for (int j = 0; j < nsub; j++) if (c.mask & (1u << (8 + j))) t = sub_t[j];
            if (t >= 0.999f) {   // copie a la date de l'image reelle : sommets reels
                memcpy(static_cast<uint8_t*>(*kVertPtr) + size_t(c.a) * 32, real, bytes); continue;
            }
            pc.dst.push_back(static_cast<uint8_t*>(*kVertPtr) + size_t(c.a) * 32); pc.t.push_back(t);
        }
        sh_group.push_back(std::move(pc));
        if (!sh_group.back().ok || !shadow_interp_on) sh_group.back().dst.clear();   // sans correspondance (ou coupe) : position actuelle
        sh_group.back().ok = true; sh_flush();                  // applique tout de suite (le jeu transmet les sommets aussitot)
    }
    // plages (dans l'ordre croissant des sommets : copie reelle puis copies)
    if (r1 > r0) mranges.push_back({r0, r1, live_mask});
    for (int k = 0; k < ncp; k++) if (cp[k].b > cp[k].a) mranges.push_back({cp[k].a, cp[k].b, (cp[k].mask & SUB_DEAD) ? SUB_DEAD : cp[k].mask});
    st.models++; frame_models++; in_model = false;
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
    if (!enabled || !my_sort) return 0;     // F8 / F6 : comportement d'origine exact
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
    if (enabled && my_sort && last_vb_data && sz == 0x24) {
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
    if (enabled && tag_fixes && last_vb_data && sz == 0x24) order_tag_layers(static_cast<uint8_t*>(base), n);
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
// ---- Deuxieme type de decor (quartier au soleil couchant) : 0x004478B0 prend la zone de la camera (0x0040247D), puis
// dessine chaque zone de sa liste (table [0xDFD0A0], comme la ville) si elle est chargee (0x00405097(0x00405B0A(id)) == 2),
// avec 4 fonctions qui prennent la zone dans eax. Les zones hors liste surgissent quand la camera change de zone.
// On dessine en plus les autres zones chargees, exactement comme le fait la variante du jeu 0x00447970.
using VoidFn = void (__cdecl*)();
VoidFn zone_draw_orig = nullptr; IntFn zone_area = nullptr, zone_ptr = nullptr, zone_state = nullptr; IntFn zone_flag = nullptr;
uintptr_t zone_fn[4] = {};
unsigned zone_extra_drawn = 0;
void zone_call(uintptr_t fn, int id) {
    __asm {
        mov eax, id
        call fn
    }
}
void __cdecl zone_draw_hook() {
    zone_draw_orig();
    if (!enabled || !city_extend) return;
    const int area = zone_area(0);
    const uint8_t* tbl = *at<uint8_t**>(0x00DFD0A0);
    if (area < 0 || !tbl) return;
    bool listed[0x72] = {};
    for (const int* l = *reinterpret_cast<int* const*>(tbl + area * 8 + 4); l && *l >= 0; l++) if (*l < 0x72) listed[*l] = true;
    int added[0x72]; int na = 0;
    for (int id = 0; id < 0x72; id++) {
        if (listed[id] || zone_state(zone_ptr(id)) != 2) continue;
        zone_flag(0x400); zone_call(zone_fn[0], id); zone_call(zone_fn[1], id); zone_call(zone_fn[2], id);
        zone_flag(0x2000); zone_call(zone_fn[3], id);
        added[na++] = id; zone_extra_drawn++;
    }
    if (model_log) { fprintf(model_log, "QUARTIER zone %d ; ajoutees :", area); for (int i = 0; i < na; i++) fprintf(model_log, " %d", added[i]); fprintf(model_log, "\n"); }
}
// ---- Eclairage des objets de decor poses a part (panneaux, enseignes : 0x00441390 -> appel 0x00441405 -> 0x004066F9).
// Le jeu eclaire tout objet "en direct" avec une lumiere accrochee a la camera ([0x0098F5FC], rangee via 0x00402F18 /
// 0x00405218 et utilisee dans le repere camera par 0x0059BAC0) : voulu pour les personnages, absurde pour un panneau
// fixe (clair face a la camera, sombre sinon, alors que la facade peinte derriere ne change jamais ; flash a chaque
// changement de plan, la lumiere etant encore celle de l'ancienne camera). Pour ces objets : lumiere fixe dans le decor
// (venant d'en haut), convertie dans le repere de la camera de l'image ; la lumiere du jeu est remise ensuite.
using LightFn = void (__cdecl*)(float, float, float);
LightFn light1_orig = nullptr, light2_orig = nullptr; bool decor_light_fix = true;
float light_w[2][3]; bool light_have[2] = {};
void __cdecl light1_hook(float x, float y, float z) { light_w[0][0] = x; light_w[0][1] = y; light_w[0][2] = z; light_have[0] = true; light1_orig(x, y, z); }
void __cdecl light2_hook(float x, float y, float z) { light_w[1][0] = x; light_w[1][1] = y; light_w[1][2] = z; light_have[1] = true; light2_orig(x, y, z); }
using DrawModelFn = int (__cdecl*)(void*, float);
DrawModelFn decor_draw_orig = nullptr; unsigned decor_drawn = 0;
// Objet fixe ou mobile : on garde, par modele, ses positions dans le decor a l'image precedente. Un exemplaire qui etait
// tout pres a l'image d'avant sans etre exactement au meme endroit est en mouvement (voiture, passant) : eclairage du jeu.
// Sinon (meme position, ou objet qui vient d'apparaitre, ex. premiere image d'un plan) : objet fixe.
struct DecorPos { float w[3]; unsigned long frame; };
std::unordered_map<void*, std::vector<DecorPos>> decor_pos;
bool decor_is_static(void* model) {
    if (!kCurMatrix || !*kCurMatrix) return false;
    float w[3]; world_pos(*kCurMatrix, w);
    auto& vec = decor_pos[model];
    bool moving = false;
    for (const DecorPos& p : vec) {
        if (p.frame + 1 != frame_no) continue;
        const float dx = p.w[0] - w[0], dy = p.w[1] - w[1], dz = p.w[2] - w[2], d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < 0.05f * 0.05f) { moving = false; break; }   // meme place : fixe
        if (d2 < 10.f * 10.f) moving = true;                  // juste a cote : il bouge
    }
    vec.erase(std::remove_if(vec.begin(), vec.end(), [](const DecorPos& p) { return p.frame + 1 < frame_no; }), vec.end());
    if (vec.size() < 256) vec.push_back({{w[0], w[1], w[2]}, frame_no});
    return !moving;
}
int __cdecl decor_draw_hook(void* model, float f) {
    const float* v = (decor_light_fix && enabled && kViewMatrix) ? *kViewMatrix : nullptr;
    if (!v || !light_have[0] || !decor_is_static(model)) return decor_draw_orig(model, f);
    const float wx = 0.f, wy = 1.f, wz = 0.f;   // lumiere du decor : verticale
    const float x = wx * v[0] + wy * v[4] + wz * v[8], y = wx * v[1] + wy * v[5] + wz * v[9], z = wx * v[2] + wy * v[6] + wz * v[10];
    light1_orig(x, y, z); if (light_have[1]) light2_orig(x, y, z);
    const int r = decor_draw_orig(model, f);
    light1_orig(light_w[0][0], light_w[0][1], light_w[0][2]); if (light_have[1]) light2_orig(light_w[1][0], light_w[1][1], light_w[1][2]);
    decor_drawn++;
    return r;
}
// ---- Demarrage : la tache 0x00457CC0 (etapes dans [esi+0x6C]) passe la video SEGA (Heritage), puis affiche le logo
// « blit » (etapes 2 a 6 : chargement de blit_big.png, fondu, 2 s d'affichage, fondu). A la fin de la video (etape 1,
// apres l'arret de la video), on passe directement a l'etape 7 (qui gere l'absence du logo) : le logo n'est jamais charge.
bool skip_blit_logo() {
    // Etape 1 d'origine : arret de la video, creation du voile de fondu ([0x9725B4]), puis chargement de blit_big.png et
    // etape 2. On garde le voile (il couvre l'ecran pendant la transition : sans lui, l'avertissement apparait une image en
    // pleine lumiere) mais on ne charge pas le logo ([0x9725B8] = 0) et on passe a l'etape 5 (fondu au noir) -> 6 -> 7.
    uint8_t* c = at<uint8_t*>(0x00457D70);
    const uint8_t expect[4] = {0xFF, 0x46, 0x6C, 0x68};
    if (memcmp(c, expect, 4)) { log("logo blit : code inattendu, non modifie\n"); return false; }
    uint8_t code[21]; int k = 0;
    code[k++] = 0xC7; code[k++] = 0x46; code[k++] = 0x6C; code[k++] = 5; code[k++] = 0; code[k++] = 0; code[k++] = 0;   // etape = 5
    code[k++] = 0x83; code[k++] = 0xC4; code[k++] = 0x0C;                                                       // add esp, 0x0C
    const uint32_t blit_var = uint32_t(0x009725B8 + exe_delta);
    code[k++] = 0xC7; code[k++] = 0x05; memcpy(code + k, &blit_var, 4); k += 4; memset(code + k, 0, 4); k += 4;  // [0x9725B8] = 0
    code[k++] = 0x90;
    DWORD old; VirtualProtect(c, k, PAGE_EXECUTE_READWRITE, &old); memcpy(c, code, k); VirtualProtect(c, k, old, &old);
    FlushInstructionCache(GetCurrentProcess(), c, k);
    return true;
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
    log("dessin de la ville (2e appel) intercepte : %s\n", patch_call(0x0043D19E, 0x00401E6A, (void*)city_draw_hook) ? "oui" : "non");
    zone_draw_orig = at<VoidFn>(0x004478B0); zone_area = at<IntFn>(0x0040247D); zone_ptr = at<IntFn>(0x00405B0A); zone_state = at<IntFn>(0x00405097); zone_flag = at<IntFn>(0x004011FE);
    zone_fn[0] = 0x00446DB0 + exe_delta; zone_fn[1] = 0x00446ED0 + exe_delta; zone_fn[2] = 0x00447140 + exe_delta; zone_fn[3] = 0x00447240 + exe_delta;
    log("logo blit au demarrage supprime : %s\n", skip_blit_logo() ? "oui" : "non");
    light1_orig = at<LightFn>(0x00599580); light2_orig = at<LightFn>(0x005A04E0); decor_draw_orig = at<DrawModelFn>(0x004066F9);
    log("lumiere fixe pour les objets de decor : %s / %s / %s\n", patch_relay(0x00402F18, 0x00599580, (void*)light1_hook) ? "oui" : "non",
        patch_relay(0x00405218, 0x005A04E0, (void*)light2_hook) ? "oui" : "non", patch_call(0x00441405, 0x004066F9, (void*)decor_draw_hook) ? "oui" : "non");
    log("dessin du quartier intercepte : %s\n", patch_relay(0x0040350D, 0x004478B0, (void*)zone_draw_hook) ? "oui" : "non");
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
IDirect3DVertexBuffer9* cur_vb = nullptr; UINT cur_stride = 0, cur_off = 0;
// Ombre du personnage (et petits elements sans texture) : sommets deja en coordonnees d'ecran, ecrits par le jeu
// hors du dessin des modeles, donc identiques dans toutes les images. On garde leurs sommets d'une image du jeu a
// l'autre (meme rang, meme taille) et l'image intermediaire les place a mi-chemin.
const uint8_t* vb_base[3] = {};
std::vector<std::vector<uint8_t>> scr_cur, scr_prev;
bool scr_interp_on = false;   // essai sans effet demontre : coupe
IDirect3DVertexBuffer9* game_vbs[3] = {}; int game_vb_count = 0;
IDirect3DStateBlock9* sb_begin = nullptr; IDirect3DStateBlock9* sb_end = nullptr;
// ---- Presentation pendant le calcul de l'image suivante : l'image du jeu terminee (« shown ») est montree en plusieurs
// images, une par rafraichissement ; celles qui tombent pendant que le jeu calcule l'image suivante sont presentees
// depuis le dessin des modeles (appele des centaines de fois par image) : on ferme la scene du jeu, on presente,
// on rouvre la scene et on rejoue ce que le jeu avait deja fait dans cette image.
Frame shown; IDirect3DStateBlock9* sb_shown = nullptr; IDirect3DStateBlock9* sb_save = nullptr;
std::vector<IUnknown*> held_shown;
int show_n = 0, show_next = 0; int64_t show_when[8] = {}; bool show_blur = false; bool in_scene = false;
// (dev : declare plus haut)
uint32_t blob(const void* p, size_t n) { const uint32_t at = (uint32_t)frame.blobs.size(); frame.blobs.insert(frame.blobs.end(), (const uint8_t*)p, (const uint8_t*)p + n); return at; }
void push(const Cmd& c) { if (recording && !in_replay) frame.cmds.push_back(c); }
// Ressource referencee par une commande enregistree (texture, tampon, shader...) : gardee en vie tant que l'image peut
// etre rejouee. Sinon le jeu peut la liberer (retour du menu de pause) et le rejeu la lit detruite : plantage.
void keep(IUnknown* u) { if (u && recording && !in_replay) { u->AddRef(); held.push_back(u); } }
unsigned prim_vertices(UINT t, UINT n) {
    switch (t) { case D3DPT_TRIANGLELIST: return n*3; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n+2;
        case D3DPT_LINELIST: return n*2; case D3DPT_LINESTRIP: return n+1; default: return n; }
}
bool is_game_vb(IDirect3DVertexBuffer9* vb) { for (int i = 0; i < game_vb_count; i++) if (game_vbs[i] == vb) return true; return false; }

HRESULT WINAPI h_begin(IDirect3DDevice9* d) {
    const HRESULT hr = o_begin(d);
    if (!in_replay) {
        in_scene = true; dev = d;
        frame.cmds.clear(); frame.blobs.clear(); frame.valid = false; frame.interp_draws = 0; frame.blur_draws = 0;
        for (IUnknown* u : held) u->Release(); held.clear();
        new_frame_backups();
        if (!sb_begin) d->CreateStateBlock(D3DSBT_ALL, &sb_begin); else sb_begin->Capture();
        recording = sb_begin != nullptr;
    }
    return hr;
}
HRESULT WINAPI h_end(IDirect3DDevice9* d) { if (!in_replay) in_scene = false; if (!in_replay && recording) { frame.valid = true; recording = false; } return o_end(d); }
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
// ---- Texture de l'ecran titre (logo, « APPUIE SUR START », menus ; 512x512) : le fond autour de APPUIE SUR START a ete
// mal efface dans la texture (traces semi-transparentes en haut a gauche et en bas a droite du texte). A la premiere
// utilisation de cette texture (reconnue par son contenu), on rend transparents, dans la bande du texte, les pixels a plus de
// 2 pixels de toute partie opaque des lettres.
std::set<IDirect3DBaseTexture9*> tex_checked; unsigned tex_cleaned = 0;
int alpha_of(const uint8_t* row, int x, D3DFORMAT f) {
    if (f == D3DFMT_A8R8G8B8) return row[x * 4 + 3];
    const uint16_t v = reinterpret_cast<const uint16_t*>(row)[x];
    if (f == D3DFMT_A4R4G4B4) return (v >> 12) * 17;
    return (v & 0x8000) ? 255 : 0;   // A1R5G5B5
}
void clear_alpha(uint8_t* row, int x, D3DFORMAT f) {
    if (f == D3DFMT_A8R8G8B8) { reinterpret_cast<uint32_t*>(row)[x] = 0; return; }
    reinterpret_cast<uint16_t*>(row)[x] = 0;
}
void fix_title_texture(IDirect3DBaseTexture9* bt) {
    if (!bt || !enabled || !tex_checked.insert(bt).second || bt->GetType() != D3DRTYPE_TEXTURE) return;
    IDirect3DTexture9* t = static_cast<IDirect3DTexture9*>(bt);
    D3DSURFACE_DESC desc; if (FAILED(t->GetLevelDesc(0, &desc))) return;
    if (desc.Width != 512 || desc.Height != 512 || desc.Pool != D3DPOOL_MANAGED) return;
    const D3DFORMAT f = desc.Format;
    if (f != D3DFMT_A8R8G8B8 && f != D3DFMT_A4R4G4B4 && f != D3DFMT_A1R5G5B5) return;
    D3DLOCKED_RECT lr; if (FAILED(t->LockRect(0, &lr, nullptr, 0))) return;
    uint8_t* bits = static_cast<uint8_t*>(lr.pBits);
    auto row = [&](int y) { return bits + size_t(y) * lr.Pitch; };
    unsigned total = 0, band = 0;
    for (int y = 0; y < 512; y++) for (int x = 0; x < 512; x++) if (alpha_of(row(y), x, f) >= 128) { total++; if (y >= 295 && y < 360) band++; }
    if (total == 74246 && band == 12429) {
        const int y0 = 290, y1 = 366; std::vector<uint8_t> strong(size_t(y1 - y0 + 4) * 512, 0);
        for (int y = y0 - 2; y < y1 + 2; y++) for (int x = 0; x < 512; x++) strong[size_t(y - y0 + 2) * 512 + x] = alpha_of(row(y), x, f) >= 128;
        unsigned n = 0;
        for (int y = y0; y < y1; y++) for (int x = 0; x < 512; x++) {
            const int a = alpha_of(row(y), x, f); if (!a || a >= 128) continue;
            bool nearl = false;
            for (int dy = -2; dy <= 2 && !nearl; dy++) for (int dx = -2; dx <= 2; dx++) { const int xx = x + dx; if (xx >= 0 && xx < 512 && strong[size_t(y + dy - y0 + 2) * 512 + xx]) { nearl = true; break; } }
            if (!nearl) { clear_alpha(row(y), x, f); n++; }
        }
        tex_cleaned++;
        log("texture de l'ecran titre : %u pixels de fond residuels effaces autour de APPUIE SUR START\n", n);
    }
    t->UnlockRect(0);
}
HRESULT WINAPI h_tex(IDirect3DDevice9* d, DWORD st_, IDirect3DBaseTexture9* t) {
    if (false && t && !in_replay) {   // diagnostic : niveaux de reduction des textures
        static std::set<IDirect3DBaseTexture9*> seen; static unsigned with = 0, without = 0, logged = 0;
        if (seen.size() < 4000 && seen.insert(t).second) {
            if (t->GetLevelCount() > 1) with++; else without++;
            if ((with + without) % 200 == 0 && logged < 20) { logged++; log("textures vues : %u avec niveaux de reduction, %u sans\n", with, without); }
        }
    } if (st_ == 0 && !in_replay) cur_tex0 = t; if (!in_replay) fix_title_texture(t); Cmd c{OP_TEX}; c.a = st_; c.p = t; push(c); keep(t); return o_tex(d, st_, t); }
HRESULT WINAPI h_decl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* v) { Cmd c{OP_DECL}; c.p = v; push(c); keep(v); return o_decl(d, v); }
HRESULT WINAPI h_fvf(IDirect3DDevice9* d, DWORD f) { Cmd c{OP_FVF}; c.a = f; push(c); return o_fvf(d, f); }
HRESULT WINAPI h_vs(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { Cmd c{OP_VS}; c.p = s; push(c); keep(s); return o_vs(d, s); }
HRESULT WINAPI h_ps(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { Cmd c{OP_PS}; c.p = s; push(c); keep(s); return o_ps(d, s); }
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
    if (n == 0) { cur_vb = vb; cur_stride = stride; cur_off = off; }
    Cmd c{OP_STREAM}; c.a = n; c.b = off; c.c = stride; c.p = vb; push(c); keep(vb); return o_stream(d, n, vb, off, stride);
}
void dump_tex(const void* data, UINT n, UINT stride, D3DPRIMITIVETYPE t);
HRESULT WINAPI h_draw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT n) {
    // Dessins du grand tampon du jeu : classes d'apres les plages notees pendant le dessin des modeles.
    const int kind = (!in_replay && is_game_vb(cur_vb)) ? classify(start) : 0;
    if (recording && !in_replay) { Cmd c{OP_DRAW}; c.a = t; c.b = start; c.c = n; c.d = (uint32_t)kind;
        if (kind == 0 && enabled && scr_interp_on && !cur_tex0 && cur_stride >= 16 && is_game_vb(cur_vb)) {
            const uint8_t* base = nullptr; for (int i = 0; i < game_vb_count; i++) if (game_vbs[i] == cur_vb) base = vb_base[i];
            const size_t bytes = size_t(prim_vertices(t, n)) * cur_stride;
            if (base && bytes && bytes < 65536) {
                const uint8_t* src = base + cur_off + size_t(start) * cur_stride;
                const size_t idx = scr_cur.size(); scr_cur.emplace_back(src, src + bytes);
                if (idx < scr_prev.size() && scr_prev[idx].size() == bytes) {
                    c.blob = blob(src, bytes); blob(scr_prev[idx].data(), bytes); c.blob_n = uint32_t(bytes); c.f = float(cur_stride);
                }
            }
        }
        push(c); if (kind & 0x3F) frame.interp_draws++; if (kind & 0x3F00) frame.blur_draws++; }
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
    if (model_log && !in_replay && is_game_vb(cur_vb)) { DWORD st = 0, cw = 0; d->GetRenderState(D3DRS_STENCILENABLE, &st); d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw); fprintf(model_log, "dessin %u+%u type %d classe %d tex %p%s\n", start, n, int(t), kind, (void*)cur_tex0, st ? (cw ? " STENCIL" : " STENCIL-sans-couleur") : ""); }
    if (model_log && !in_replay && !is_game_vb(cur_vb)) { DWORD st = 0; d->GetRenderState(D3DRS_STENCILENABLE, &st); fprintf(model_log, "dessin AUTRE tampon %p %u+%u type %d tex %p stencil %u\n", (void*)cur_vb, start, n, int(t), (void*)cur_tex0, unsigned(st)); }
    if (model_log && !in_replay && is_game_vb(cur_vb)) for (const Range& r : suivi_ranges) if (start >= r.a && start < r.b) {
        DWORD v[12] = {};
        const D3DRENDERSTATETYPE rs[12] = {D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILPASS, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_TEXTUREFACTOR, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE, D3DRS_ZWRITEENABLE};
        for (int k = 0; k < 12; k++) d->GetRenderState(rs[k], &v[k]);
        DWORD c0 = 0, a0 = 0; d->GetTextureStageState(0, D3DTSS_COLOROP, &c0); d->GetTextureStageState(0, D3DTSS_ALPHAOP, &a0);
        fprintf(model_log, "DESSIN SUIVI %u+%u classe %d : stencil %u func %u ref %u mask %X pass %u | blend %u src %u dst %u | tfactor %08X zfunc %u cw %X zw %u | colorop %u alphaop %u tex %p\n",
            start, n, kind, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], c0, a0, (void*)cur_tex0);
        break;
    }
    if (model_log && !in_replay) { DWORD st = 0, cw = 0; d->GetRenderState(D3DRS_STENCILENABLE, &st); d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw); if (st) { if (cw) stencil_draws++; else stencil_volumes++; } }
    if (!in_replay && kind == 0 && is_game_vb(cur_vb) && last_vb_data && cur_stride >= 16) {
        const uint8_t* base = nullptr; for (int i = 0; i < game_vb_count; i++) if (game_vbs[i] == cur_vb) base = vb_base[i];
        if (base) dump_tex(base + cur_off + size_t(start) * cur_stride, n, cur_stride, t);
    }
    if (kind && !(kind & SUB_LIVE) && !in_replay) return D3D_OK;      // copies : seulement dans les rejeux      // copies intermediaire / flou : pas dans l'image en direct
    return o_draw(d, t, start, n);
}
// Test (JSR60_DUMPTEX=secondes de cinematique) : a ce moment, chaque texture d'un dessin 2D est enregistree une fois
// (E:/Recomp/JSR_Mod/textures/<adresse>.png) avec son rectangle a l'ecran dans le journal.
using SaveTexFn = HRESULT (WINAPI*)(LPCSTR, DWORD, IDirect3DBaseTexture9*, const PALETTEENTRY*);
void dump_tex(const void* data, UINT n, UINT stride, D3DPRIMITIVETYPE t) {
    static long at_f = -1; if (at_f < 0) { const char* e = getenv("JSR60_DUMPTEX"); at_f = e ? long(atof(e) * 30) : 0; }
    if (!at_f || !g_title_start || !cur_tex0 || !data || frame_no < g_title_start + (unsigned long)at_f || frame_no > g_title_start + (unsigned long)at_f + 150) return;
    static std::set<void*> done; if (!done.insert(cur_tex0).second) return;
    static SaveTexFn save = nullptr; if (!save) { HMODULE m = LoadLibraryA("d3dx9_43.dll"); if (m) save = (SaveTexFn)GetProcAddress(m, "D3DXSaveTextureToFileA"); }
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f; const unsigned nv = prim_vertices(t, n);
    for (unsigned i = 0; i < nv; i++) { const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + size_t(i) * stride); x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]); y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]); }
    char nm[MAX_PATH]; sprintf(nm, "E:/Recomp/JSR_Mod/textures/%p.png", (void*)cur_tex0);
    CreateDirectoryA("E:/Recomp/JSR_Mod/textures", nullptr);
    const HRESULT hr = save ? save(nm, 3, cur_tex0, nullptr) : E_FAIL;
    log("DUMPTEX %p ecran x %.0f..%.0f y %.0f..%.0f : %s\n", (void*)cur_tex0, x0, x1, y0, y1, SUCCEEDED(hr) ? "ok" : "echec");
}
// Quadrilatere 2D qui couvre exactement toute la cible (video du demarrage, voiles de fondu) : le jeu le place sur les
// bords des pixels (0..L, 0..H) sans le decalage d'un demi-pixel de Direct3D 9 ; la premiere colonne et la premiere ligne
// lisent alors la texture a cheval sur son bord (bandes grises a gauche et en haut). On le decale de -0,5 pixel.
const void* half_pixel_fix(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* data, UINT stride) {
    static std::vector<uint8_t> buf;
    if (!enabled || !data || stride < 16 || t != D3DPT_TRIANGLESTRIP || n != 2) return data;
    // rectangle aligne qui part du coin (0,0) de la zone de rendu du jeu (la video est rendue en 1280x720 puis etiree)
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; bool full = true;
    for (int i = 0; i < 4; i++) {
        const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + size_t(i) * stride);
        x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]); y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]);
        if (v[3] != 1.f) full = false;
    }
    for (int i = 0; i < 4 && full; i++) {
        const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + size_t(i) * stride);
        if (!((v[0] == x0 || v[0] == x1) && (v[1] == y0 || v[1] == y1))) full = false;
    }
    full = full && x1 - x0 >= 640.f && y1 - y0 >= 360.f;   // grand rectangle 2D (videos du demarrage, voiles), ou qu'il soit place
    if (!full) return data;
    buf.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size_t(4) * stride);
    for (int i = 0; i < 4; i++) { float* v = reinterpret_cast<float*>(buf.data() + size_t(i) * stride); v[0] -= 0.5f; v[1] -= 0.5f; }
    return buf.data();
}
HRESULT WINAPI h_drawup(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* data, UINT stride) {
    // Quadrilatere plein ecran (video, voiles) : la texture etait lue en mode « repetition » ; la colonne et la ligne du bord
    // melangeaient le bord oppose (bandes grises). Lecture bornee (clamp) pendant ce dessin, sans deplacer le quadrilatere.
    const bool fs_quad = !in_replay && half_pixel_fix(d, t, n, data, stride) != data;
    DWORD su = 0, sv = 0;
    if (fs_quad) { d->GetSamplerState(0, D3DSAMP_ADDRESSU, &su); d->GetSamplerState(0, D3DSAMP_ADDRESSV, &sv);
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); }
    struct RestoreSS { IDirect3DDevice9* d; bool on; DWORD u, v; ~RestoreSS() { if (on) { d->SetSamplerState(0, D3DSAMP_ADDRESSU, u); d->SetSamplerState(0, D3DSAMP_ADDRESSV, v); } } } restore_ss{d, fs_quad, su, sv};
    if (!in_replay) dump_tex(data, n, stride, t);
    if (model_log && !in_replay) { DWORD st = 0; d->GetRenderState(D3DRS_STENCILENABLE, &st); DWORD v[8] = {}; const D3DRENDERSTATETYPE rs[8] = {D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZENABLE, D3DRS_STENCILPASS};
      for (int k = 0; k < 8; k++) d->GetRenderState(rs[k], &v[k]);
      fprintf(model_log, "dessin UP (image %lu) type %d n %u stride %u tex %p stencil %u func %u ref %X mask %X pass %u blend %u %u/%u z %u couleur %08X\n", frame_no, int(t), n, stride, (void*)cur_tex0, unsigned(st), v[0], v[1], v[2], v[7], v[3], v[4], v[5], v[6], data ? static_cast<const uint32_t*>(data)[4] : 0u);
      if (data && stride >= 16) { fprintf(model_log, "   sommets :"); for (unsigned i = 0; i < prim_vertices(t, n) && i < 6; i++) { const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + size_t(i) * stride); fprintf(model_log, " (%.2f %.2f %.3f %.3f | %.4f %.4f)", v[0], v[1], v[2], v[3], stride >= 32 ? v[6] : 0.f, stride >= 32 ? v[7] : 0.f); } fprintf(model_log, "\n"); } }
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
    if (!in_replay && is_game_vb(vb)) { suivi_ranges.clear(); interp_ranges.clear(); real_ranges.clear(); blur3_ranges.clear(); blur4_ranges.clear(); mranges.clear(); vb_locks++; }
    const HRESULT hr = o_vblock(vb, off, len, p, fl);
    if (SUCCEEDED(hr) && p && *p && !in_replay && is_game_vb(vb)) {
        D3DVERTEXBUFFER_DESC desc; vb->GetDesc(&desc);
        last_vb_data = static_cast<const uint8_t*>(*p); last_vb_bytes = desc.Size - off;
        for (int i = 0; i < game_vb_count; i++) if (game_vbs[i] == vb) vb_base[i] = static_cast<const uint8_t*>(*p) - off;
    }
    return hr;
}
HRESULT WINAPI h_createvb(IDirect3DDevice9* d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* sh) {
    // Les grands tampons du jeu (4 160 000 octets) sont agrandis x3 : place pour les copies intermediaires
    // (le jeu verrouille le tampon entier et n'en verifie pas la capacite).
    const bool game = !in_replay && len == 4160000;
    const HRESULT hr = o_createvb(d, game ? len * 8 : len, usage, fvf, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out && game && game_vb_count < 3) {
        game_vbs[game_vb_count++] = *out;
        if (!o_vblock) o_vblock = (VBLockFn)patch(*out, 11, (void*)h_vblock);
    }
    if (game) log("tampon de sommets du jeu agrandi : %u -> %u octets (%s)\n", len, len * 8, SUCCEEDED(hr) ? "ok" : "echec");
    return hr;
}

// filtre : 1 = image reelle (reel + commun), 2 = image intermediaire (intermediaire + commun)
void replay_of(IDirect3DDevice9* d, const Frame& fr, IDirect3DStateBlock9* sb, int filter, bool partial);
void replay(IDirect3DDevice9* d, int filter) { replay_of(d, frame, sb_begin, filter, false); }
// partial : image du jeu en cours de calcul, rejouee apres une presentation au milieu de son calcul : etats, effacements
// et dessins sans tampon de sommets seulement (son tampon de sommets est verrouille), scene laissee ouverte.
void replay_of(IDirect3DDevice9* d, const Frame& fr, IDirect3DStateBlock9* sb, int filter, bool partial) {
    in_replay = true;
    if (sb) sb->Apply();
    o_begin(d);
    const Frame& frame = fr;
    IDirect3DVertexBuffer9* s0_vb = nullptr; UINT s0_off = 0, s0_stride = 0;
    for (const Cmd& c : frame.cmds) {
        if (partial && (c.op == OP_DRAW || c.op == OP_STRETCH)) continue;
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
        case OP_STREAM: o_stream(d, c.a, (IDirect3DVertexBuffer9*)c.p, c.b, c.c); if (c.a == 0) { s0_vb = (IDirect3DVertexBuffer9*)c.p; s0_off = c.b; s0_stride = c.c; } break;
        case OP_DRAW:
            if (c.d == 0 && c.f > 0 && c.blob_n && !partial) {   // sommets d'ecran : a mi-chemin pour une image intermediaire
                float t = 1.f;
                if (filter & 0xFF) { int j = 0; while (!(filter & (1 << j))) j++; t = sub_t[j]; }
                else if (filter & 0xFF00) { int j = 0; while (!(filter & (1 << (8 + j)))) j++; t = sub_t[j] - blur_s() * (2.f / nsub); }
                if (t < 0.999f) {
                    const UINT stride = UINT(c.f); const uint8_t* cu = frame.blobs.data() + c.blob; const uint8_t* pr = cu + c.blob_n;
                    static std::vector<uint8_t> buf; buf.assign(cu, cu + c.blob_n);
                    bool ok = true;
                    for (size_t o = 0; o + stride <= c.blob_n; o += stride) {
                        const float* a = (const float*)(pr + o); const float* b = (const float*)(cu + o);
                        if (std::fabs(a[0] - b[0]) > 300.f || std::fabs(a[1] - b[1]) > 300.f) { ok = false; break; }   // autre element : pas d'interpolation
                    }
                    if (ok) {
                        for (size_t o = 0; o + stride <= c.blob_n; o += stride) {
                            float* v = (float*)(buf.data() + o); const float* a = (const float*)(pr + o); const float* b = (const float*)(cu + o);
                            for (int k = 0; k < 4; k++) v[k] = a[k] + (b[k] - a[k]) * t;
                        }
                        o_drawup(d, (D3DPRIMITIVETYPE)c.a, c.c, buf.data(), stride);
                        if (s0_vb) o_stream(d, 0, s0_vb, s0_off, s0_stride);
                        break;
                    }
                }
            }
            if (c.d == 0 || (c.d & uint32_t(filter))) o_draw(d, (D3DPRIMITIVETYPE)c.a, c.b, c.c); break;
        case OP_STRETCH: {
            const RECT* rr = (const RECT*)(frame.blobs.data() + c.blob);
            o_stretch(d, (IDirect3DSurface9*)c.p, (c.c & 1) ? &rr[0] : nullptr, (IDirect3DSurface9*)(uintptr_t)c.a, (c.c & 2) ? &rr[1] : nullptr, (D3DTEXTUREFILTERTYPE)c.b);
            break; }
        case OP_DRAWUP: o_drawup(d, (D3DPRIMITIVETYPE)c.a, c.b, frame.blobs.data() + c.blob, c.c); break;
        }
    }
    if (!partial) o_end(d);
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
            char name[300]; if (what[0] == '@') sprintf(name, "%s", what + 1); else if (!strncmp(what, "voile", 5)) sprintf(name, "jsr60-%s.png", what); else sprintf(name, "jsr60-serie-%d-image%lu-%s.png", sets_done, frame_no, what);
            save(name, 3 /* PNG */, rt, nullptr, nullptr);
        }
    }
    if (rt) rt->Release(); if (bb) bb->Release();
}
std::chrono::steady_clock::time_point t_last;
void new_game_frame() {
    frame_models = 0;
    if (model_log) fprintf(model_log, "=== image %lu (image precedente : %u dessins avec pochoir, %u volumes)\n", frame_no, stencil_draws, stencil_volumes);
    stencil_draws = stencil_volumes = 0;
    if (model_log) fprintf(model_log, "LUMIERES df9d74 %.4f df9d0c %.4f\n", *at<float*>(0x00DF9D74), *at<float*>(0x00DF9D0C));
    if (have_view_cur) { memcpy(view_prev, view_cur, 64); have_view_prev = true; have_view_cur = false; }
    std::swap(inst_cur, inst_prev); inst_cur->clear();
    interp_ranges.clear(); real_ranges.clear(); blur3_ranges.clear(); blur4_ranges.clear(); mranges.clear(); tag_ranges.clear();
    {   // test (JSR60_LUMLOG=n) : direction de lumiere et orientation camera, n images apres la fin de l'avance rapide
        static int n = -1; if (n < 0) { const char* e = getenv("JSR60_LUMLOG"); n = e ? atoi(e) : 0; }
        if (n > 0 && !g_ff && g_title_start && kViewMatrix && *kViewMatrix) { const float* v = *kViewMatrix;
            log("LUMLOG image %lu : lumiere %.3f %.3f %.3f | camera avant %.3f %.3f %.3f%s\n", frame_no, *at<float*>(0x0098F5FC), *at<float*>(0x0098F600), *at<float*>(0x0098F604), v[2], v[6], v[10], frame_cut ? " COUPURE" : ""); n--; }
    }
    std::swap(scr_cur, scr_prev); scr_cur.clear();
    sh_flush();
    std::swap(shadow_cur, shadow_prev); shadow_cur.clear(); sh_weld.clear();
    { static int alt = -1; if (alt < 0) alt = getenv("JSR60_OMBRE_ALT") ? 1 : 0; if (alt) shadow_interp_on = (frame_no & 1) != 0; }   // test : une image sur deux comme avant
}
// Moyenne de deux sous-images : la premiere est recopiee (resolue) dans une texture, la seconde est rejouee dans
// le tampon arriere, puis la texture est posee par-dessus a 50 %.
IDirect3DTexture9* blur_tex = nullptr; UINT blur_w = 0, blur_h = 0;
IDirect3DTexture9* prev_tex = nullptr; IDirect3DTexture9* raw_tex = nullptr; bool have_prev = false;   // fondu (> 2 images)
bool resolve_to(IDirect3DDevice9* d, IDirect3DTexture9*& tex) {
    IDirect3DSurface9* bb = nullptr; if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC desc; bb->GetDesc(&desc);
    if (tex) { D3DSURFACE_DESC td; tex->GetLevelDesc(0, &td); if (td.Width != desc.Width || td.Height != desc.Height) { tex->Release(); tex = nullptr; } }
    if (!tex) d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &tex, nullptr);
    blur_w = desc.Width; blur_h = desc.Height;
    bool ok = false;
    if (tex) { IDirect3DSurface9* sf = nullptr; if (SUCCEEDED(tex->GetSurfaceLevel(0, &sf))) { ok = SUCCEEDED(o_stretch(d, bb, nullptr, sf, nullptr, D3DTEXF_NONE)); sf->Release(); } }
    bb->Release(); return ok;
}
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
void blend_tex(IDirect3DDevice9* d, IDirect3DTexture9* tex, DWORD alpha);
void blend_blur(IDirect3DDevice9* d) { blend_tex(d, blur_tex, 0x80); }
void blend_tex(IDirect3DDevice9* d, IDirect3DTexture9* tex, DWORD alpha) {
    in_replay = true;
    o_begin(d);
    D3DVIEWPORT9 vp = {0, 0, blur_w, blur_h, 0.f, 1.f}; o_viewport(d, &vp);
    o_vs(d, nullptr); o_ps(d, nullptr); o_fvf(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    o_rs(d, D3DRS_ZENABLE, FALSE); o_rs(d, D3DRS_ZWRITEENABLE, FALSE); o_rs(d, D3DRS_ALPHATESTENABLE, FALSE);
    o_rs(d, D3DRS_CULLMODE, D3DCULL_NONE); o_rs(d, D3DRS_FOGENABLE, FALSE); o_rs(d, D3DRS_STENCILENABLE, FALSE);
    o_rs(d, D3DRS_SCISSORTESTENABLE, FALSE); o_rs(d, D3DRS_COLORWRITEENABLE, 0xF); o_rs(d, D3DRS_LIGHTING, FALSE);
    o_rs(d, D3DRS_ALPHABLENDENABLE, TRUE); o_rs(d, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); o_rs(d, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    o_rs(d, D3DRS_BLENDOP, D3DBLENDOP_ADD); o_rs(d, D3DRS_TEXTUREFACTOR, (alpha << 24) | 0xFFFFFF);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    o_ss(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    o_ss(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_SRGBTEXTURE, 0);
    o_tex(d, 0, tex); o_tex(d, 1, nullptr);
    const float W = float(blur_w) - 0.5f, H = float(blur_h) - 0.5f;
    struct V { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {W, -0.5f, 0, 1, 1, 0}, {-0.5f, H, 0, 1, 0, 1}, {W, H, 0, 1, 1, 1}};
    o_drawup(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    o_end(d);
    in_replay = false;
}
int64_t qpc_freq() { static int64_t f = 0; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart; } return f; }
int64_t qpc_now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
int64_t t_inter = 0;
void wait_until(int64_t target);
// ---- Cadence d'affichage. Le jeu attend 32 ms entre deux images (0x0058BA6D) : sur un ecran a 60 Hz (33,3 ms
// pour deux rafraichissements) l'ecran doit repeter ou sauter une image toutes les ~0,8 s, tres visible a 60 FPS.
// Le mod prend la cadence : l'attente du jeu est mise a 0 et chaque image (intermediaire puis reelle) part au debut
// d'un rafraichissement de l'ecran -> une image par rafraichissement, jeu a 30 images/s exactement (vitesse Dreamcast).
// Ecran a une autre frequence : periode 32 ms, image reelle a mi-periode.
bool sync60 = false;            // ecran a ~60 Hz (decide a la creation du peripherique)
bool windowed_mode = false; HWND game_wnd = nullptr; UINT win_hz = 0;
// Mode fenetre : frequence de l'ecran qui affiche la fenetre (qui peut etre un 2e ecran, une tele a 60 Hz...),
// relue toutes les 2 s si la fenetre change d'ecran.
UINT window_monitor_hz() {
    static DWORD next = 0; static UINT hz = 0;
    if (GetTickCount() < next && hz) return hz;
    next = GetTickCount() + 2000;
    HMONITOR mon = MonitorFromWindow(game_wnd ? game_wnd : GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmDisplayFrequency != hz) log("fenetre sur un ecran a %u Hz\n", unsigned(dm.dmDisplayFrequency));
        hz = dm.dmDisplayFrequency;
    }
    return hz;
}
int64_t t_next_frame = 0;       // date visee pour l'image suivante du jeu
IDirect3DDevice9* g_dev = nullptr;
bool game_30fps_mode() {        // drapeau du jeu : attente de 32 ms (et non 16) -> [renderer + 0x198]
    const uint8_t* rend = *at<uint8_t**>(0x00B5A280);
    return rend && rend[0x198] != 0;
}
// Attente du rafraichissement suivant par le noyau graphique (D3DKMTWaitForVerticalBlankEvent, gdi32) : fiable a
// 60 comme a 120 Hz, contrairement a la lecture de la ligne de balayage. Ecran = celui de la fenetre du jeu.
typedef LONG NTSTATUS_K;
struct KMT_OPEN_HDC { HDC hDc; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
struct KMT_WAIT_VB { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; };
struct KMT_CLOSE { UINT hAdapter; };
using OpenFromHdcFn = NTSTATUS_K (APIENTRY*)(KMT_OPEN_HDC*);
using WaitVbFn = NTSTATUS_K (APIENTRY*)(const KMT_WAIT_VB*);
using CloseFn = NTSTATUS_K (APIENTRY*)(const KMT_CLOSE*);
WaitVbFn kmt_wait = nullptr; KMT_WAIT_VB kmt_vb = {}; bool kmt_ok = false; HMONITOR kmt_mon = nullptr;
bool kmt_open(HWND wnd) {
    HMONITOR mon = MonitorFromWindow(wnd ? wnd : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (kmt_ok && mon == kmt_mon) return true;
    static HMODULE g = LoadLibraryW(L"gdi32.dll");
    auto open = (OpenFromHdcFn)GetProcAddress(g, "D3DKMTOpenAdapterFromHdc");
    auto close = (CloseFn)GetProcAddress(g, "D3DKMTCloseAdapter");
    kmt_wait = (WaitVbFn)GetProcAddress(g, "D3DKMTWaitForVerticalBlankEvent");
    if (!open || !kmt_wait) return false;
    if (kmt_ok && close) { KMT_CLOSE c{kmt_vb.hAdapter}; close(&c); kmt_ok = false; }
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi); if (!GetMonitorInfoW(mon, &mi)) return false;
    HDC dc = CreateDCW(nullptr, mi.szDevice, nullptr, nullptr); if (!dc) return false;
    KMT_OPEN_HDC oa = {}; oa.hDc = dc;
    const bool ok = open(&oa) >= 0; DeleteDC(dc);
    if (!ok) return false;
    kmt_vb.hAdapter = oa.hAdapter; kmt_vb.hDevice = 0; kmt_vb.VidPnSourceId = oa.VidPnSourceId;
    kmt_ok = true; kmt_mon = mon;
    log("attente des rafraichissements : %ls\n", mi.szDevice);
    return true;
}
void wait_vblank(IDirect3DDevice9*) { if (kmt_open(game_wnd) && kmt_wait) kmt_wait(&kmt_vb); }
UINT screen_hz = 60;
int64_t refresh_half(int64_t f) { return f / (2 * (screen_hz ? screen_hz : 60)); }   // demi-rafraichissement
// attend le moment de presenter : first = image intermediaire (ou image unique), sinon image reelle
void pace(IDirect3DDevice9* d, bool first) {
    const int64_t f = qpc_freq(), now = qpc_now();
    bool vb = sync60;                       // attente du rafraichissement (plein ecran a 60 Hz)
    int64_t period = sync60 ? f / 30 : f * 32 / 1000;
    if (windowed_mode) {                    // fenetre : frequence et rafraichissements de l'ecran qui l'affiche
        vb = false;
        const UINT hz = window_monitor_hz();
        // 59 / 119 = 59,94 / 119,88 Hz (frequences « TV ») : periode allongee de 1001/1000
        if (hz >= 59 && hz <= 61) { period = hz == 59 ? f * 2 * 1001 / 60000 : f * 2 / hz; vb = true; screen_hz = hz; }
        else if (hz >= 119 && hz <= 121) { period = hz == 119 ? f * 4 * 1001 / 120000 : f * 4 / hz; vb = true; screen_hz = hz; }
    }
    if (first) {
        if (!t_next_frame || now > t_next_frame + period) t_next_frame = now;   // retard : on se recale
        if (vb) { wait_until(t_next_frame - refresh_half(f)); wait_vblank(d); }
        else wait_until(t_next_frame);
        t_inter = qpc_now();
        t_next_frame = (vb ? t_inter : t_next_frame) + period;
    } else {
        if (vb) { wait_until(t_inter + period / 2 - refresh_half(f)); wait_vblank(d); }
        else wait_until(t_inter + period / 2);
    }
}
// attente precise : Sleep(1) tant qu'il reste plus de 2 ms, puis boucle active
void wait_until(int64_t target) {
    static bool tp = false; if (!tp) { tp = true; timeBeginPeriod(1); }
    for (;;) {
        const int64_t now = qpc_now(); if (now >= target) return;
        if ((target - now) * 1000 / qpc_freq() > 2) Sleep(1); else YieldProcessor();
    }
}
// ---- Ordonnancement : le jeu avance d'une image toutes les P = 1/30 s (fenetre [win_start, win_start + P)).
// Les images de cette fenetre sont affichees aux rafraichissements de l'ecran qui y tombent ; chacune montre la
// scene a t = (date d'affichage - debut de fenetre) / P. L'attente de 32 ms du jeu est mise a 0 : c'est ici qu'on attend.
int64_t win_start = 0, vb_phase = 0;
int64_t refresh_ticks() {
    UINT hz = window_monitor_hz(); if (hz < 24 || hz > 500) hz = 60;
    double exact = hz; if (hz == 59 || hz == 119 || hz == 143) exact = (hz + 1) * 1000.0 / 1001.0;
    return int64_t(qpc_freq() / exact);
}
// presente l'image affichee au rafraichissement « tau » : attente jusqu'au rafraichissement precedent, puis Present
void wait_for_slot(int64_t tau) {
    // Presentation a l'heure exacte, sans attendre de rafraichissement : mesure a 165 Hz, les images alternent alors
    // regulierement 2 / 3 rafraichissements ; l'attente du rafraichissement (D3DKMT) donnait 1 a 5 (file d'affichage variable).
    wait_until(tau);
}
// prepare les dates t des images de la prochaine image du jeu (avant que le jeu ne dessine ses modeles)
void plan_next() {
    const int64_t f = qpc_freq(), P = f / 30, R = refresh_ticks(), now = qpc_now();
    if (!win_start || now > win_start + 2 * P) win_start = now + R;   // debut ou vrai blocage : on se recale
    if (!vb_phase) { if (kmt_open(game_wnd) && kmt_wait) kmt_wait(&kmt_vb); vb_phase = qpc_now(); }
    int64_t taus[64]; int n = 0;
    int64_t tau = vb_phase + ((win_start - vb_phase) / R + 1) * R;   // premier rafraichissement apres le debut
    for (; tau <= win_start + P && n < 64; tau += R) taus[n++] = tau;
    if (n == 0) taus[n++] = win_start + P;
    // 2 images par image du jeu (60 images/s) sur tout ecran, presentees a mi-fenetre et en fin de fenetre.
    nsub = 2;
    sub_when[0] = win_start + P / 2; sub_t[0] = 0.5f;
    sub_when[1] = win_start + P;     sub_t[1] = 1.f;
    (void)taus; (void)n;
    sub_blur = nsub <= 2;
}
// ---- Contournement du crochet de Steam : lance par Steam, le vrai Present de d3d9.dll commence par un saut vers le
// code de l'overlay/enregistrement de Steam, qui n'est pas fait pour plusieurs images par image du jeu (saccades,
// decor qui clignote). Nos presentations passent par un petit relais : debut d'origine de la fonction (« mov edi,edi ;
// push ebp ; mov ebp,esp », 5 octets, note avant le crochet si possible) puis saut a la suite de la vraie fonction.
uint8_t present_head[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC}; bool head_noted = false;
void* present_stub = nullptr;
void note_present_head() {
    const uint8_t* c = (const uint8_t*)o_present;
    if (!head_noted && c && c[0] != 0xE9 && c[0] != 0xFF) { memcpy(present_head, c, 5); head_noted = true; }
}
PresentFn real_present() {
    const uint8_t* c = (const uint8_t*)o_present;
    if (!c || c[0] != 0xE9) return o_present;                       // pas de crochet
    if (!(present_head[0] == 0x8B && present_head[1] == 0xFF && present_head[2] == 0x55)) return o_present;   // debut inconnu
    if (!present_stub) {
        uint8_t* st = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!st) return o_present;
        memcpy(st, present_head, 5);
        st[5] = 0xE9; const int32_t rel = int32_t(uintptr_t(c + 5) - uintptr_t(st + 10)); memcpy(st + 6, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), st, 16);
        present_stub = st;
        log("Present detourne par un crochet (Steam) : nos presentations le contournent\n");
    }
    return (PresentFn)present_stub;
}
static const DWORD kFade[4] = {0, 0x33, 0x55, 0x73};   // fondu avec l'image precedente (> 2 images)
// prepare dans le tampon arriere l'image j de « shown », attend son rafraichissement et la presente
// ---- F9 : captures des deux images envoyees (intermediaire / reelle) + journal des modeles de cette image du jeu.
// Repere de temps : debut de l'ecran titre = premiere image du jeu ou des modeles sont dessines apres une periode sans.
unsigned long title_start = 0, last_model_frame = 0, show_frame = 0, f9_frame = 0; int f9_count = 0; int f9_left = 0;
char f9_dir[MAX_PATH] = "";
void f9_save(IDirect3DDevice9* d, int j) {
    if (!f9_frame || show_frame != f9_frame) return;
    if (!f9_dir[0]) { if (GetFileAttributesA("E:\\JETSETRADIO") != INVALID_FILE_ATTRIBUTES) strcpy(f9_dir, "E:\\JETSETRADIO\\"); else strcpy(f9_dir, ""); }
    const double tsec = title_start && f9_frame >= title_start ? (f9_frame - title_start) / 30.0 : -1.0;
    char nm[300]; sprintf(nm, "@%sF9_%02d_titre+%06.2fs_%s.png", f9_dir, f9_count, tsec, j == 0 ? "1_intermediaire" : "2_reelle");
    save_bb(d, nm);
    if (j == show_n - 1) {
        log("F9 %d : image du jeu %lu, %.2f s apres le debut de l'ecran titre\n", f9_count, f9_frame, tsec); f9_count++;
        if (f9_left > 0) { f9_left--; f9_frame = f9_frame + 1; } else f9_frame = 0;   // serie (tests) : images suivantes
    }
}
bool only_real = false; bool hold_last = false; bool cut_hold = false;   // saut de l image de coupure : rejete (cache le defaut sans le corriger)
 unsigned log_cut_hold = 0;   // F7 (diagnostic) : l'image intermediaire est remplacee par l'image reelle
// Test (JSR60_F9_COUPE) : capture F9 de chaque debut de plan (image de la coupure + 2 suivantes), avec journal.
void f9_on_cut() {
    static int on = -1; if (on < 0) on = getenv("JSR60_F9_COUPE") ? 1 : 0;
    if (!on || f9_frame || !g_title_start || g_ff) return;
    f9_frame = frame_no; f9_left = 2;
    const char* dd = getenv("JSR60_F9_DIR"); strcpy(f9_dir, dd ? dd : "");
    char nm[300]; sprintf(nm, "%sF9_%02d_modeles.txt", f9_dir, f9_count);
    if (model_log) fclose(model_log); model_log = fopen(nm, "w");
}
// ---- Enregistrement video (JSR60_VIDEO=chemin.mp4, tests) : chaque image envoyee a l'ecran (intermediaire et
// reelle) est ecrite, depuis le debut de la sequence 3D, pendant JSR60_VIDEO_S secondes (90 par defaut). Le jeu
// ralentit si besoin, la video reste a 60 images/s exactes. Encodage par ffmpeg (c:\ffmpeg\bin), sans fenetre.
int video_on = -1; bool video_done = false; HANDLE video_pipe = nullptr; PROCESS_INFORMATION video_pi = {};
unsigned long video_n = 0, video_max = 0; IDirect3DSurface9 *video_full = nullptr, *video_small = nullptr, *video_sys = nullptr;
constexpr UINT kVidW = 1280, kVidH = 720;
void video_frame(IDirect3DDevice9* d, int jj) {
    static char path[MAX_PATH];
    if (video_on < 0) {
        const char* e = getenv("JSR60_VIDEO"); video_on = e ? 1 : 0;
        if (e) { strncpy(path, e, MAX_PATH - 1); const char* sec = getenv("JSR60_VIDEO_S"); video_max = 60ul * (sec ? atoi(sec) : 90); }
        // Enregistrement d'une partie (test) : fichier E:\JETSETRADIO\enregistrer.txt present -> video de toute la session,
        // depuis le demarrage jusqu'a la fermeture du jeu (Alt+F4 : ffmpeg recoit la fin du flux et termine le fichier).
        else if (GetFileAttributesA("E:/JETSETRADIO/enregistrer.txt") != INVALID_FILE_ATTRIBUTES) {
            SYSTEMTIME t; GetLocalTime(&t);
            sprintf(path, "E:/JETSETRADIO/JSR60_partie_%04d-%02d-%02d_%02dh%02d.mkv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
            video_on = 2; video_max = ~0ul;
        }
    }
    if (!video_on || video_done || (!g_title_start && video_on != 2)) return;
    if (!video_pipe) {
        if (jj != 0 || show_n != 2) return;                 // commence sur une image intermediaire
        SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE}; HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 24)) { video_done = true; return; }
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOA si = {}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = rd; si.hStdOutput = nul; si.hStdError = nul;
        static char cmd[2048];
        const char* av = getenv("JSR60_AVANCE"); const double vid_off_s = av ? atof(av) : 0.0; const long vid_off_f = long(vid_off_s * 30);
        sprintf(cmd, "\"c:/ffmpeg/bin/ffmpeg.exe\" -y -loglevel error -f rawvideo -pix_fmt bgra -s %ux%u -r 60 -i - -vf \""
            "drawtext=fontfile='C\\:/Windows/Fonts/arialbd.ttf':text='cinematique + %%{pts\\:hms\\:%.3f}':x=20:y=20:fontsize=30:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=6,"
            "drawtext=fontfile='C\\:/Windows/Fonts/arialbd.ttf':text='image du jeu %%{eif\\:floor(n/2)+%ld\\:d}':x=20:y=62:fontsize=30:fontcolor=white:box=1:boxcolor=black@0.6:boxborderw=6,"
            "drawtext=fontfile='C\\:/Windows/Fonts/arialbd.ttf':text='INTERMEDIAIRE':x=20:y=104:fontsize=30:fontcolor=yellow:box=1:boxcolor=black@0.6:boxborderw=6:enable='eq(mod(n\\,2)\\,0)',"
            "drawtext=fontfile='C\\:/Windows/Fonts/arialbd.ttf':text='REELLE':x=20:y=104:fontsize=30:fontcolor=0x60FF60:box=1:boxcolor=black@0.6:boxborderw=6:enable='eq(mod(n\\,2)\\,1)'"
            "\" -c:v libx264 -preset veryfast -crf 16 -pix_fmt yuv420p \"%s\"", kVidW, kVidH, vid_off_s, vid_off_f, path);
        const BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &video_pi); const DWORD cp_err = GetLastError();
        CloseHandle(rd); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) { CloseHandle(wr); video_done = true; log("video : ffmpeg introuvable (erreur %lu)\n", cp_err); return; }
        video_pipe = wr; log("video : debut de l'enregistrement (%s)\n", path);
    }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC desc; bb->GetDesc(&desc);
    if (!video_full) d->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &video_full, nullptr);
    if (!video_small) d->CreateRenderTarget(kVidW, kVidH, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &video_small, nullptr);
    if (!video_sys) d->CreateOffscreenPlainSurface(kVidW, kVidH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &video_sys, nullptr);
    D3DLOCKED_RECT lr;
    if (video_full && video_small && video_sys && SUCCEEDED(d->StretchRect(bb, nullptr, video_full, nullptr, D3DTEXF_NONE)) &&
        SUCCEEDED(d->StretchRect(video_full, nullptr, video_small, nullptr, D3DTEXF_LINEAR)) &&
        SUCCEEDED(d->GetRenderTargetData(video_small, video_sys)) && SUCCEEDED(video_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        for (UINT y = 0; y < kVidH; y++) {
            uint8_t* row = static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
            for (UINT x = 0; x < kVidW; x++) row[x * 4 + 3] = 255;
            DWORD w = 0; WriteFile(video_pipe, row, kVidW * 4, &w, nullptr);
        }
        video_sys->UnlockRect(); video_n++;
    }
    bb->Release();
    if (video_n >= video_max) {
        CloseHandle(video_pipe); video_pipe = nullptr;
        WaitForSingleObject(video_pi.hProcess, 60000); CloseHandle(video_pi.hProcess); CloseHandle(video_pi.hThread);
        video_done = true; log("video terminee : %lu images\n", video_n);
    }
}
// ---- Bords de l'image : l'image du jeu est rendue dans une zone puis etiree sur l'ecran (filtrage lineaire) ; la ligne et
// la colonne de chaque bord melangent ce qui se trouve juste en dehors de la zone (bandes claires pendant les videos et les
// fondus). Juste avant chaque presentation, chaque bord de l'ecran recoit une copie de la ligne/colonne voisine.
// ---- Couleurs (F11 : coupe / leger / moyen / fort ; moyen par defaut) : vibrance (les couleurs ternes sont renforcees,
// les couleurs deja vives presque pas) + un peu de saturation. Passe sur l'image finale, juste avant l'affichage.
int color_level = 2;
struct XBuffer { virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0; virtual ULONG __stdcall AddRef() = 0; virtual ULONG __stdcall Release() = 0;
                 virtual void* __stdcall GetBufferPointer() = 0; virtual DWORD __stdcall GetBufferSize() = 0; };
using CompileFn = HRESULT (WINAPI*)(LPCSTR, UINT, const void*, void*, LPCSTR, LPCSTR, DWORD, XBuffer**, XBuffer**, void**);
IDirect3DPixelShader9* color_ps = nullptr; bool color_ps_failed = false; IDirect3DTexture9* color_tex = nullptr; IDirect3DStateBlock9* color_sb = nullptr;
bool color_shader(IDirect3DDevice9* d) {
    if (color_ps || color_ps_failed) return color_ps != nullptr;
    color_ps_failed = true;
    HMODULE m = LoadLibraryA("d3dx9_43.dll"); if (!m) { log("couleurs : d3dx9_43.dll absente\n"); return false; }
    CompileFn compile = (CompileFn)GetProcAddress(m, "D3DXCompileShader"); if (!compile) return false;
    static const char src[] =
        "sampler s0 : register(s0); float4 p : register(c0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "  float4 c = tex2D(s0, uv);\n"
        "  float l = dot(c.rgb, float3(0.299, 0.587, 0.114));\n"
        "  float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));\n"
        "  float k = 1.0 + p.x + p.y * (1.0 - sat);\n"
        "  c.rgb = saturate(l + (c.rgb - l) * k);\n"
        "  return c; }\n";
    XBuffer* code = nullptr; XBuffer* err = nullptr;
    if (FAILED(compile(src, UINT(sizeof(src) - 1), nullptr, nullptr, "main", "ps_2_0", 0, &code, &err, nullptr)) || !code) {
        log("couleurs : compilation du shader echouee %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        if (err) err->Release(); return false;
    }
    const HRESULT hr = d->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &color_ps);
    code->Release(); if (err) err->Release();
    if (FAILED(hr)) { color_ps = nullptr; return false; }
    color_ps_failed = false; log("couleurs : shader pret (niveau %d)\n", color_level);
    return true;
}
void apply_colors(IDirect3DDevice9* d) {
    static const float k[4][2] = {{0, 0}, {0.10f, 0.30f}, {0.15f, 0.42f}, {0.35f, 0.85f}};   // saturation, vibrance (moyen = entre leger et l ancien moyen, choisi par l utilisateur)
    { static bool once = false; if (!once) { once = true; if (const char* e = getenv("JSR60_COULEURS")) color_level = atoi(e) & 3; } }   // test
    if (!enabled || color_level <= 0 || !color_shader(d) || !resolve_to(d, color_tex) || !color_tex) return;
    if (!color_sb) d->CreateStateBlock(D3DSBT_ALL, &color_sb); else color_sb->Capture();
    const bool was = in_replay; in_replay = true;
    o_begin(d);
    D3DVIEWPORT9 vp = {0, 0, blur_w, blur_h, 0.f, 1.f}; o_viewport(d, &vp);
    o_vs(d, nullptr); o_ps(d, color_ps); o_fvf(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    const float c0[4] = {k[color_level & 3][0], k[color_level & 3][1], 0, 0}; o_psc(d, 0, c0, 1);
    o_rs(d, D3DRS_ZENABLE, FALSE); o_rs(d, D3DRS_ZWRITEENABLE, FALSE); o_rs(d, D3DRS_ALPHATESTENABLE, FALSE);
    o_rs(d, D3DRS_CULLMODE, D3DCULL_NONE); o_rs(d, D3DRS_FOGENABLE, FALSE); o_rs(d, D3DRS_STENCILENABLE, FALSE);
    o_rs(d, D3DRS_SCISSORTESTENABLE, FALSE); o_rs(d, D3DRS_COLORWRITEENABLE, 0xF); o_rs(d, D3DRS_ALPHABLENDENABLE, FALSE);
    o_rs(d, D3DRS_SRGBWRITEENABLE, FALSE);
    o_ss(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    o_ss(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_SRGBTEXTURE, 0);
    o_tex(d, 0, color_tex); o_tex(d, 1, nullptr);
    const float W = float(blur_w) - 0.5f, H = float(blur_h) - 0.5f;
    struct V { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {W, -0.5f, 0, 1, 1, 0}, {-0.5f, H, 0, 1, 0, 1}, {W, H, 0, 1, 1, 1}};
    o_drawup(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    o_end(d);
    in_replay = was;
    if (color_sb) color_sb->Apply();
}
IDirect3DTexture9* edge_tex = nullptr; IDirect3DStateBlock9* edge_sb = nullptr;
void fix_edges(IDirect3DDevice9* d) {
    if (!enabled || !resolve_to(d, edge_tex) || !edge_tex) return;   // copie de l'ecran (resolue si anticrenelage)
    if (!edge_sb) d->CreateStateBlock(D3DSBT_ALL, &edge_sb); else edge_sb->Capture();
    const bool was = in_replay; in_replay = true;
    o_begin(d);
    D3DVIEWPORT9 vp = {0, 0, blur_w, blur_h, 0.f, 1.f}; o_viewport(d, &vp);
    o_vs(d, nullptr); o_ps(d, nullptr); o_fvf(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    o_rs(d, D3DRS_ZENABLE, FALSE); o_rs(d, D3DRS_ZWRITEENABLE, FALSE); o_rs(d, D3DRS_ALPHATESTENABLE, FALSE);
    o_rs(d, D3DRS_CULLMODE, D3DCULL_NONE); o_rs(d, D3DRS_FOGENABLE, FALSE); o_rs(d, D3DRS_STENCILENABLE, FALSE);
    o_rs(d, D3DRS_SCISSORTESTENABLE, FALSE); o_rs(d, D3DRS_COLORWRITEENABLE, 0xF); o_rs(d, D3DRS_LIGHTING, FALSE);
    o_rs(d, D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    o_ss(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); o_ss(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    o_ss(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); o_ss(d, 0, D3DSAMP_SRGBTEXTURE, 0);
    o_tex(d, 0, edge_tex); o_tex(d, 1, nullptr);
    const float W = float(blur_w), H = float(blur_h);
    struct V { float x, y, z, rhw, u, v; };
    auto strip = [&](float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
        V q[4] = {{x0, y0, 0, 1, u0, v0}, {x1, y0, 0, 1, u1, v0}, {x0, y1, 0, 1, u0, v1}, {x1, y1, 0, 1, u1, v1}};
        o_drawup(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    };
    const float vr1 = 1.5f / H, vrb = (H - 1.5f) / H, uc1 = 1.5f / W, ucr = (W - 1.5f) / W;
    strip(-0.5f, -0.5f, W - 0.5f, 0.5f, 0.f, vr1, 1.f, vr1);              // haut <- ligne 1
    strip(-0.5f, H - 1.5f, W - 0.5f, H - 0.5f, 0.f, vrb, 1.f, vrb);       // bas <- avant-derniere ligne
    strip(-0.5f, -0.5f, 0.5f, H - 0.5f, uc1, 0.f, uc1, 1.f);              // gauche <- colonne 1
    strip(W - 1.5f, -0.5f, W - 0.5f, H - 0.5f, ucr, 0.f, ucr, 1.f);       // droite <- avant-derniere colonne
    o_end(d);
    in_replay = was;
    if (edge_sb) edge_sb->Apply();
}
// ---- Mesure de regularite (fichier jsr60-mesure.txt dans le dossier du jeu) : 20 s apres le debut de la sequence 3D,
// ecart entre deux presentations successives pendant 1200 images, en nombre de rafraichissements de l'ecran.
void measure_present(int jj) {
    static int on = -1; if (on < 0) { FILE* f = fopen("jsr60-mesure.txt", "r"); on = f ? 1 : 0; if (f) fclose(f); }
    if (!on || !g_title_start || frame_no < g_title_start + 300) return;
    static std::vector<double> gaps; static int64_t last = 0; static bool done = false; static int order_bad = 0, last_j = -1;
    if (done) return;
    const int64_t now = qpc_now();
    if (last) gaps.push_back(double(now - last) * 1000.0 / double(qpc_freq()));
    if (last_j >= 0 && jj == last_j && show_n == 2) order_bad++;   // deux fois la meme sorte d'image de suite
    last = now; last_j = jj;
    if (gaps.size() >= 1200) {
        done = true;
        const double R = screen_hz ? 1000.0 / screen_hz : 16.667;
        unsigned c1 = 0, c2 = 0, c3 = 0, cfast = 0; double mx = 0, sum = 0;
        for (double g : gaps) { const double k = g / R; if (k < 0.5) cfast++; else if (k < 1.5) c1++; else if (k < 2.5) c2++; else c3++; mx = std::max(mx, g); sum += g; }
        std::vector<double> s2 = gaps; std::sort(s2.begin(), s2.end());
        log("MESURE regularite (%u Hz, %u images) : 1 rafraichissement %u, 2 rafraichissements %u, 3 ou plus %u, trop tot %u ; ecart median %.2f ms, moyen %.2f ms, max %.1f ms ; meme sorte d'image deux fois de suite %d\n",
            screen_hz, unsigned(gaps.size()), c1, c2, c3, cfast, s2[s2.size() / 2], sum / gaps.size(), mx, order_bad);
    }
}
HRESULT show_image(IDirect3DDevice9* d, int jj) {
    const int j = (only_real || hold_last) ? show_n - 1 : jj;
    bool mix = false;
    if (show_blur && blur_level > 0 && shown.blur_draws) { replay_of(d, shown, sb_shown, 1 << (8 + j), false); mix = resolve_to_blur(d); }
    replay_of(d, shown, sb_shown, 1 << j, false);
    if (mix) blend_blur(d);
    else if (!show_blur && blur_level > 0) {
        const bool got = resolve_to(d, raw_tex);
        if (have_prev && prev_tex) blend_tex(d, prev_tex, kFade[blur_level & 3]);
        if (got) { std::swap(prev_tex, raw_tex); have_prev = true; }
    }
    apply_colors(d);
    fix_edges(d);
    f9_save(d, jj);
    video_frame(d, jj);
    wait_for_slot(show_when[jj]);
    const HRESULT phr = real_present()(d, nullptr, nullptr, nullptr, nullptr);
    measure_present(jj);
    return phr;
}
// images de « shown » dont le rafraichissement arrive : until = date limite (0 = toutes, en attendant)
void show_pending(IDirect3DDevice9* d, int64_t until) {
    const int64_t R = refresh_ticks();
    while (show_next < show_n) {
        const int j = show_next;
        if (until && show_when[j] - R - R / 2 > until) break;            // pas encore le moment
        show_next++;
        if (j < show_n - 1 && qpc_now() > show_when[j] + R && !f9_frame && !(video_on > 0 && !video_done)) continue;   // plus d'un rafraichissement de retard : sautee (sauf capture F9)
        show_image(d, j);
    }
}
// depuis le dessin des modeles : presentation au milieu du calcul de l'image suivante
void tick_present() {
    if (!dev || in_replay || show_next >= show_n || !sb_shown) return;
    const int64_t R = refresh_ticks();
    if (qpc_now() < show_when[show_next] - R - R / 2) return;
    IDirect3DDevice9* d = dev;
    if (!sb_save) d->CreateStateBlock(D3DSBT_ALL, &sb_save); else sb_save->Capture();
    const bool scene = in_scene;
    if (scene) o_end(d);
    show_pending(d, qpc_now());
    if (scene) replay_of(d, frame, sb_begin, 0x7FFFFFFF, true);   // reouvre la scene et refait l'image en cours
    if (sb_save) sb_save->Apply();
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
    {   // F9 : capture de la prochaine image du jeu (son journal des modeles s'ouvre maintenant)
        static bool f9_down = false; const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !f9_down && !f9_frame) {
            f9_frame = frame_no + 1;
            { const char* e = getenv("JSR60_F9N"); f9_left = e ? std::max(0, atoi(e) - 1) : 0; }
            char nm[300]; sprintf(nm, "%sF9_%02d_modeles.txt", f9_dir[0] ? f9_dir : (GetFileAttributesA("E:\\JETSETRADIO") != INVALID_FILE_ATTRIBUTES ? "E:\\JETSETRADIO\\" : ""), f9_count);
            if (model_log) fclose(model_log); model_log = fopen(nm, "w");
        }
        f9_down = f9;
        if (frame_models > 0) { if (!last_model_frame || frame_no > last_model_frame + 60) { title_start = frame_no; g_title_start = frame_no; log("debut d'une sequence 3D (ecran titre) : image %lu\n", frame_no); } last_model_frame = frame_no; }
    }
    { static bool f5_down = false; const bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
      if (f5 && !f5_down) { tag_fixes = !tag_fixes; log("F5 : corrections des tags %s\n", tag_fixes ? "oui" : "non"); } f5_down = f5; }
    { static bool f11_down = false; const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
      static const char* cn[4] = {"coupe", "leger", "moyen", "fort"};
      if (f11 && !f11_down) { color_level = (color_level + 1) & 3; log("F11 : couleurs %s\n", cn[color_level]); } f11_down = f11; }
    { static bool f7_down = false; const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
      if (f7 && !f7_down) { only_real = !only_real; log("F7 : image intermediaire %s\n", only_real ? "remplacee par la reelle" : "normale"); } f7_down = f7; }
    { static bool f6_down = false; const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
      if (f6 && !f6_down) { shadow_interp_on = !shadow_interp_on; log("F6 : ombre interpolee %s\n", shadow_interp_on ? "oui" : "non (comme avant)"); } f6_down = f6; }
    { static bool once = false; if (!once) { once = true; if (getenv("JSR60_OFF")) { enabled = false; log("mod coupe au demarrage (test)\n"); } } }
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !f8_down) { enabled = !enabled; log("F8 : interpolation %s\n", enabled ? "activee" : "coupee"); }
    f8_down = f8;
    bool two = false;
    // mesure (diagnostic) : temps entre deux Present du jeu et temps passe dans les rejeux
    static LARGE_INTEGER qf = {}, t_prev = {}; if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    static std::vector<float> gaps, works; static int mes_logged = 0;
    if (t_prev.QuadPart) gaps.push_back(float(double(t0.QuadPart - t_prev.QuadPart) * 1000.0 / qf.QuadPart));
    if (const char* e = getenv("JSR60_FLOU")) { static bool done = false; if (!done) { done = true; blur_level = atoi(e) & 3; } }
    const bool paced = game_30fps_mode();
    HRESULT hr = D3D_OK;
    dev = d;
    // Avance rapide (tests, JSR60_AVANCE=secondes) : jusqu'a ce moment de la cinematique, rien n'est montre ni attendu
    // (une image sur 16 seulement), le jeu calcule donc ses images aussi vite que possible.
    static long ff_frames = -1; if (ff_frames < 0) { const char* e = getenv("JSR60_AVANCE"); ff_frames = e ? long(atof(e) * 30) : 0; }
    static unsigned long first_start = 0; if (!first_start && g_title_start) first_start = g_title_start;
    const bool ff = ff_frames > 0 && (!first_start || frame_no < first_start + (unsigned long)ff_frames);
    {   // capture F9 automatique a une image absolue (tests, JSR60_F9_IMG=numero d'image du jeu)
        static long f9i = -1; if (f9i < 0) { const char* e = getenv("JSR60_F9_IMG"); f9i = e ? atol(e) : 0; }
        if (f9i > 0 && frame_no == (unsigned long)f9i && !f9_frame) {
            f9_frame = frame_no + 1; { const char* e = getenv("JSR60_F9N"); f9_left = e ? std::max(0, atoi(e) - 1) : 0; }
            const char* dd = getenv("JSR60_F9_DIR"); strcpy(f9_dir, dd ? dd : "");
            char nm[300]; sprintf(nm, "%sF9_%02d_modeles.txt", f9_dir, f9_count);
            if (model_log) fclose(model_log); model_log = fopen(nm, "w");
        }
    }
    {   // capture F9 automatique (tests, JSR60_F9_A=secondes de cinematique depuis le tout premier debut)
        static long f9a = -1; if (f9a < 0) { const char* e = getenv("JSR60_F9_A"); f9a = e ? long(atof(e) * 30) : 0; }
        if (f9a > 0 && first_start && frame_no == first_start + (unsigned long)f9a && !f9_frame) {
            f9_frame = frame_no + 1; { const char* e = getenv("JSR60_F9N"); f9_left = e ? std::max(0, atoi(e) - 1) : 0; }
            char nm[300]; { const char* dd = getenv("JSR60_F9_DIR"); strcpy(f9_dir, dd ? dd : (GetFileAttributesA("E:/JETSETRADIO") != INVALID_FILE_ATTRIBUTES ? "E:/JETSETRADIO/" : "")); } sprintf(nm, "%sF9_%02d_modeles.txt", f9_dir, f9_count);
            if (model_log) fclose(model_log); model_log = fopen(nm, "w");
        }
    }
    g_ff = ff;
    if (ff) { show_n = show_next = 0; static unsigned k = 0; if ((++k & 63) == 0) hr = real_present()(d, a, b, w, r); win_start = 0; } else {
    if (paced) show_pending(d, 0);                   // images restantes de l'image precedente (attente de leurs rafraichissements)
    // Premiere image d'un nouveau plan : le jeu la calcule avec un eclairage d'objets encore faux (panneaux, portes :
    // faces claires/sombres inversees pendant une image). On ne la montre pas : la derniere image du plan precedent
    // reste 1/30 s de plus, le nouveau plan commence a l'image suivante (correcte).
    if (cut_hold && enabled && frame_cut && frame.valid && sb_begin && paced && sb_shown && !shown.cmds.empty()) {
        if (!sb_end) d->CreateStateBlock(D3DSBT_ALL, &sb_end); else sb_end->Capture();
        for (IUnknown* u : held) u->Release(); held.clear();
        hold_last = true; show_n = nsub; show_next = 0; show_frame = frame_no;
        for (int j = 0; j < nsub; j++) show_when[j] = sub_when[j];
        show_pending(d, qpc_now() + refresh_ticks() / 2);
        if (sb_end) sb_end->Apply();
        two = true;
        log_cut_hold++;
    } else
    if (frame.valid && frame.interp_draws > 0 && sb_begin && paced) {
        if (!sb_end) d->CreateStateBlock(D3DSBT_ALL, &sb_end); else sb_end->Capture();
        hold_last = false;
        // passage de relais : cette image du jeu devient « shown » ; ses images seront montrees a leurs rafraichissements
        std::swap(shown, frame); std::swap(sb_shown, sb_begin);
        for (IUnknown* u : held_shown) u->Release(); held_shown.clear(); held_shown.swap(held);
        show_n = nsub; show_next = 0; show_blur = sub_blur; show_frame = frame_no;
        for (int j = 0; j < nsub; j++) show_when[j] = sub_when[j];
        show_pending(d, qpc_now() + refresh_ticks() / 2);   // celles qui arrivent tout de suite
        if (sb_end) sb_end->Apply();
        two = true;
    } else {
        show_n = show_next = 0;
        if (paced) { have_prev = false; wait_for_slot(win_start + qpc_freq() / 30); }   // image unique (F8, chargements)
        if (f9_frame && frame_no == f9_frame) { show_frame = f9_frame; show_n = 1; f9_save(d, 0); show_n = 0; }   // capture F9 mod coupe
        apply_colors(d);
        fix_edges(d);
        if (video_on < 0) video_frame(d, 1);   // lit la configuration de l'enregistrement des la premiere image
        if (!enabled || video_on == 2) { show_n = 2; video_frame(d, 0); video_frame(d, 1); show_n = 0; }   // image unique (menus, chargements) : doublee   // video mod coupe : image doublee (60 i/s)
        hr = real_present()(d, a, b, w, r);
    }
    if (paced) { win_start += qpc_freq() / 30; plan_next(); }
    else { nsub = 2; sub_t[0] = 0.5f; sub_t[1] = 1.f; sub_blur = true; win_start = 0; }
    }   // fin de l'avance rapide
    { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); works.push_back(float(double(t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart)); t_prev = t1;
      if (gaps.size() > 4000) { gaps.clear(); works.clear(); }
      if (gaps.size() >= 900 && mes_logged < 0) {
          auto pct = [](std::vector<float> v, float q) { std::sort(v.begin(), v.end()); return v[size_t(q * (v.size() - 1))]; };
          int late = 0; for (float g : gaps) if (g > 40.f) late++;
          log("mesure (flou %d) : entre images du jeu med %.1f ms, 95%% %.1f, max %.1f, >40 ms : %d/%u ; rejeux+presentations med %.1f ms, 95%% %.1f, max %.1f\n",
              blur_level, pct(gaps, .5f), pct(gaps, .95f), pct(gaps, 1.f), late, unsigned(gaps.size()), pct(works, .5f), pct(works, .95f), pct(works, 1.f));
          gaps.clear(); works.clear(); mes_logged++;
      } }
    if (kStatsLog && frame_no % 300 == 0) {   // statistiques periodiques : coupees (ecriture disque toutes les 10 s)
        const auto now = std::chrono::steady_clock::now();
        log("image %lu : %s, modeles %u (a mi-chemin %u, sans precedent %u, sauts %u, copies vides %u, interface %u), dessins intermediaires %u, verrous %u (%.1f s)\n",
            frame_no, two ? "2 images" : "1 image", st.models, st.interp, st.same, st.jump, st.empty, st.hud, frame.interp_draws, vb_locks,
            std::chrono::duration<double>(now - t_last).count());
        if (tags_pulled) log("  modeles de tags rapproches : %u, couches de tags reordonnees : %u\n", tags_pulled, tag_layers_fixed); tags_pulled = 0; tag_layers_fixed = 0; t_last = now; st = Stats{}; vb_locks = 0;
    }
    frame_no++; frame.valid = false;
    if (model_log && !(f9_frame && frame_no <= f9_frame)) { fclose(model_log); model_log = nullptr; }
    if (sets_done < sets_max) {   // images capturees sera capturee : journal des modeles
        char name[80]; sprintf(name, "jsr60-modeles-%lu.txt", frame_no); model_log = fopen(name, "w");
    }
    new_game_frame();
    return hr;
}
void upgrade_depth(D3DPRESENT_PARAMETERS* pp); void set_anisotropy(IDirect3DDevice9* d); void force_60hz(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp);
void borderless_if_needed(IDirect3D9* d3d, UINT ad, HWND w, D3DPRESENT_PARAMETERS* pp);
HRESULT WINAPI h_reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    { IDirect3D9* d3d = nullptr; D3DDEVICE_CREATION_PARAMETERS cp = {}; d->GetCreationParameters(&cp);
      if (SUCCEEDED(d->GetDirect3D(&d3d)) && d3d) { borderless_if_needed(d3d, cp.AdapterOrdinal, cp.hFocusWindow, pp); d3d->Release(); } }
    if (sb_begin) { sb_begin->Release(); sb_begin = nullptr; } if (sb_end) { sb_end->Release(); sb_end = nullptr; }
    frame.valid = false; frame.cmds.clear(); for (IUnknown* u : held) u->Release(); held.clear(); release_backups();
    if (blur_tex) { blur_tex->Release(); blur_tex = nullptr; }
    if (edge_tex) { edge_tex->Release(); edge_tex = nullptr; } if (edge_sb) { edge_sb->Release(); edge_sb = nullptr; }
    if (color_tex) { color_tex->Release(); color_tex = nullptr; } if (color_sb) { color_sb->Release(); color_sb = nullptr; }
    if (sb_shown) { sb_shown->Release(); sb_shown = nullptr; } if (sb_save) { sb_save->Release(); sb_save = nullptr; }
    for (IUnknown* u : held_shown) u->Release(); held_shown.clear(); shown.cmds.clear(); show_n = show_next = 0;
    if (prev_tex) { prev_tex->Release(); prev_tex = nullptr; } if (raw_tex) { raw_tex->Release(); raw_tex = nullptr; } have_prev = false;
    upgrade_depth(pp);
    if (pp) { UINT hz = pp->Windowed ? 0 : pp->FullScreen_RefreshRateInHz; if (!hz) { D3DDISPLAYMODE dm; if (SUCCEEDED(d->GetDisplayMode(0, &dm))) hz = dm.RefreshRate; } sync60 = !pp->Windowed && ((hz >= 59 && hz <= 61) || (hz >= 119 && hz <= 121)); screen_hz = hz; windowed_mode = pp->Windowed != FALSE; }
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
// Plein ecran : le jeu garde la frequence du bureau (165 Hz chez l'utilisateur) alors que ses reglages demandent 60 Hz.
// A 165 Hz, une image toutes les 32 ms reste tantot 2, tantot 3 rafraichissements -> saccades. On demande 60 Hz si
// l'ecran propose ce mode a cette resolution ; la cadence se cale alors sur les rafraichissements (voir pace).
void force_60hz(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp) {
    // 120 Hz de preference (multiple de 30 : 4 rafraichissements par image du jeu, mode « PC » ou la carte garde
    // les couleurs en plage complete) ; sinon 60 Hz (qui peut passer la sortie en plage limitee : noirs gris).
    if (!d3d || !pp || pp->Windowed) return;
    const D3DFORMAT fmt = pp->BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    const UINT n = d3d->GetAdapterModeCount(adapter, fmt);
    bool has120 = false, has60 = false;
    for (UINT i = 0; i < n; i++) {
        D3DDISPLAYMODE m;
        if (SUCCEEDED(d3d->EnumAdapterModes(adapter, fmt, i, &m)) && m.Width == pp->BackBufferWidth && m.Height == pp->BackBufferHeight) {
            if (m.RefreshRate == 120) has120 = true;
            if (m.RefreshRate == 60) has60 = true;
        }
    }
    const UINT target = has120 ? 120 : (has60 ? 60 : 0);
    if (!target) { log("plein ecran : ni 120 ni 60 Hz, %u Hz conserve\n", pp->FullScreen_RefreshRateInHz); return; }
    if (pp->FullScreen_RefreshRateInHz != target) log("plein ecran : %u Hz -> %u Hz\n", pp->FullScreen_RefreshRateInHz, target);
    pp->FullScreen_RefreshRateInHz = target;
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
// ---- Resolution plus grande que celles de l'ecran (supersampling, ex. 3840x2160 sur une tele 1080p sans mise a l'echelle
// du pilote) : le plein ecran exclusif est refuse et le jeu plante. Si le mode n'existe pas sur l'ecran, on passe en plein
// ecran sans bordure : fenetre sans cadre a la taille de l'ecran, image toujours calculee dans la resolution demandee
// (la presentation la reduit a la taille de la fenetre).
bool fake_fullscreen = false;
bool mode_exists(IDirect3D9* d3d, UINT ad, UINT w, UINT h) {
    const D3DFORMAT fmts[2] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5};
    for (D3DFORMAT f : fmts) { const UINT n = d3d->GetAdapterModeCount(ad, f);
        for (UINT i = 0; i < n; i++) { D3DDISPLAYMODE m; if (SUCCEEDED(d3d->EnumAdapterModes(ad, f, i, &m)) && m.Width == w && m.Height == h) return true; } }
    return false;
}
void borderless_if_needed(IDirect3D9* d3d, UINT ad, HWND w, D3DPRESENT_PARAMETERS* pp) {
    if (!d3d || !pp) return;
    if (pp->Windowed && !fake_fullscreen) return;   // fenetre choisie par le joueur
    const bool force = getenv("JSR60_SANSBORDURE") != nullptr;   // test
    if (!force && !pp->Windowed && mode_exists(d3d, ad, pp->BackBufferWidth, pp->BackBufferHeight)) {
        fake_fullscreen = false;
        // frequence demandee absente a cette resolution (reglage du jeu fait sur un autre ecran, ex. 75 Hz du Dell sur la
        // tele a 60 Hz) : creation refusee -> plantage. On prend 120 Hz, sinon 60 Hz, sinon la plus haute disponible.
        const UINT want = pp->FullScreen_RefreshRateInHz; bool have = want == 0; UINT best = 0; bool h120 = false, h60 = false;
        const D3DFORMAT fmts[2] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5};
        for (D3DFORMAT f : fmts) { const UINT n = d3d->GetAdapterModeCount(ad, f);
            for (UINT i = 0; i < n; i++) { D3DDISPLAYMODE m; if (FAILED(d3d->EnumAdapterModes(ad, f, i, &m)) || m.Width != pp->BackBufferWidth || m.Height != pp->BackBufferHeight) continue;
                if (m.RefreshRate == want) have = true; if (m.RefreshRate == 120) h120 = true; if (m.RefreshRate == 60) h60 = true; best = std::max(best, m.RefreshRate); } }
        if (!have) { const UINT hz = h120 ? 120 : (h60 ? 60 : best);
            log("frequence %u Hz absente en %ux%u sur cet ecran : %u Hz'''+B+'''n", want, pp->BackBufferWidth, pp->BackBufferHeight, hz);
            pp->FullScreen_RefreshRateInHz = hz; }
        return;
    }
    if (!force && pp->Windowed && fake_fullscreen && mode_exists(d3d, ad, pp->BackBufferWidth, pp->BackBufferHeight)) return;   // (Reset suivant) on reste en sans bordure
    HWND wnd = pp->hDeviceWindow ? pp->hDeviceWindow : w;
    MONITORINFO mi = {sizeof mi}; HMONITOR mon = d3d->GetAdapterMonitor(ad);
    if (!wnd || !mon || !GetMonitorInfoW(mon, &mi)) return;
    log("resolution %ux%u absente de l'ecran : plein ecran sans bordure (%ldx%ld)\n", pp->BackBufferWidth, pp->BackBufferHeight,
        mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    pp->Windowed = TRUE; pp->FullScreen_RefreshRateInHz = 0; fake_fullscreen = true;
    SetWindowLongPtrW(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(wnd, GWL_EXSTYLE, GetWindowLongPtrW(wnd, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME));
    SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}
// Ecran choisi (fichier jsr60-ecran.txt dans le dossier du jeu, ecrit par le script de lancement sur la tele) : numero
// de l'ecran Windows (2 = \.\DISPLAY2). Le jeu s'ouvre alors sur cet ecran au lieu de l'ecran principal.
UINT chosen_adapter(IDirect3D9* d3d, UINT ad) {
    FILE* f = fopen("jsr60-ecran.txt", "r"); if (!f) return ad;
    int n = 0; if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f);
    if (n <= 0) return ad;
    char want[32]; sprintf(want, "%c%c.%cDISPLAY%d", 92, 92, 92, n);   // \.DISPLAYn
    for (UINT i = 0; i < d3d->GetAdapterCount(); i++) {
        D3DADAPTER_IDENTIFIER9 id; if (SUCCEEDED(d3d->GetAdapterIdentifier(i, 0, &id)) && !_stricmp(id.DeviceName, want)) {
            log("ecran choisi : %s (adaptateur %u)\n", want, i); return i; }
    }
    log("ecran choisi %s introuvable : ecran principal\n", want); return ad;
}
HRESULT WINAPI h_createdev(IDirect3D9* self, UINT ad, D3DDEVTYPE ty, HWND w, DWORD fl, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    ad = chosen_adapter(self, ad);
    borderless_if_needed(self, ad, w, pp);
    const D3DFORMAT asked = pp ? pp->AutoDepthStencilFormat : D3DFMT_UNKNOWN;
    upgrade_depth(pp);
    const UINT asked_hz = pp ? pp->FullScreen_RefreshRateInHz : 0;

    HRESULT hr = o_createdev(self, ad, ty, w, fl, pp, out);
    if (FAILED(hr) && pp && pp->FullScreen_RefreshRateInHz != asked_hz) {   // 60 Hz refuse : frequence d'origine
        log("  60 Hz refuse, frequence d'origine\n"); pp->FullScreen_RefreshRateInHz = asked_hz;
        hr = o_createdev(self, ad, ty, w, fl, pp, out);
    }
    if (FAILED(hr) && pp && pp->AutoDepthStencilFormat != asked) {   // refuse par la carte : format d'origine
        log("  D24S8 refuse, retour au format d'origine\n"); pp->AutoDepthStencilFormat = asked;
        hr = o_createdev(self, ad, ty, w, fl, pp, out);
    }
    if (SUCCEEDED(hr) && out && *out && !o_present) {
        IDirect3DDevice9* d = *out;
        o_reset = (ResetFn)patch(d, 16, (void*)h_reset);
        o_present = (PresentFn)patch(d, 17, (void*)h_present);
        note_present_head();
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
        {   // frequence de l'ecran
            UINT hz = pp->Windowed ? 0 : pp->FullScreen_RefreshRateInHz;
            if (!hz) { D3DDISPLAYMODE dm; if (SUCCEEDED(d->GetDisplayMode(0, &dm))) hz = dm.RefreshRate; }
            sync60 = !pp->Windowed && ((hz >= 59 && hz <= 61) || (hz >= 119 && hz <= 121)); screen_hz = hz;
            windowed_mode = pp->Windowed != FALSE; game_wnd = pp->hDeviceWindow ? pp->hDeviceWindow : w;
            log("adaptateur %u, fenetre %d\n", ad, int(pp->Windowed));
            // attente de 32 ms du jeu mise a 0 : c'est le mod qui cadence (voir pace)
            uint8_t* imm = at<uint8_t*>(0x0058BA6E);
            DWORD old; if (VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old)) { const uint32_t z = 0; memcpy(imm, &z, 4); VirtualProtect(imm, 4, old, &old); }
            log("ecran %u Hz : cadence %s\n", hz, sync60 ? "calee sur les rafraichissements (30 images/s)" : "32 ms");
        }
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
// Le mod ne s'active que dans le jeu : d'autres programmes du dossier (jsrsetup.exe, les reglages) chargent aussi
// cette d3d9.dll et n'ont pas le code du jeu aux adresses modifiees. Pour eux : simple relais vers le vrai DirectX.
bool host_is_game() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    const wchar_t* n = wcsrchr(p, L'\\'); n = n ? n + 1 : p;
    return _wcsicmp(n, L"jetsetradio.exe") == 0;
}
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (!load_real()) return nullptr;
    if (!host_is_game()) return real_create(sdk);
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
