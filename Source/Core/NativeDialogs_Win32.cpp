// Core/NativeDialogs_Win32.cpp - the Windows implementation of Core/NativeDialogs.h.
//
// This is the ONLY place in the editor that touches comdlg32 (GetOpenFileName), the shell
// folder picker (IFileDialog) and GDI (GetPixel). A second platform adds NativeDialogs_Gtk
// .cpp (or similar) beside it and the CMake selection picks one, exactly as the RHI does.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>  // GetOpenFileNameW
#include <shobjidl.h> // IFileDialog (folder picker)

#include "Core/NativeDialogs.h"

namespace hbe::platform {

namespace {

// UTF-8 -> UTF-16 for a dialog label. Labels here are ASCII, but doing it correctly costs
// nothing and means a translated label would not turn into mojibake.
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int need =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

// Build the Win32 common-dialog filter: pairs of "Label\0patterns\0", the whole thing
// terminated by a final empty entry. This is the Win32-only detail that used to leak into the
// portable Assets header; it now lives exactly where the Win32 API is called.
std::wstring BuildFilterBlock(const std::vector<FileFilter>& filters) {
    std::wstring block;
    const auto add = [&block](const std::wstring& label, const std::wstring& patterns) {
        block += label;
        block += L'\0';
        block += patterns;
        block += L'\0';
    };
    for (const FileFilter& f : filters) {
        std::wstring patterns;
        if (f.extensions.empty()) {
            patterns = L"*.*";
        } else {
            for (const std::string& ext : f.extensions) {
                if (!patterns.empty()) patterns += L';';
                patterns += L"*.";
                patterns += Widen(ext); // extensions are dotless ASCII tokens
            }
        }
        add(Widen(f.label), patterns);
    }
    add(L"All files", L"*.*"); // the backend always offers this; callers never add it
    block += L'\0';            // terminating empty entry
    return block;
}

} // namespace

std::optional<std::filesystem::path> OpenFileDialog(const std::vector<FileFilter>& filters) {
    // A single path fits easily; grow well past MAX_PATH so a deeply-nested selection is not
    // truncated the way the old fixed 1024/2048 wchar buffers could be.
    std::vector<wchar_t> file(4096, L'\0');
    const std::wstring filterBlock = BuildFilterBlock(filters);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filterBlock.c_str();
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    // FILEMUSTEXIST/PATHMUSTEXIST: only offer real files. NOCHANGEDIR: the dialog must not
    // move the process's current directory - asset paths are resolved relative to it.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&ofn)) return std::nullopt; // cancelled or dismissed
    return std::filesystem::path(file.data());
}

std::optional<std::filesystem::path> PickFolderDialog() {
    // S_FALSE (already initialised on this thread) is fine.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&dlg)))) {
        return std::nullopt;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);

    std::optional<std::filesystem::path> out;
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz))) {
                out = std::filesystem::path(psz);
                ::CoTaskMemFree(psz);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

bool SampleDesktopColorAtCursor(u8& r, u8& g, u8& b) {
    POINT pt{};
    if (!::GetCursorPos(&pt)) return false;
    HDC dc = ::GetDC(nullptr); // the whole-screen device context
    if (!dc) return false;
    const COLORREF c = ::GetPixel(dc, pt.x, pt.y);
    ::ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) return false;
    r = GetRValue(c);
    g = GetGValue(c);
    b = GetBValue(c);
    return true;
}

} // namespace hbe::platform
