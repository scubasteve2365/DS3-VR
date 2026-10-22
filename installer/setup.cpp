// ds3vr-patcher.exe: run by install-ds3vr.bat / uninstall-ds3vr.bat in the game's Game folder.
// geo-11 and Helifax's Dark Souls 3 fix cannot be bundled with DS3 VR, so this adapts the copy the
// player downloaded: unpacks Helifax's archive if needed, checks it, backs up every file it changes,
// switches geo-11 to VR output and patches five of its shaders (all checked in memory first; nothing
// is written if any step fails), and sets the game's graphics options for VR. Uninstall restores it.
//   ds3vr-patcher.exe install   <Game folder>
//   ds3vr-patcher.exe uninstall <Game folder>

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <urlmon.h>
#include <wininet.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "../src/version.h"
#include "patch.h"
#include "sevenzip.h"
#include "shader_inserts.h"

using namespace setup;

namespace {

// Helifax's fix as tested with this release (21 Oct 2023 update): geo-11's d3d11.dll.
const char* kTestedGeo11Sha256 = "50bb7df2414920f6950d6d15519974a98be3865da17727ec58e14a3a0f03e62f";
const wchar_t* kBackupDir = L"ds3vr_backup";
// Files the installer changes (relative to Game). The shaders come from shader_inserts.h.
std::vector<std::wstring> ChangedFiles() {
    std::vector<std::wstring> v = {L"d3dx.ini", L"d3dxdm.ini"};
    for (const ShaderInsert& s : kShaderInserts) {
        bool have = false;
        for (auto& f : v) have |= f == s.file;
        if (!have) v.push_back(s.file);
    }
    return v;
}

void Say(const std::string& s) { std::cout << s << std::endl; }

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool ReadFileAll(const std::wstring& p, std::string& out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool WriteFileAll(const std::wstring& p, const std::string& data) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

std::string Sha256(const std::wstring& path) {
    std::string data;
    if (!ReadFileAll(path, data)) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32] = {};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(h, reinterpret_cast<PUCHAR>(&data[0]), static_cast<ULONG>(data.size()), 0);
            BCryptFinishHash(h, digest, sizeof(digest), 0);
            BCryptDestroyHash(h);
            char b[3];
            for (unsigned char c : digest) {
                snprintf(b, sizeof(b), "%02x", c);
                hex += b;
            }
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

// ---------- finding the game ----------
std::wstring ReadSteamPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) ==
        ERROR_SUCCESS)
        return buf;
    return L"";
}

std::wstring FindGameFolder() {
    const std::wstring steam = ReadSteamPath();
    if (steam.empty()) return L"";
    std::vector<std::wstring> libs = {steam};
    std::string vdf;
    if (ReadFileAll(steam + L"\\steamapps\\libraryfolders.vdf", vdf)) {
        size_t p = 0;
        while ((p = vdf.find("\"path\"", p)) != std::string::npos) {
            const size_t a = vdf.find('"', p + 6);
            const size_t b = a == std::string::npos ? a : vdf.find('"', a + 1);
            if (b == std::string::npos) break;
            std::string path = vdf.substr(a + 1, b - a - 1);
            std::string clean;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\') ++i;
                clean += path[i];
            }
            libs.push_back(Widen(clean));
            p = b + 1;
        }
    }
    for (auto& lib : libs) {
        std::wstring g = lib + L"\\steamapps\\common\\DARK SOULS III\\Game";
        for (auto& c : g)
            if (c == L'/') c = L'\\';
        if (Exists(g + L"\\DarkSoulsIII.exe")) return g;
    }
    return L"";
}

std::wstring BrowseForGame() {
    Say("Please pick the folder that contains DarkSoulsIII.exe (DARK SOULS III\\Game).");
    BROWSEINFOW bi{};
    bi.lpszTitle = L"Select the DARK SOULS III \\Game folder (the one with DarkSoulsIII.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return L"";
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(id, path);
    CoTaskMemFree(id);
    std::wstring g = path;
    if (Exists(g + L"\\Game\\DarkSoulsIII.exe")) g += L"\\Game";
    return Exists(g + L"\\DarkSoulsIII.exe") ? g : L"";
}

