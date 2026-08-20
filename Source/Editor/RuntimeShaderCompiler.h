// Editor/RuntimeShaderCompiler.h - EDITOR-ONLY runtime HLSL -> DXIL/SPIR-V compiler.
//
// Heartbreak's shipped runtime has NO shader compiler (shaders are compiled offline by cmake). This
// adds one for the EDITOR only, so tools that generate shaders at runtime - the Material Maker's
// GPU texture preview/bake being the motivating case - can compile HLSL on the fly.
//
// It invokes the SAME DXC toolchain the offline build used (their absolute paths are baked in at
// configure time): the Windows-SDK DXC for DXIL (signed, accepted by D3D12) and the Vulkan-SDK DXC
// for SPIR-V, with the identical flags (SM 6.5, the -fvk-*-shift register shifts). Reusing the exact
// toolchain out-of-process means the runtime-compiled bytecode is byte-identical to an offline build
// and there is no dxcompiler.dll / dxil.dll version-matching hazard.
//
// The compiled bytecode is written into <exe>/shaders/<outName>.<stage>.{dxil,spv} - the SAME place
// and naming IRenderDevice::CreateComputePipeline / the PSO loaders read - so a runtime-generated
// kernel is created through the existing, tested pipeline path with ZERO RHI changes.
//
// NOT linked into the shipped runtime (lives in HBE_EDITOR_SOURCES).
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h" // rhi::GraphicsAPI

#include <string>
#include <vector>

namespace hbe::editor {

struct ShaderCompileResult {
    bool ok = false;
    std::string log;            // DXC stderr (errors + warnings); non-empty on failure
    std::string name;           // base name written under <exe>/shaders (pass to CreateComputePipeline)
    std::string stage;          // "cs" / "vs" / "ps"
    std::vector<u8> bytecode;   // the compiled DXIL / SPIR-V (also written to disk)
};

class RuntimeShaderCompiler {
public:
    // Is a DXC toolchain for this backend's bytecode format present on this machine?
    static bool Available(rhi::GraphicsAPI api);

    // Compile `hlsl` (compute/vertex/pixel per `stage`) for `api`'s bytecode format and write it to
    // <exe>/shaders/<outName>.<stage>.{dxil|spv}. `entry` is the entry-point function. Returns ok +
    // the DXC log on failure. GraphicsAPI::OpenGL is unsupported (returns ok=false).
    static ShaderCompileResult Compile(rhi::GraphicsAPI api, const std::string& hlsl,
                                       const std::string& entry, const std::string& stage,
                                       const std::string& outName);

    // --test-shadercompile: compile a known compute kernel for whichever toolchains are present and
    // verify the bytecode magic (DXContainer 'DXBC' for DXIL, 0x07230203 for SPIR-V). Headless, no
    // GPU. Skips (passes) a format whose toolchain is absent; fails if a present toolchain misbehaves.
    static bool SelfTest();
};

} // namespace hbe::editor
