// Renders the DS3 VR menu tabs for the README (tools/menushots/render.sh): the menu's own UI code
// (extracted from src/menu.cpp at build time) with stubbed mod state and a small software rasterizer.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "imgui.h"
#include "menu_font.h"

#define DS3VR_VERSION "2.0.0"
typedef unsigned int UINT;

namespace ds3vr {
enum DlssMode { kDlssOff = 0, kDlssDlaa, kDlssQuality, kDlssBalanced, kDlssPerformance, kDlssUltraPerformance, kDlssModeCount };
struct HudSettings { int mode; float width, distance, drop; };

// ---- stubbed state (what a typical session shows) ----
static float s_ws = 1.f; static int s_pitch = 0; static bool s_scaleHead = true;
static HudSettings s_hud{0, 1.0f, 1.2f, 0.10f};
static int s_dlssSaved = 1, s_dlssLive = 1, s_preset = 6, s_sharp = 1;
static bool g_dev = false;
static const char* kModeNames[] = {"Off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Perf"};

float WorldScaleGet() { return s_ws; }
void WorldScaleStep(int) {}
void WorldScaleSet(float) {}
int CamHookGetDecoupledPitch() { return s_pitch; }
void CamHookSetDecoupledPitch(int) {}
bool CamHookGetScaleHead() { return s_scaleHead; }
void CamHookSetScaleHead(bool) {}
HudSettings XrGetHudSettings() { return s_hud; }
void XrSetHudSettings(const HudSettings&, bool) {}
void XrRecenter() {}
int XrGetPoseLag() { return 2; }
void XrSetPoseLag(int) {}
bool XrRecommendedNow(unsigned* w, unsigned* h) { *w = 2064; *h = 2208; return true; }
bool XrEyeImageSize(unsigned* w, unsigned* h) { *w = 2240; *h = 2208; return true; }
bool RenderSizeGet(unsigned* w, unsigned* h) { *w = 2240; *h = 2208; return true; }
bool RenderSizeNextLaunch(unsigned* w, unsigned* h, bool* f) { *w = 2240; *h = 2208; *f = false; return true; }
int DlssModeSaved() { return s_dlssSaved; }
int DlssModeNow() { return s_dlssLive; }
int DlssAvailable() { return 1; }
void DlssSetMode(int) {}
const char* DlssStatus() { return s_dlssLive == 1 ? "DLAA running" : "off"; }
const char* DlssModeName(int m) { return kModeNames[m]; }
float DlssModeScale(int) { return 1.f; }
bool DlssUpscaling() { return false; }
bool DlssOutputSize(unsigned*, unsigned*) { return false; }
int DlaaPreset() { return s_preset; }
void DlaaSetPreset(int) {}
float DlaaJitterSign() { return 1.f; }
float DlaaEyeSign() { return 1.f; }
void DlaaSetSigns(float, float) {}
bool DlssPairPrevious() { return false; }
void DlssSetPairPrevious(bool) {}
bool DlssHalvesSwapped() { return false; }
void DlssSetHalvesSwapped(bool) {}
float DlssMipBias() { return 0.f; }
float DlssMipBiasOffset() { return -1.f; }
void DlssStepMipBiasOffset(int) {}
constexpr int kSharpenLevels = 5;
static const char* kSharpNames[kSharpenLevels] = {"Off", "Low", "Medium", "High", "Max"};
int SharpenLevel() { return s_sharp; }
void SharpenSetLevel(int) {}
const char* SharpenLevelName(int i) { return kSharpNames[i]; }
bool LogVerbose() { return g_dev; }
void Log(const char*, ...) {}

namespace {
static const ImVec4 kMenuAccent(0.85f, 0.68f, 0.32f, 1.f);
static const ImVec4 kMenuSelected(0.62f, 0.44f, 0.16f, 1.f);
constexpr UINT kW = 1280, kH = 1250;
std::atomic<unsigned> g_openGen{1};
#include "ui_extract.inc"
}  // namespace

void Setup() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouse;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.DisplaySize = ImVec2((float)kW, (float)kH);
    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kMenuFontData), (int)kMenuFontSize, 44.f, &fontCfg);
#include "style_extract.inc"
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);  // no focus ring in screenshots
}

// ---- software rasterizer for ImDrawData (straight alpha over a transparent target) ----
struct Img { int w, h; std::vector<float> px; };  // premultiplied RGBA
static unsigned char* g_font; static int g_fw, g_fh;