// ---------- helpers ----------
bool AskYes(const std::string& q, bool def) {
    std::cout << q << (def ? " [Y/n] " : " [y/N] ") << std::flush;
    std::string a;
    std::getline(std::cin, a);
    if (a.empty()) return def;
    return a[0] == 'y' || a[0] == 'Y';
}

std::wstring SetupDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool IsOurDll(const std::wstring& path) {
    std::string d;
    return ReadFileAll(path, d) && d.find("ds3vr milestone") != std::string::npos;
}

void MakeDirs(const std::wstring& path) {
    size_t p = 0;
    while ((p = path.find(L'\\', p + 1)) != std::wstring::npos) CreateDirectoryW(path.substr(0, p).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

// ---------- the game's own graphics settings (%APPDATA%\\DarkSoulsIII\\GraphicsConfig.xml, UTF-16) ----------
std::wstring GraphicsConfigPath() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = p;
    if (p) CoTaskMemFree(p);
    return d.empty() ? d : d + L"\\DarkSoulsIII\\GraphicsConfig.xml";
}

// UTF-16LE bytes (with or without BOM) to UTF-8; remembers the BOM.
bool Utf16ToUtf8(const std::string& raw, std::string& out, bool& bom) {
    bom = raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE;
    const size_t start = bom ? 2 : 0;
    if ((raw.size() - start) % 2) return false;
    std::wstring w((raw.size() - start) / 2, L'\0');
    if (!w.empty()) memcpy(&w[0], raw.data() + start, w.size() * 2);
    out = Narrow(w);
    return true;
}
std::string Utf8ToUtf16(const std::string& s, bool bom) {
    const std::wstring w = Widen(s);
    std::string out = bom ? std::string("\xFF\xFE") : std::string();
    out.append(reinterpret_cast<const char*>(w.data()), w.size() * 2);
    return out;
}

// ---------- uninstall ----------
void RemoveModFiles(const std::wstring& game, bool askSettings) {
    const std::wstring dll = game + L"\\dinput8.dll";
    if (IsOurDll(dll)) DeleteFileW(dll.c_str());
    bool keep = askSettings ? AskYes("Keep your DS3 VR settings (ds3vr.ini) for next time?", true) : true;
    if (!keep) DeleteFileW((game + L"\\ds3vr.ini").c_str());
    DeleteFileW((game + L"\\ds3vr.log").c_str());
    DeleteFileW((game + L"\\ds3vr_prev.log").c_str());
    WIN32_FIND_DATAW fd;
    for (const wchar_t* pat : {L"\\ds3vr_snap_*.bmp", L"\\ds3vr_draws_*.txt"}) {
        HANDLE h = FindFirstFileW((game + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do DeleteFileW((game + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// removeMod: also delete dinput8.dll, logs and (if the player agrees) ds3vr.ini. False when an install
// rolls back or a reinstall starts over: then only Helifax's files and the graphics settings come back.
bool Uninstall(const std::wstring& game, bool askSettings, bool removeMod) {
    const std::wstring bdir = game + L"\\" + kBackupDir;
    if (!Exists(bdir)) {
        if (!removeMod) return true;
        Say("DS3 VR was not set up in this folder, so none of Helifax's files need restoring.");
        RemoveModFiles(game, askSettings);
        return true;
    }
    int restored = 0, failed = 0;
    for (auto& rel : ChangedFiles()) {
        const std::wstring b = bdir + L"\\" + rel, g = game + L"\\" + rel;
        if (!Exists(b)) continue;
        if (CopyFileW(b.c_str(), g.c_str(), FALSE)) ++restored;
        else ++failed;
        // geo-11 keeps a compiled copy of each shader (.bin); restore it too so it matches.
        const std::wstring bb = b.substr(0, b.size() - 4) + L".bin", gb = g.substr(0, g.size() - 4) + L".bin";
        if (rel.size() > 4 && rel.substr(rel.size() - 4) == L".txt" && Exists(bb)) CopyFileW(bb.c_str(), gb.c_str(), FALSE);
    }
    if (failed) {
        Say("Could not restore " + std::to_string(failed) + " file(s). Close the game and run Uninstall again.");
        return false;
    }
    const std::wstring gfx = GraphicsConfigPath(), gfxBackup = bdir + L"\\GraphicsConfig.xml";
    if (!gfx.empty() && Exists(gfxBackup)) {
        if (!askSettings || AskYes("Put the game's graphics settings back to how they were before DS3 VR?", true))
            CopyFileW(gfxBackup.c_str(), gfx.c_str(), FALSE);
        DeleteFileW(gfxBackup.c_str());
    }
    if (removeMod) RemoveModFiles(game, askSettings);
    // Remove the backup folder (files first, then the directories).
    for (auto& rel : ChangedFiles()) {
        DeleteFileW((bdir + L"\\" + rel).c_str());
        if (rel.size() > 4) DeleteFileW((bdir + L"\\" + rel.substr(0, rel.size() - 4) + L".bin").c_str());
    }
    DeleteFileW((bdir + L"\\dinput8.dll").c_str());
    RemoveDirectoryW((bdir + L"\\ShaderFixesDM").c_str());
    RemoveDirectoryW(bdir.c_str());
    Say("Restored " + std::to_string(restored) + " of Helifax's file(s); his fix is back to how it was.");
    return true;
}

// ---------- Helifax's fix: find and unpack the archive if needed ----------
const wchar_t* kFixFiles[] = {L"d3d11.dll", L"d3dx.ini", L"d3dxdm.ini"};
const wchar_t* kOuterName = L"Dark_Souls_3_geo11_fix.7z";
const wchar_t* kInnerName = L"FixFiles.7z";

std::wstring MissingFixFile(const std::wstring& game) {
    for (const wchar_t* f : kFixFiles)
        if (!Exists(game + L"\\" + f)) return f;
    return L"";
}

std::wstring DownloadsDir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) d = p;
    if (p) CoTaskMemFree(p);
    return d;
}

std::wstring FirstExisting(const std::vector<std::wstring>& dirs, const wchar_t* name) {
    for (auto& d : dirs)
        if (!d.empty() && Exists(d + L"\\" + name)) return d + L"\\" + name;
    return L"";
}

bool Unpack(const std::wstring& archive, const std::wstring& dest) {
    Say("Extracting " + Narrow(archive) + " ...");
    std::string err;
    int n = 0;
    if (!Extract7z(archive, dest, err, &n)) {
        Say("Could not extract it: " + err + ".");
        Say("Close the game if it is running, or extract the archive yourself with 7-Zip, then run setup again.");
        return false;
    }
    Say("  " + std::to_string(n) + " file(s) extracted.");
    return true;
}

// Helifax's download is a 7z (Dark_Souls_3_geo11_fix.7z) that holds a second 7z (FixFiles.7z)
// plus LICENSE.txt. The fix itself is inside FixFiles.7z. Handle either one being found.
// Where Helifax's fix can be fetched from (masterotaku's server, linked from Helifax's HelixMod page).
// Each download is checked before it is used, so a wrong package (the 3D Vision version) or a changed
// file is never installed: it must contain geo-11 (d3d11.dll, d3dx.ini and d3dxdm.ini).
const wchar_t* kFixUrls[] = {
    L"https://s3.amazonaws.com/masterotaku/Dark+Souls+3/Dark_Souls_3_3D_Vision_fix.7z",
    L"https://s3.amazonaws.com/masterotaku/Dark+Souls+3/Dark_Souls_3_geo11_fix.7z",
};
const char* kTestedFixArchiveSha256 = "2ca06af593da735876a4471516f54c6e96c8e2fc023d8c5973e90bd93df99a8d";  // Dark_Souls_3_geo11_fix.7z, 21 Oct 2023

bool HasGeo11(const std::wstring& dir) {
    return Exists(dir + L"\\d3d11.dll") && Exists(dir + L"\\d3dx.ini") && Exists(dir + L"\\d3dxdm.ini");
}

bool UnpackQuiet(const std::wstring& archive, const std::wstring& dest) {
    std::string err;
    return Extract7z(archive, dest, err, nullptr);
}

// Downloads Helifax's fix and checks it. On success the archive is in the Game folder under the name
// the rest of the install expects (Dark_Souls_3_geo11_fix.7z, or FixFiles.7z when the download is the
// inner archive itself), and true is returned.
bool DownloadFix(const std::wstring& game) {
    const std::wstring work = game + L"\\ds3vr\\download";
    std::error_code ec;
    std::vector<std::wstring> urls;
    wchar_t env[2048] = {};
    if (GetEnvironmentVariableW(L"DS3VR_FIX_URL", env, 2048) > 0) urls.push_back(env);  // for testing another source
    for (const wchar_t* u : kFixUrls) urls.push_back(u);
    for (const std::wstring& u : urls) {
        const wchar_t* url = u.c_str();
        std::filesystem::remove_all(work, ec);
        MakeDirs(work + L"\\check");
        const std::wstring file = work + L"\\fix.7z";
        Say("Downloading " + Narrow(url) + " ...");
        DeleteUrlCacheEntryW(url);
        if (FAILED(URLDownloadToFileW(nullptr, url, file.c_str(), 0, nullptr)) || !Exists(file)) {
            Say("  could not download it.");
            continue;
        }
        const bool tested = Sha256(file) == kTestedFixArchiveSha256;
        const std::wstring check = work + L"\\check";
        bool ok = false, nested = false;
        if (UnpackQuiet(file, check)) {
            if (Exists(check + L"\\" + kInnerName)) {
                nested = true;
                ok = UnpackQuiet(check + L"\\" + kInnerName, check + L"\\fix") && HasGeo11(check + L"\\fix");
            } else {
                ok = HasGeo11(check);
            }
        }
        if (!ok) {
            Say("  that file is not the geo-11 version of the fix (perhaps the 3D Vision one), so it was not used.");
            continue;
        }
        const std::wstring dest = game + L"\\" + (nested ? kOuterName : kInnerName);
        if (!CopyFileW(file.c_str(), dest.c_str(), FALSE)) {
            Say("  could not save it in the Game folder.");
            continue;
        }
        std::filesystem::remove_all(work, ec);
        Say(tested ? "  OK: the geo-11 fix, the exact version DS3 VR was tested with."
                   : "  OK: the geo-11 fix (a different version from the one DS3 VR was tested with).");
        return true;
    }
    std::filesystem::remove_all(work, ec);
    return false;
}

bool EnsureFix(const std::wstring& game) {
    std::wstring missing = MissingFixFile(game);
    if (missing.empty()) return true;
    const std::wstring parent = game.substr(0, game.find_last_of(L'\\'));
    const std::wstring sub = game + L"\\Dark_Souls_3_geo11_fix";
    std::wstring inner = FirstExisting({game, sub}, kInnerName);
    std::wstring outer = inner.empty() ? FirstExisting({game, parent, SetupDir(), DownloadsDir()}, kOuterName) : L"";
    if (inner.empty() && outer.empty()) {
        Say("Helifax's Dark Souls 3 geo-11 fix is not in this Game folder yet.");
        Say("DS3 VR can download it for you from masterotaku's server (linked from Helifax's HelixMod page),");
        Say("about 6 MB. It is checked before it is used.");
        if (AskYes("Download it now?", true) && DownloadFix(game)) {
            inner = FirstExisting({game}, kInnerName);
            outer = inner.empty() ? FirstExisting({game}, kOuterName) : L"";
        }
    }
    if (inner.empty() && outer.empty()) {
        Say("");
        Say("Please download it yourself: open https://helixmod.blogspot.com/2016/04/dark-souls-3-3d-vision-fix.html,");
        Say("download Dark_Souls_3_geo11_fix.7z (the geo-11 version, not the 3D Vision one), put it in the Game");
        Say("folder or your Downloads folder, then run install-ds3vr.bat again.");
        return false;
    }
    Say("Helifax's geo-11 fix is ready to unpack:");
    Say("  " + Narrow(inner.empty() ? outer : inner));
    if (!inner.empty()) Say("(FixFiles.7z is the archive inside Helifax's download that holds the actual fix.)");
    if (!AskYes("Unpack it into the Game folder now?", true)) {
        Say("Then extract FixFiles.7z into the Game folder yourself and run this setup again.");
        return false;
    }
    if (inner.empty()) {
        // The outer archive holds FixFiles.7z and LICENSE.txt; put both in the Game folder.
        if (!Unpack(outer, game)) return false;
        inner = game + L"\\" + kInnerName;
        if (!Exists(inner)) {
            Say("That archive did not contain FixFiles.7z, so it is not the Helifax fix this setup expects.");
            return false;
        }
    }
    if (!Unpack(inner, game)) return false;
    missing = MissingFixFile(game);
    if (!missing.empty()) {
        Say("The fix was unpacked but " + Narrow(missing) + " is still missing. Please check the download.");
        return false;
    }
    Say("Helifax's fix is unpacked.");
    return true;
}

// ---------- install ----------
bool Install(const std::wstring& game) {
    const std::wstring files = game + L"\\ds3vr";
    const std::wstring dll = game + L"\\dinput8.dll";
    if (!IsOurDll(dll)) {
        Say("dinput8.dll from the DS3 VR zip is not in this folder. Extract the whole zip into the Game folder");
        Say("(the folder with DarkSoulsIII.exe), then run install-ds3vr.bat again.");
        return false;
    }
    // 1. Helifax's fix present? If not, look for its archive and offer to unpack it.
    if (!EnsureFix(game)) return false;
    const std::string sha = Sha256(game + L"\\d3d11.dll");
    if (sha != kTestedGeo11Sha256) {
        Say("Your geo-11 (d3d11.dll) is a different build from the one DS3 VR " DS3VR_VERSION " was tested with.");
        Say("It may still work. The patches are checked one by one and nothing is changed if any of them do not fit.");
        if (!AskYes("Continue anyway?", false)) return false;
    }
    // 2. Already installed? Start again from the originals.
    const std::wstring bdir = game + L"\\" + kBackupDir;
    if (Exists(bdir)) {
        Say("DS3 VR is already installed here. Reinstalling: restoring the original files first.");
        if (!Uninstall(game, false, false)) return false;
    }
    // 3. Prepare every change in memory.
    struct Pending {
        std::wstring rel;
        TextFile text;
    };
    std::vector<Pending> pend;
    std::vector<std::string> problems;
    for (auto& rel : ChangedFiles()) {
        std::string raw;
        if (!ReadFileAll(game + L"\\" + rel, raw)) {
            problems.push_back(Narrow(rel) + " is missing (is Helifax's fix fully extracted?)");
            continue;
        }
        Pending p{rel, {}};
        p.text.Load(raw);
        if (rel == L"d3dx.ini" && HasDs3vrMarks(p.text)) {
            problems.push_back(Narrow(rel) + " already contains DS3 VR changes but there is no backup to start from. "
                               "Re-extract Helifax's fix over the Game folder, then run setup again.");
            continue;
        }
        if (rel == L"d3dx.ini") PatchD3dx(p.text, problems);
        else if (rel == L"d3dxdm.ini") PatchD3dxdm(p.text, problems);
        else
            for (const ShaderInsert& s : kShaderInserts)
                if (rel == s.file && !InsertAfterLine(p.text, s.after, s.text))
                    problems.push_back(Narrow(rel) + ": expected line not found (\"" + s.after + "\")");
        pend.push_back(std::move(p));
    }
    if (!problems.empty()) {
        Say("Nothing was changed, because:");
        for (auto& p : problems) Say("  - " + p);
        return false;
    }
    // The game's graphics settings: windowed, no motion blur / depth of field / ambient occlusion / antialiasing.
    // A convenience, so a problem here never stops the install.
    const std::wstring gfx = GraphicsConfigPath();
    std::string gfxOld, gfxNew;
    bool gfxBom = false, gfxHad = false;
    if (!gfx.empty()) {
        std::string raw, xml;
        if (ReadFileAll(gfx, raw)) {
            std::vector<std::string> gp;
            if (Utf16ToUtf8(raw, xml, gfxBom) && PatchGraphicsConfig(xml, gp)) {
                gfxOld = raw;
                gfxHad = true;
                gfxNew = Utf8ToUtf16(xml, gfxBom);
            } else {
                Say("Note: could not read the game's graphics settings, so they were left alone. Set them by hand (see README).");
            }
        } else {
            gfxNew = Utf8ToUtf16(DefaultGraphicsConfig(), false);
        }
    }
    // 4. Back up.
    MakeDirs(bdir + L"\\ShaderFixesDM");
    for (auto& p : pend) {
        const std::wstring g = game + L"\\" + p.rel, b = bdir + L"\\" + p.rel;
        if (!CopyFileW(g.c_str(), b.c_str(), FALSE)) {
            Say("Could not back up " + Narrow(p.rel) + ". Nothing was changed.");
            return false;
        }
        const std::wstring gb = g.substr(0, g.size() - 4) + L".bin";
        if (p.rel.size() > 4 && p.rel.substr(p.rel.size() - 4) == L".txt" && Exists(gb))
            CopyFileW(gb.c_str(), (b.substr(0, b.size() - 4) + L".bin").c_str(), FALSE);
    }
    // 5. Write.
    for (auto& p : pend)
        if (!WriteFileAll(game + L"\\" + p.rel, p.text.Save())) {
            Say("Could not write " + Narrow(p.rel) + " (is the game running?). Restoring the originals.");
            Uninstall(game, false, false);
            return false;
        }
    if (!gfxNew.empty() && gfxNew != gfxOld) {
        if (gfxHad) WriteFileAll(bdir + L"\\GraphicsConfig.xml", gfxOld);
        MakeDirs(gfx.substr(0, gfx.find_last_of(L'\\')));
        if (WriteFileAll(gfx, gfxNew))
            Say("Set the game's graphics for VR: windowed, motion blur, depth of field, ambient occlusion and antialiasing off.");
        else
            Say("Note: could not change the game's graphics settings. Set them by hand (see README).");
    } else if (gfxHad) {
        Say("The game's graphics settings are already right for VR.");
    }
    const std::wstring ini = game + L"\\ds3vr.ini";
    if (!Exists(ini)) CopyFileW((files + L"\\ds3vr.default.ini").c_str(), ini.c_str(), FALSE);
    else Say("Kept your existing ds3vr.ini settings.");
    // 6. Verify.
    int bad = 0;
    for (auto& p : pend) {
        std::string raw;
        if (!ReadFileAll(game + L"\\" + p.rel, raw) || raw != p.text.Save()) ++bad;
    }
    if (bad || !IsOurDll(dll)) {
        Say("Verification failed. Restoring the originals.");
        Uninstall(game, false, false);
        return false;
    }
    Say("");
    Say("Installed DS3 VR " DS3VR_VERSION ". Helifax's original files are saved in ds3vr_backup.");
    Say("Start your VR runtime (for example SteamVR), then launch Dark Souls III from Steam as usual.");
    Say("In game: click both thumbsticks together (or press Insert) for the VR menu.");
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring verb = argc > 1 ? argv[1] : L"";
    if (argc < 3 || (verb != L"install" && verb != L"uninstall" && verb != L"check-download")) {
        Say("DS3 VR " DS3VR_VERSION ": run install-ds3vr.bat or uninstall-ds3vr.bat in the game's Game folder.");
        return 2;
    }
    wchar_t full[MAX_PATH * 2] = {};
    GetFullPathNameW(argv[2], MAX_PATH * 2, full, nullptr);
    std::wstring game = full;
    while (!game.empty() && (game.back() == L'\\' || game.back() == L'/')) game.pop_back();
    Say("DS3 VR " DS3VR_VERSION ": Dark Souls III in VR");
    Say("");
    if (!Exists(game + L"\\DarkSoulsIII.exe")) {
        Say("DarkSoulsIII.exe is not in " + Narrow(game) + ".");
        Say("Extract the DS3 VR zip into the game's Game folder, for example");
        Say("  C:\\Program Files (x86)\\Steam\\steamapps\\common\\DARK SOULS III\\Game");
        return 1;
    }
    Say("Game folder: " + Narrow(game));
    Say("");
    if (verb == L"check-download") {
        // Testing aid: download and check Helifax's fix only (ignores any copy already on the PC).
        const bool ok = DownloadFix(game);
        Say(ok ? "Download check passed." : "Download check failed: no usable geo-11 fix was downloaded.");
        return ok ? 0 : 1;
    }
    if (verb == L"install") {
        Say("Why this step: DS3 VR needs geo-11 and Helifax's Dark Souls 3 fix, which cannot be included in");
        Say("the DS3 VR download. This step adapts the copy you downloaded for VR. Every file it changes is");
        Say("backed up first, and uninstall-ds3vr.bat puts them back.");
        Say("");
        return Install(game) ? 0 : 1;
    }
    return Uninstall(game, true, true) ? 0 : 1;
}
