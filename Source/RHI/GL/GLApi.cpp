// RHI/GL/GLApi.cpp - definitions + runtime loader for the GL entry points.
#include "RHI/GL/GLApi.h"
#include "Core/Log.h"

#include <string>

namespace hbe::rhi::gl {

// Storage for each function pointer.
#define HBE_GL_DEFN(ret, name, params) PFN_##name name = nullptr;
HBE_GL_FUNCTIONS(HBE_GL_DEFN)
#undef HBE_GL_DEFN

namespace {
// wglGetProcAddress can hand back 1/2/3/-1 as "valid but unavailable" sentinels on
// some drivers; treat those as failures.
void* GetGLProc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    const auto v = reinterpret_cast<intptr_t>(p);
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) {
        // GL 1.1 functions live in opengl32.dll, not the ICD; fall back there.
        if (HMODULE m = GetModuleHandleA("opengl32.dll"))
            p = reinterpret_cast<void*>(GetProcAddress(m, name));
        else
            p = nullptr;
    }
    return p;
}
} // namespace

bool LoadGLFunctions() {
    bool ok = true;
    int missing = 0;
#define HBE_GL_LOAD(ret, name, params)                                       \
    name = reinterpret_cast<PFN_##name>(GetGLProc(#name));                    \
    if (!name) {                                                              \
        /* glDebugMessageCallback is optional (debug builds / KHR_debug). */  \
        if (std::string(#name) != "glDebugMessageCallback") {                \
            HBE_ERROR("[GL] missing entry point: {}", #name);                \
            ++missing;                                                        \
            ok = false;                                                       \
        }                                                                    \
    }
    HBE_GL_FUNCTIONS(HBE_GL_LOAD)
#undef HBE_GL_LOAD
    if (!ok) HBE_ERROR("[GL] {} required GL entry point(s) unavailable.", missing);
    return ok;
}

} // namespace hbe::rhi::gl
