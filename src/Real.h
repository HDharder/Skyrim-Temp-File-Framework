#pragma once

// Pointers to the ORIGINAL Windows file API functions.
//
// Before Hooks::Install they point at the API itself; afterwards, at the MinHook trampoline.
// All framework code that needs to see the disk "as it really is" - without OUR redirection, but
// STILL going through the MO2 VFS (usvfs) - calls through here.
//
// WARNING: calling ::CreateFileW directly from inside the framework goes through our own hook.
// It is not a bug (the reentrancy guard keeps it safe), but it is wasted work and confuses
// whoever reads the log.

// CopyFile2 only exists in the SDK with _WIN32_WINNT >= 0x0602 and CommonLib compiles with 0x0601,
// so we declare only the part of the parameters struct that we use.
struct TFF_CopyFile2Params {
    DWORD dwSize;
    DWORD dwCopyFlags;
};
using CopyFile2Fn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, TFF_CopyFile2Params*);
// Same for CreateFile2 - it is what MSVC's std::filesystem uses to open files (remove, file_size,
// equivalent, ...). The extended parameter is only passed through, never read.
using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, void*);

namespace Real {
    inline decltype(&::CreateFileW) CreateFileW = ::CreateFileW;
    inline decltype(&::CreateFileA) CreateFileA = ::CreateFileA;
    inline decltype(&::GetFileAttributesW) GetFileAttributesW = ::GetFileAttributesW;
    inline decltype(&::GetFileAttributesA) GetFileAttributesA = ::GetFileAttributesA;
    inline decltype(&::GetFileAttributesExW) GetFileAttributesExW = ::GetFileAttributesExW;
    inline decltype(&::GetFileAttributesExA) GetFileAttributesExA = ::GetFileAttributesExA;
    inline decltype(&::FindFirstFileW) FindFirstFileW = ::FindFirstFileW;
    inline decltype(&::FindFirstFileA) FindFirstFileA = ::FindFirstFileA;
    inline decltype(&::FindFirstFileExW) FindFirstFileExW = ::FindFirstFileExW;
    inline decltype(&::FindFirstFileExA) FindFirstFileExA = ::FindFirstFileExA;
    inline decltype(&::FindNextFileW) FindNextFileW = ::FindNextFileW;
    inline decltype(&::FindNextFileA) FindNextFileA = ::FindNextFileA;
    inline decltype(&::FindClose) FindClose = ::FindClose;
    inline decltype(&::DeleteFileW) DeleteFileW = ::DeleteFileW;
    inline decltype(&::DeleteFileA) DeleteFileA = ::DeleteFileA;
    inline decltype(&::MoveFileExW) MoveFileExW = ::MoveFileExW;
    inline decltype(&::MoveFileWithProgressW) MoveFileWithProgressW = ::MoveFileWithProgressW;
    inline decltype(&::ReplaceFileW) ReplaceFileW = ::ReplaceFileW;
    inline decltype(&::CopyFileW) CopyFileW = ::CopyFileW;
    inline decltype(&::CopyFileExW) CopyFileExW = ::CopyFileExW;
    inline decltype(&::SetFileInformationByHandle) SetFileInformationByHandle = ::SetFileInformationByHandle;
    inline CopyFile2Fn CopyFile2 = nullptr;      // set by Hooks::Install
    inline CreateFile2Fn CreateFile2 = nullptr;  // same
}
