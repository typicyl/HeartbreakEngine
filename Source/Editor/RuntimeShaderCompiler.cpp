// Editor/RuntimeShaderCompiler.cpp - see RuntimeShaderCompiler.h.
#include "Editor/RuntimeShaderCompiler.h"

#include "Core/Log.h"
#include "Core/Platform.h" // platform::ExecutableDir()

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

// The toolchain DXC paths + shader include dir baked in by cmake (see CMakeLists target_compile_
// definitions). Fallbacks keep the TU compilable if built outside that cmake (Available() -> false).
#ifndef HBE_RT_DXC_DXIL
#  define HBE_RT_DXC_DXIL ""
#endif
#ifndef HBE_RT_DXC_SPIRV
#  define HBE_RT_DXC_SPIRV ""
#endif
#ifndef HBE_RT_SHADER_INCLUDE
#  define HBE_RT_SHADER_INCLUDE ""
#endif

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace hbe::editor {

namespace {
namespace fs = std::filesystem;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

const char* DxcPath(rhi::GraphicsAPI api) {
    switch (api) {
        case rhi::GraphicsAPI::D3D12: return HBE_RT_DXC_DXIL;   // Windows-SDK DXC (signs DXIL)
        case rhi::GraphicsAPI::Vulkan: return HBE_RT_DXC_SPIRV; // Vulkan-SDK DXC (SPIR-V backend)
        default: return "";
    }
}
const char* Ext(rhi::GraphicsAPI api) { return api == rhi::GraphicsAPI::Vulkan ? "spv" : "dxil"; }

// Run a process, capturing combined stdout+stderr into `outLog`. Returns false only if the process
// could not be started; a nonzero compiler exit is reported via `exitCode`.
bool RunCapture(std::wstring cmdline, std::string& outLog, DWORD& exitCode) {
    wchar_t tmpDir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmpDir)) return false;
    wchar_t logPath[MAX_PATH];
    if (!GetTempFileNameW(tmpDir, L"dxc", 0, logPath)) return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        DeleteFileW(logPath);
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    // CreateProcessW may modify the command-line buffer, so pass a writable copy.
    const BOOL started = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hLog);
    if (!started) {
        DeleteFileW(logPath);
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    exitCode = ec;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::ifstream f(logPath, std::ios::binary);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        outLog = ss.str();
    }
    DeleteFileW(logPath);
    return true;
}
} // namespace

bool RuntimeShaderCompiler::Available(rhi::GraphicsAPI api) {
    const char* p = DxcPath(api);
    std::error_code ec;
    return p && p[0] && fs::exists(fs::path(p), ec);
}

