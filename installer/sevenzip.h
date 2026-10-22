#pragma once
// Extracts a .7z archive (LZMA, LZMA2, BCJ2, PPMd) with the public-domain 7-Zip C decoder.
#include <string>

namespace setup {

// Extracts every file (with folders) from `archive` into `destDir`, keeping the files' dates.
// Returns false and fills `error` on failure. `files` gets the number of files written.
bool Extract7z(const std::wstring& archive, const std::wstring& destDir, std::string& error, int* files = nullptr);

} // namespace setup