static void Raster(Img& im, const ImDrawData* dd) {
    for (int l = 0; l < dd->CmdListsCount; ++l) {
        const ImDrawList* cl = dd->CmdLists[l];
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const int cx0 = std::max(0, (int)cmd.ClipRect.x), cy0 = std::max(0, (int)cmd.ClipRect.y);
            const int cx1 = std::min(im.w, (int)cmd.ClipRect.z), cy1 = std::min(im.h, (int)cmd.ClipRect.w);
            for (unsigned i = 0; i < cmd.ElemCount; i += 3) {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k) v[k] = &cl->VtxBuffer[cmd.VtxOffset + cl->IdxBuffer[cmd.IdxOffset + i + k]];
                const float x0 = v[0]->pos.x, y0 = v[0]->pos.y, x1 = v[1]->pos.x, y1 = v[1]->pos.y, x2 = v[2]->pos.x, y2 = v[2]->pos.y;
                const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (std::fabs(area) < 1e-8f) continue;
                int bx0 = std::max(cx0, (int)std::floor(std::min({x0, x1, x2})));
                int by0 = std::max(cy0, (int)std::floor(std::min({y0, y1, y2})));
                int bx1 = std::min(cx1, (int)std::ceil(std::max({x0, x1, x2})));
                int by1 = std::min(cy1, (int)std::ceil(std::max({y0, y1, y2})));
                for (int y = by0; y < by1; ++y)
                    for (int x = bx0; x < bx1; ++x) {
                        float acc[4] = {0, 0, 0, 0};
                        int hits = 0;
                        {
                            {
                                const float px = x + 0.5f + 1.3e-4f, py = y + 0.5f + 2.7e-4f;
                                float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                                float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                                float w2 = 1.f - w0 - w1;
                                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                                const float u = w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x;
                                const float t = w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y;
                                const float fx = u * g_fw - 0.5f, fy = t * g_fh - 0.5f;
                                const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
                                const float ax = fx - ix, ay = fy - iy;
                                auto A = [&](int xx, int yy) {
                                    xx = std::clamp(xx, 0, g_fw - 1); yy = std::clamp(yy, 0, g_fh - 1);
                                    return g_font[(yy * g_fw + xx) * 4 + 3] / 255.f;
                                };
                                const float ta = (A(ix, iy) * (1 - ax) + A(ix + 1, iy) * ax) * (1 - ay) + (A(ix, iy + 1) * (1 - ax) + A(ix + 1, iy + 1) * ax) * ay;
                                float c[4];
                                for (int k = 0; k < 4; ++k)
                                    c[k] = ((v[0]->col >> (8 * k)) & 255) / 255.f * w0 + ((v[1]->col >> (8 * k)) & 255) / 255.f * w1 +
                                           ((v[2]->col >> (8 * k)) & 255) / 255.f * w2;
                                c[3] *= ta;
                                acc[0] = c[0] * c[3]; acc[1] = c[1] * c[3]; acc[2] = c[2] * c[3]; acc[3] = c[3];
                                hits = 1;
                            }
                        }
                        if (!hits) continue;
                                                float* d = &im.px[(y * im.w + x) * 4];
                        for (int k = 0; k < 4; ++k) d[k] = acc[k] + d[k] * (1.f - acc[3]);
                    }
            }
        }
    }
}

static void Frame(int tab, bool jump) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.f / 60.f;
    if (jump) { g_tab = tab; g_tabJump = true; }
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();
}

void Shoot(int tab, const char* path) {
    for (int f = 0; f < 20; ++f) Frame(tab, f == 3);
    Img im{(int)kW, (int)kH, std::vector<float>(kW * kH * 4, 0.f)};
    Raster(im, ImGui::GetDrawData());
    FILE* fp = fopen(path, "wb");
    for (float f : im.px) { unsigned char b = (unsigned char)std::lround(std::clamp(f, 0.f, 1.f) * 255.f); fwrite(&b, 1, 1, fp); }
    fclose(fp);
}
}  // namespace ds3vr

int main() {
    using namespace ds3vr;
    Setup();
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&g_font, &g_fw, &g_fh);
    ImGui::GetIO().Fonts->SetTexID((ImTextureID)1);
    Shoot(0, "general.rgba");
    Shoot(1, "graphics.rgba");
    g_dev = true;
    Shoot(2, "developer.rgba");
    return 0;
}
