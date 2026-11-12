// Installeur du mod « Jet Set Radio 60 FPS » (d3d9.dll intermediaire).
// Programme Windows sans console : trouve le jeu (Steam ou choix du dossier), installe / met a jour / desinstalle.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "ressources.h"

static const wchar_t* kTitre = L"Jet Set Radio 60 FPS - Installation";
static const wchar_t* kSauvegarde = L"d3d9.dll.avant_jsr60";

static bool existe(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool dossier_du_jeu(const std::wstring& d) { return !d.empty() && existe(d + L"\\jetsetradio.exe"); }

static std::vector<char> ressource(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return {};
    HGLOBAL h = LoadResource(nullptr, r);
    const char* p = static_cast<const char*>(LockResource(h));
    return std::vector<char>(p, p + SizeofResource(nullptr, r));
}

static std::wstring lire_registre(HKEY racine, const wchar_t* cle, const wchar_t* valeur) {
    wchar_t buf[MAX_PATH] = {}; DWORD taille = sizeof(buf);
    if (RegGetValueW(racine, cle, valeur, RRF_RT_REG_SZ, nullptr, buf, &taille) == ERROR_SUCCESS) return buf;
    return L"";
}

// Bibliotheques Steam : dossier de Steam + chemins "path" de steamapps\libraryfolders.vdf
static std::vector<std::wstring> bibliotheques_steam() {
    std::vector<std::wstring> res;
    std::wstring steam = lire_registre(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = lire_registre(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) return res;
    for (auto& c : steam) if (c == L'/') c = L'\\';
    res.push_back(steam);
    std::ifstream f(steam + L"\\steamapps\\libraryfolders.vdf");
    std::string ligne;
    while (std::getline(f, ligne)) {
        const size_t k = ligne.find("\"path\"");
        if (k == std::string::npos) continue;
        const size_t a = ligne.find('"', k + 6); if (a == std::string::npos) continue;
        const size_t b = ligne.find('"', a + 1); if (b == std::string::npos) continue;
        std::string chemin = ligne.substr(a + 1, b - a - 1), net;
        for (size_t i = 0; i < chemin.size(); i++) { if (chemin[i] == '\\' && i + 1 < chemin.size() && chemin[i + 1] == '\\') i++; net += chemin[i]; }
        const int n = MultiByteToWideChar(CP_UTF8, 0, net.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, 0, net.c_str(), -1, &w[0], n);
        res.push_back(w);
    }
    return res;
}

static std::wstring choisir_dossier(HWND h) {
    BROWSEINFOW bi = {}; bi.hwndOwner = h;
    bi.lpszTitle = L"Choisissez le dossier de Jet Set Radio (celui qui contient jetsetradio.exe) :";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    wchar_t chemin[MAX_PATH] = {}; SHGetPathFromIDListW(pidl, chemin); CoTaskMemFree(pidl);
    return chemin;
}

static bool ecrire(const std::wstring& chemin, const std::vector<char>& donnees, DWORD& erreur) {
    HANDLE f = CreateFileW(chemin.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { erreur = GetLastError(); return false; }
    DWORD ecrit = 0; const BOOL ok = WriteFile(f, donnees.data(), DWORD(donnees.size()), &ecrit, nullptr);
    erreur = ok ? 0 : GetLastError(); CloseHandle(f);
    return ok && ecrit == donnees.size();
}

// Notre d3d9.dll contient la chaine "jsr60-log.txt"
static bool est_notre_dll(const std::wstring& chemin) {
    std::ifstream f(chemin, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.find("jsr60-log.txt") != std::string::npos;
}

static bool jeu_lance() {
    HWND w = FindWindowW(nullptr, L"Jet Set Radio");
    return w != nullptr;
}

static void relancer_en_admin() {
    wchar_t moi[MAX_PATH]; GetModuleFileNameW(nullptr, moi, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)}; sei.lpVerb = L"runas"; sei.lpFile = moi; sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 1. trouver le jeu : dossier de l'installeur, puis bibliotheques Steam, sinon choix manuel
    std::wstring jeu;
    wchar_t moi[MAX_PATH]; GetModuleFileNameW(nullptr, moi, MAX_PATH);
    std::wstring ici = moi; ici = ici.substr(0, ici.find_last_of(L'\\'));
    if (dossier_du_jeu(ici)) jeu = ici;
    if (jeu.empty()) for (const auto& b : bibliotheques_steam()) {
        const std::wstring d = b + L"\\steamapps\\common\\Jet Set Radio";
        if (dossier_du_jeu(d)) { jeu = d; break; }
    }
    if (jeu.empty()) {
        MessageBoxW(nullptr, L"Jet Set Radio n'a pas été trouvé automatiquement.\nIndiquez son dossier dans la fenêtre suivante.", kTitre, MB_ICONINFORMATION);
        jeu = choisir_dossier(nullptr);
        if (jeu.empty()) return 0;
        if (!dossier_du_jeu(jeu)) { MessageBoxW(nullptr, L"Ce dossier ne contient pas jetsetradio.exe.", kTitre, MB_ICONERROR); return 1; }
    }
    if (jeu_lance()) { MessageBoxW(nullptr, L"Fermez Jet Set Radio avant d'installer ou de désinstaller le mod.", kTitre, MB_ICONWARNING); return 1; }

    const std::wstring dll = jeu + L"\\d3d9.dll", lisezmoi = jeu + L"\\JSR60_LISEZMOI.txt", sauvegarde = jeu + L"\\" + kSauvegarde;
    const bool deja = existe(dll) && est_notre_dll(dll);

    // 2. choix de l'action
    int action = IDYES;
    if (deja) {
        std::wstring q = L"Le mod est déjà installé dans :\n" + jeu +
            L"\n\nOui : le mettre à jour (réinstaller)\nNon : le désinstaller\nAnnuler : ne rien faire";
        action = MessageBoxW(nullptr, q.c_str(), kTitre, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (action == IDCANCEL) return 0;
    } else {
        std::wstring q = L"Installer Jet Set Radio 60 FPS dans :\n" + jeu + L" ?";
        if (MessageBoxW(nullptr, q.c_str(), kTitre, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
    }

    DWORD err = 0;
    if (action == IDNO) {   // desinstallation
        if (!DeleteFileW(dll.c_str())) {
            err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) { relancer_en_admin(); return 0; }
            MessageBoxW(nullptr, L"Impossible de supprimer d3d9.dll.", kTitre, MB_ICONERROR); return 1;
        }
        DeleteFileW(lisezmoi.c_str());
        if (existe(sauvegarde)) MoveFileW(sauvegarde.c_str(), dll.c_str());   // remet l'ancienne d3d9.dll d'un autre mod
        MessageBoxW(nullptr, L"Le mod est désinstallé. Le jeu est revenu à l'origine.", kTitre, MB_ICONINFORMATION);
        return 0;
    }

    // installation / mise a jour : une d3d9.dll d'un autre programme est mise de cote
    if (existe(dll) && !deja && !existe(sauvegarde)) {
        if (!MoveFileW(dll.c_str(), sauvegarde.c_str())) {
            if (GetLastError() == ERROR_ACCESS_DENIED) { relancer_en_admin(); return 0; }
            MessageBoxW(nullptr, L"Une autre d3d9.dll est présente et n'a pas pu être mise de côté.", kTitre, MB_ICONERROR); return 1;
        }
    }
    if (!ecrire(dll, ressource(IDR_DLL), err)) {
        if (err == ERROR_ACCESS_DENIED) { relancer_en_admin(); return 0; }
        MessageBoxW(nullptr, L"Impossible d'écrire d3d9.dll dans le dossier du jeu.", kTitre, MB_ICONERROR); return 1;
    }
    ecrire(lisezmoi, ressource(IDR_LISEZMOI), err);
    MessageBoxW(nullptr,
        L"Installation terminée.\n\n"
        L"Lancez le jeu normalement depuis Steam.\n\n"
        L"Touches en jeu :\n"
        L"  F1 : flou de mouvement coupé\n"
        L"  F2 : flou léger\n"
        L"  F3 : flou intermédiaire\n"
        L"  F4 : flou fort (par défaut)\n"
        L"  F8 : couper / remettre le mod (jeu d'origine)\n\n"
        L"Pour désinstaller : relancez cet installeur et choisissez Non.",
        kTitre, MB_ICONINFORMATION);
    return 0;
}
