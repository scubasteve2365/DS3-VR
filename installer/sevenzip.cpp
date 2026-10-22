#include "sevenzip.h"

#include <windows.h>

#include <cstdio>
#include <vector>

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
}

namespace setup {
namespace {

void MakeDirs(const std::wstring& path) {
    for (size_t p = path.find(L'\\', 3); p != std::wstring::npos; p = path.find(L'\\', p + 1))
        CreateDirectoryW(path.substr(0, p).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

} // namespace

bool Extract7z(const std::wstring& archive, const std::wstring& destDir, std::string& error, int* files) {
    if (files) *files = 0;
    ISzAlloc allocImp = {SzAlloc, SzFree}, allocTemp = {SzAllocTemp, SzFreeTemp};
    CFileInStream in;
    if (InFile_OpenW(&in.file, archive.c_str()) != 0) {
        error = "cannot open the archive";
        return false;
    }
    FileInStream_CreateVTable(&in);
    in.wres = 0;
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    const size_t kBuf = 1 << 18;
    look.buf = static_cast<Byte*>(ISzAlloc_Alloc(&allocImp, kBuf));
    look.bufSize = kBuf;
    look.realStream = &in.vt;
    LookToRead2_INIT(&look)
    CrcGenerateTable();
    CSzArEx db;
    SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &look.vt, &allocImp, &allocTemp);
    if (res != SZ_OK) {
        error = "not a readable 7z archive (error " + std::to_string(res) + ")";
    } else {
        UInt32 blockIndex = 0xFFFFFFFF;
        Byte* outBuffer = nullptr;
        size_t outBufferSize = 0;
        std::vector<UInt16> name;
        for (UInt32 i = 0; i < db.NumFiles && res == SZ_OK; ++i) {
            const size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
            name.assign(len + 1, 0);
            SzArEx_GetFileNameUtf16(&db, i, name.data());
            std::wstring rel;
            for (size_t k = 0; k < len && name[k]; ++k) rel += (name[k] == L'/') ? L'\\' : static_cast<wchar_t>(name[k]);
            if (rel.find(L"..") != std::wstring::npos || (rel.size() > 1 && rel[1] == L':')) {
                error = "unsafe path in archive";
                res = SZ_ERROR_FAIL;
                break;
            }
            const std::wstring out = destDir + L"\\" + rel;
            if (SzArEx_IsDir(&db, i)) {
                MakeDirs(out);
                continue;
            }
            const size_t slash = out.find_last_of(L'\\');
            if (slash != std::wstring::npos) MakeDirs(out.substr(0, slash));
            size_t offset = 0, size = 0;
            res = SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize, &offset, &size, &allocImp, &allocTemp);
            if (res != SZ_OK) {
                error = "could not decompress a file (error " + std::to_string(res) + ")";
                break;
            }
            HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                error = "could not write a file (is the game running?)";
                res = SZ_ERROR_WRITE;
                break;
            }
            DWORD written = 0;
            const BOOL ok = size == 0 || WriteFile(h, outBuffer + offset, static_cast<DWORD>(size), &written, nullptr);
            if (SzBitWithVals_Check(&db.MTime, i)) {
                FILETIME ft;
                ft.dwLowDateTime = db.MTime.Vals[i].Low;
                ft.dwHighDateTime = db.MTime.Vals[i].High;
                SetFileTime(h, nullptr, nullptr, &ft);
            }
            CloseHandle(h);
            if (!ok || written != size) {
                error = "could not write a file (disk full?)";
                res = SZ_ERROR_WRITE;
                break;
            }
            if (files) ++*files;
        }
        ISzAlloc_Free(&allocImp, outBuffer);
    }
    SzArEx_Free(&db, &allocImp);
    ISzAlloc_Free(&allocImp, look.buf);
    File_Close(&in.file);
    return res == SZ_OK;
}

} // namespace setup