ShaderCompileResult RuntimeShaderCompiler::Compile(rhi::GraphicsAPI api, const std::string& hlsl,
                                                   const std::string& entry, const std::string& stage,
                                                   const std::string& outName) {
    ShaderCompileResult r;
    r.name = outName;
    r.stage = stage;
    if (api == rhi::GraphicsAPI::OpenGL) {
        r.log = "runtime shader compilation is unsupported on the OpenGL backend";
        return r;
    }
    const char* dxc = DxcPath(api);
    std::error_code ec;
    if (!dxc || !dxc[0] || !fs::exists(fs::path(dxc), ec)) {
        r.log = std::string("DXC toolchain not found for this backend: ") + (dxc ? dxc : "(none)");
        return r;
    }

    // Write the HLSL source to a temp file (DXC compiles a file, not a string, from the CLI).
    const fs::path tmp = fs::temp_directory_path(ec) / (std::string("hbe_rt_") + outName + ".hlsl");
    {
        std::ofstream o(tmp, std::ios::binary);
        if (!o) {
            r.log = "cannot write temp HLSL source";
            return r;
        }
        o << hlsl;
    }

    const fs::path shadersDir = platform::ExecutableDir() / "shaders";
    fs::create_directories(shadersDir, ec);
    const fs::path outFile = shadersDir / (outName + "." + stage + "." + Ext(api));

    const std::string profile = stage + "_6_5"; // SM 6.5, matching the offline build
    std::wstring cmd = L"\"" + Widen(dxc) + L"\"";
    cmd += L" -T " + Widen(profile) + L" -E " + Widen(entry);
    if (const char* inc = HBE_RT_SHADER_INCLUDE; inc && inc[0])
        cmd += L" -I \"" + Widen(inc) + L"\"";
    if (api == rhi::GraphicsAPI::Vulkan)
        cmd += L" -spirv -fspv-target-env=vulkan1.2 -fvk-t-shift 1000 0 -fvk-s-shift 2000 0 "
               L"-fvk-u-shift 3000 0";
    cmd += L" -Fo \"" + outFile.wstring() + L"\" \"" + tmp.wstring() + L"\"";

    std::string log;
    DWORD exitCode = 1;
    const bool started = RunCapture(cmd, log, exitCode);
    fs::remove(tmp, ec);
    r.log = log;
    if (!started) {
        r.log = "failed to launch DXC. " + log;
        return r;
    }
    if (exitCode != 0) return r; // r.ok stays false; r.log carries the compiler diagnostics

    std::ifstream bf(outFile, std::ios::binary);
    if (!bf) {
        r.log = "DXC reported success but produced no output file: " + outFile.string();
        return r;
    }
    r.bytecode.assign(std::istreambuf_iterator<char>(bf), std::istreambuf_iterator<char>());
    if (r.bytecode.empty()) {
        r.log = "DXC produced an empty output";
        return r;
    }
    r.ok = true;
    return r;
}

bool RuntimeShaderCompiler::SelfTest() {
    // A minimal compute kernel that reads a UAV so it is not optimised to nothing. Register u0 with
    // the -fvk-u-shift maps cleanly to SPIR-V; no explicit [[vk::binding]] needed for a compile test.
    const char* kSrc =
        "RWStructuredBuffer<uint> gOut : register(u0);\n"
        "[numthreads(64,1,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) { gOut[id.x] = id.x * 2u + 1u; }\n";

    struct Case {
        rhi::GraphicsAPI api;
        const char* label;
    };
    int tested = 0, failed = 0;
    for (const Case c : {Case{rhi::GraphicsAPI::D3D12, "DXIL"}, Case{rhi::GraphicsAPI::Vulkan, "SPIR-V"}}) {
        if (!Available(c.api)) {
            HBE_INFO("[shadercompile] {} toolchain absent - skipped", c.label);
            continue;
        }
        ++tested;
        const ShaderCompileResult r =
            Compile(c.api, kSrc, "CSMain", "cs", std::string("RtSelfTest_") + c.label);
        if (!r.ok) {
            HBE_ERROR("[shadercompile] {} compile FAILED: {}", c.label, r.log);
            ++failed;
            continue;
        }
        bool magicOk = false;
        if (c.api == rhi::GraphicsAPI::Vulkan) {
            if (r.bytecode.size() >= 4) {
                u32 m = 0;
                std::memcpy(&m, r.bytecode.data(), 4);
                magicOk = (m == 0x07230203u); // SPIR-V magic
            }
        } else {
            magicOk = r.bytecode.size() >= 4 && r.bytecode[0] == 'D' && r.bytecode[1] == 'X' &&
                      r.bytecode[2] == 'B' && r.bytecode[3] == 'C'; // DXContainer magic
        }
        if (!magicOk) {
            HBE_ERROR("[shadercompile] {} produced invalid bytecode ({} bytes)", c.label,
                      r.bytecode.size());
            ++failed;
        } else {
            HBE_INFO("[shadercompile] {} OK ({} bytes, signed/valid)", c.label, r.bytecode.size());
        }
    }
    if (tested == 0) {
        HBE_INFO("[shadercompile] no DXC toolchain present - nothing to test (pass)");
        return true;
    }
    return failed == 0;
}

} // namespace hbe::editor
