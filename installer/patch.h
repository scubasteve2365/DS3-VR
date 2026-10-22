#pragma once
// Text patching for the DS3 VR installer. Portable (no Windows calls) so it can be tested on any OS.
// Every operation checks that its target is exactly where expected and fails rather than guess.

#include <string>
#include <vector>

namespace setup {

struct ShaderInsert {
    const wchar_t* file;  // relative to the game's Game folder
    const char* after;    // exact original line (must occur once)
    const char* text;     // lines to insert, each ending in \n
};

// A text file held with \n line ends; remembers whether it used \r\n so it is written back the same way.
struct TextFile {
    std::string body;
    bool crlf = false;
    void Load(const std::string& raw);
    std::string Save() const;
};

// "key = value" inside [section]: replaces the value. False if the section or key is missing.
bool IniSet(TextFile& f, const std::string& section, const std::string& key, const std::string& value);
// Turns every line of [section] (up to the next blank line or section) into a comment, after a note line.
bool IniCommentOutSection(TextFile& f, const std::string& section, const std::string& note);
// Adds lines at the end of [section] (after its last non-blank line).
bool IniAddToSection(TextFile& f, const std::string& section, const std::string& lines);
// Appends a block of text (sections) at the end of the file.
void AppendBlock(TextFile& f, const std::string& text);
// Inserts text after the one line equal to `after`. False if it occurs zero or several times.
bool InsertAfterLine(TextFile& f, const std::string& after, const std::string& text);

// All of DS3 VR's changes to Helifax's d3dx.ini / d3dxdm.ini. `problems` gets one line per failed step.
bool PatchD3dx(TextFile& f, std::vector<std::string>& problems);
bool PatchD3dxdm(TextFile& f, std::vector<std::string>& problems);

// DARK SOULS III's own GraphicsConfig.xml (as UTF-8 text): sets windowed mode and turns off motion blur,
// depth of field, ambient occlusion and antialiasing, with the quality preset on Custom so they stick.
// Adds any missing setting before </config>. False (with a problem line) if the file is not recognised.
bool PatchGraphicsConfig(std::string& xml, std::vector<std::string>& problems);
// A complete GraphicsConfig.xml (UTF-8, \r\n) with the DS3 VR settings, for when the game has none yet.
std::string DefaultGraphicsConfig();

// True if the file already carries DS3 VR changes.
bool HasDs3vrMarks(const TextFile& f);

} // namespace setup
