// Linux test: apply the installer's patches to Helifax's original files and write the results.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include "patch.h"
#include "shader_inserts.h"

using namespace setup;

static std::string Read(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void Write(const std::string& p, const std::string& d) { std::ofstream f(p, std::ios::binary); f << d; }
static std::string Narrow(const wchar_t* w) { std::string s; for (; *w; ++w) s += (*w == L'\\') ? '/' : static_cast<char>(*w); return s; }

int main(int argc, char** argv) {
    const std::string src = argv[1], dst = argv[2];
    std::vector<std::string> problems;
    TextFile dx, dm;
    dx.Load(Read(src + "/d3dx.ini"));
    dm.Load(Read(src + "/d3dxdm.ini"));
    PatchD3dx(dx, problems);
    PatchD3dxdm(dm, problems);
    Write(dst + "/d3dx.ini", dx.Save());
    Write(dst + "/d3dxdm.ini", dm.Save());
    std::string last;
    TextFile sh;
    for (const ShaderInsert& s : kShaderInserts) {
        const std::string rel = Narrow(s.file);
        if (rel != last) {
            if (!last.empty()) Write(dst + "/" + last, sh.Save());
            sh.Load(Read(src + "/" + rel));
            last = rel;
        }
        if (!InsertAfterLine(sh, s.after, s.text)) problems.push_back(rel + ": anchor not found once: " + s.after);
    }
    Write(dst + "/" + last, sh.Save());
    for (auto& p : problems) std::cout << "PROBLEM: " << p << "\n";
    std::cout << (problems.empty() ? "all patches applied\n" : "FAILED\n");
    // Second run on patched files must be detected
    TextFile again; again.Load(Read(dst + "/d3dx.ini"));
    std::cout << "marks detected after patch: " << HasDs3vrMarks(again) << "\n";
    return problems.empty() ? 0 : 1;
}
