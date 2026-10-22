#include "patch.h"

#include <cctype>
#include <cstdio>

namespace setup {
namespace {

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> Lines(const std::string& body) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p <= body.size()) {
        size_t e = body.find('\n', p);
        if (e == std::string::npos) {
            v.push_back(body.substr(p));
            break;
        }
        v.push_back(body.substr(p, e - p));
        p = e + 1;
    }
    return v;
}

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += '\n';
        s += v[i];
    }
    return s;
}

bool IsHeader(const std::string& l) {
    const std::string t = Trim(l);
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}

// [first line after the header, one past the last line of the section)
bool FindSection(const std::vector<std::string>& v, const std::string& section, size_t& begin, size_t& end) {
    const std::string want = "[" + Lower(section) + "]";
    for (size_t i = 0; i < v.size(); ++i) {
        if (Lower(Trim(v[i])) != want) continue;
        begin = i + 1;
        end = begin;
        while (end < v.size() && !IsHeader(v[end])) ++end;
        return true;
    }
    return false;
}

void SplitLinesInto(const std::string& text, std::vector<std::string>& out) {
    std::string t = text;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    for (auto& l : Lines(t)) out.push_back(l);
}

} // namespace

void TextFile::Load(const std::string& raw) {
    crlf = raw.find("\r\n") != std::string::npos;
    body.clear();
    body.reserve(raw.size());
    for (char c : raw)
        if (c != '\r') body += c;
}

std::string TextFile::Save() const {
    if (!crlf) return body;
    std::string s;
    s.reserve(body.size() + body.size() / 20);
    for (char c : body) {
        if (c == '\n') s += '\r';
        s += c;
    }
    return s;
}

bool IniSet(TextFile& f, const std::string& section, const std::string& key, const std::string& value) {
    auto v = Lines(f.body);
    size_t b = 0, e = 0;
    if (!FindSection(v, section, b, e)) return false;
    for (size_t i = b; i < e; ++i) {
        const std::string t = Trim(v[i]);
        if (t.empty() || t[0] == ';') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos || Lower(Trim(t.substr(0, eq))) != Lower(key)) continue;
        const size_t raweq = v[i].find('=');
        size_t vs = raweq + 1;
        while (vs < v[i].size() && (v[i][vs] == ' ' || v[i][vs] == '\t')) ++vs;
        v[i] = v[i].substr(0, vs) + value;
        f.body = Join(v);
        return true;
    }
    return false;
}

bool IniCommentOutSection(TextFile& f, const std::string& section, const std::string& note) {
    auto v = Lines(f.body);
    size_t b = 0, e = 0;
    if (!FindSection(v, section, b, e)) return false;
    size_t last = b;
    while (last < e && !Trim(v[last]).empty()) ++last;  // up to the first blank line
    for (size_t i = b; i < last; ++i) v[i] = ";" + v[i];
    v[b - 1] = ";" + v[b - 1];
    v.insert(v.begin() + static_cast<long>(b - 1), note);
    f.body = Join(v);
    return true;
}

bool IniAddToSection(TextFile& f, const std::string& section, const std::string& lines) {
    auto v = Lines(f.body);
    size_t b = 0, e = 0;
    if (!FindSection(v, section, b, e)) return false;
    size_t at = e;
    while (at > b && Trim(v[at - 1]).empty()) --at;
    std::vector<std::string> add;
    SplitLinesInto(lines, add);
    v.insert(v.begin() + static_cast<long>(at), add.begin(), add.end());
    f.body = Join(v);
    return true;
}

void AppendBlock(TextFile& f, const std::string& text) {
    if (!f.body.empty() && f.body.back() != '\n') f.body += '\n';
    f.body += '\n';
    f.body += text;
}

bool InsertAfterLine(TextFile& f, const std::string& after, const std::string& text) {
    auto v = Lines(f.body);
    size_t found = v.size();
    int count = 0;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == after) {
            found = i;
            ++count;
        }
    if (count != 1) return false;
    std::vector<std::string> add;
    SplitLinesInto(text, add);
    v.insert(v.begin() + static_cast<long>(found + 1), add.begin(), add.end());
    f.body = Join(v);
    return true;
}

bool HasDs3vrMarks(const TextFile& f) { return f.body.find("ds3vr") != std::string::npos; }

bool PatchD3dx(TextFile& f, std::vector<std::string>& problems) {
    const size_t before = problems.size();
    // Bloom off by default (its glow does not line up between the eyes in VR); F4 still toggles it.
    std::string body = f.body;
    {
        auto v = Lines(f.body);
        size_t b = 0, e = 0;
        bool ok = false;
        if (FindSection(v, "Constants", b, e))
            for (size_t i = b; i < e && !ok; ++i)
                if (Trim(v[i]) == "x1=1") {
                    v.insert(v.begin() + static_cast<long>(i),
                             "; ds3vr: off by default in VR (its glow does not line up between the eyes). F4 turns it back on.");
                    v[i + 1] = "x1=0";
                    ok = true;
                }
        if (ok) f.body = Join(v);
        else problems.push_back("d3dx.ini: bloom setting (x1=1 in [Constants]) not found");
    }
    // The fix's F1 convergence presets would fight the VR mod's world scale.
    if (!IniCommentOutSection(f, "Key1", "; ds3vr: F1 convergence presets switched off (the VR mod needs a fixed convergence)"))
        problems.push_back("d3dx.ini: [Key1] (F1 convergence presets) not found");
    // The sun-ray shader gets its render target so it can recognise it at any resolution.
    if (!IniAddToSection(f, "ShaderOverrideSunVS",
                         "; ds3vr: give the shader the sun-ray buffer so it can recognise it by shape at any resolution\n"
                         "vs-t100 = ps-t0\n"))
        problems.push_back("d3dx.ini: [ShaderOverrideSunVS] not found");
    // World scale steps (pressed by the mod menu) and the lock-on marker at any resolution.
    std::string ws =
        "; ds3vr: world scale 1.0 to 5.0 in steps of 0.5 (1.0 = life size). The mod menu presses F13 to F21\n"
        "; (keys real keyboards do not have) to pick a step; convergence = 1.4834 x scale for dm_separation 20.\n";
    for (int i = 0; i < 9; ++i) {
        char b[256];
        snprintf(b, sizeof(b),
                 "[KeyDs3vrWorldScale%d]\nKey = no_modifiers VK_F%d\ntype = activate\nconvergence = %.3f\ntransition = 300\n"
                 "transition_type = cosine\n\n",
                 i + 1, 13 + i, 1.4834 * (1.0 + 0.5 * i));
        ws += b;
    }
    ws += "; ds3vr: the lock-on marker's glow textures at any render size (the hash sections only match\n"
          "; 1080p). filter_index 2 lets the HUD shaders place the marker at the enemy's depth.\n"
          "[TextureOverrideDs3vrLockOn]\nmatch_type = Texture2D\nmatch_format = R8G8B8A8_UNORM\nmatch_bind_flags = +render_target\n"
          "match_width = height\nmatch_height = < 512\nfilter_index = 2\n";
    AppendBlock(f, ws);
    if (problems.size() != before) f.body = body;
    return problems.size() == before;
}

bool PatchD3dxdm(TextFile& f, std::vector<std::string>& problems) {
    const size_t before = problems.size();
    struct Kv {
        const char* key;
        const char* value;
    };
    static const Kv kv[] = {
        {"direct_mode", "katanga_vr"},             // shared stereo texture the VR mod picks up
        {"dm_convergence", "1.483"},               // world scale 1 (life size); the mod rewrites it to the saved scale
        {"dm_separation", "20"},                   // with the mod's eye shift, a headset-correct separation
        {"dm_auto_convergence", "0"},              // fixed: the mod computes it
        {"dm_max_convergence_soft", "10.0"},       // room for world scale up to 5
        {"dm_max_convergence_hard", "10.0"},
    };
    for (const Kv& k : kv)
        if (!IniSet(f, "Device", k.key, k.value) && !IniSet(f, "Constants", k.key, k.value) && !IniSet(f, "Stereo", k.key, k.value)) {
            // Not in a section we know: fall back to the first "key = value" line anywhere.
            auto v = Lines(f.body);
            bool done = false;
            for (auto& l : v) {
                const std::string t = Trim(l);
                const size_t eq = t.find('=');
                if (!t.empty() && t[0] != ';' && eq != std::string::npos && Lower(Trim(t.substr(0, eq))) == k.key) {
                    l = std::string(k.key) + " = " + k.value;
                    done = true;
                    break;
                }
            }
            if (done) f.body = Join(v);
            else problems.push_back(std::string("d3dxdm.ini: ") + k.key + " not found");
        }
    return problems.size() == before;
}

// ---------- DARK SOULS III GraphicsConfig.xml ----------
namespace {
struct Tag {
    const char* name;
    const char* value;
};
const Tag kGraphicsTags[] = {
    {"ScreenMode", "WINDOW"},                          // the mod sizes the window for the headset
    {"Auto-detectBestRenderingSettings", "OFF"},       // so the game does not replace the choices below
    {"QualitySetting", "CUSTOM"},                      // the presets would override the advanced settings
    {"Antialiasing", "OFF"},
    {"SSAO", "DISABLE"},
    {"DepthOfField", "DISABLE"},
    {"MotionBlur", "DISABLE"},
};
} // namespace

bool PatchGraphicsConfig(std::string& xml, std::vector<std::string>& problems) {
    const size_t end = xml.find("</config>");
    if (xml.find("<config>") == std::string::npos || end == std::string::npos) {
        problems.push_back("GraphicsConfig.xml: not a DARK SOULS III settings file (no <config>)");
        return false;
    }
    const bool crlf = xml.find("\r\n") != std::string::npos;
    for (const Tag& t : kGraphicsTags) {
        const std::string open = std::string("<") + t.name + ">", close = std::string("</") + t.name + ">";
        const size_t a = xml.find(open);
        const size_t b = a == std::string::npos ? a : xml.find(close, a);
        if (a != std::string::npos && b != std::string::npos) {
            xml.replace(a + open.size(), b - a - open.size(), t.value);
        } else {
            const size_t at = xml.find("</config>");
            xml.insert(at, open + t.value + close + (crlf ? "\r\n" : "\n"));
        }
    }
    return true;
}

std::string DefaultGraphicsConfig() {
    return "<?xml version=\"1.0\" encoding=\"UTF-16\" ?>\r\n"
           "<config><ScreenMode>WINDOW</ScreenMode>\r\n"
           "<Resolution-WindowScreenWidth>1920</Resolution-WindowScreenWidth>\r\n"
           "<Resolution-WindowScreenHeight>1080</Resolution-WindowScreenHeight>\r\n"
           "<Resolution-FullScreenWidth>1920</Resolution-FullScreenWidth>\r\n"
           "<Resolution-FullScreenHeight>1080</Resolution-FullScreenHeight>\r\n"
           "<Auto-detectBestRenderingSettings>OFF</Auto-detectBestRenderingSettings>\r\n"
           "<QualitySetting>CUSTOM</QualitySetting>\r\n"
           "<TextureQuality>HIGH</TextureQuality>\r\n"
           "<Antialiasing>OFF</Antialiasing>\r\n"
           "<SSAO>DISABLE</SSAO>\r\n"
           "<DepthOfField>DISABLE</DepthOfField>\r\n"
           "<MotionBlur>DISABLE</MotionBlur>\r\n"
           "<ShadowQuality>HIGH</ShadowQuality>\r\n"
           "<LightingQuality>HIGH</LightingQuality>\r\n"
           "<EffectsQuality>HIGH</EffectsQuality>\r\n"
           "<ReflectionQuality>HIGH</ReflectionQuality>\r\n"
           "<WaterSurfaceQuality>HIGH</WaterSurfaceQuality>\r\n"
           "<ShadeQuality>HIGH</ShadeQuality>\r\n"
           "</config>\r\n";
}

} // namespace setup
