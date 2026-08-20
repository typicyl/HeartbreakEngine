// Material/MaterialGraphHlsl.h - generate an HLSL compute shader from a material node graph.
//
// This is the GPU counterpart of the CPU evaluator (MaterialGraphCompiler): it emits, for the
// subgraph reachable from the Output node, one HLSL function `mm_<id>(float2 uv)` per node - value
// nodes call their inputs at the same uv, coordinate-transform + resampling nodes call their input
// at a MODIFIED uv (the same resamplable-function model as EvalRec, but as generated HLSL functions;
// a DAG, so no HLSL recursion). The kernel evaluates the 8 channels per pixel and writes them to a
// RWStructuredBuffer<float4> (8 floats-of-4 per pixel), which the editor reads back and uploads as
// the GPU preview / exported textures.
//
// Compiled at runtime by editor::RuntimeShaderCompiler. Backend-agnostic string generation lives
// here (headless-testable); the compile + dispatch lives in the editor.
#pragma once

#include "Material/MaterialGraph.h"

#include <string>

namespace hbe::mat {

// The generated kernel writes this many float4s per pixel to the output buffer, one per channel, in
// this order. (BaseColor.rgb + opacity in .a; Normal encoded 0..1 in .rgb; the rest broadcast in .r.)
enum class HlslOutChannel : u32 {
    BaseColor = 0,
    Roughness = 1,
    Metallic = 2,
    Normal = 3,
    Height = 4,
    AO = 5,
    Emissive = 6,
    Opacity = 7,
    Count = 8
};

// Generate a complete compute-shader HLSL string for `g`. Entry point is CSMain, [numthreads(8,8,1)].
// Bindings: cbuffer b0 { uint gRes; ... } ; RWStructuredBuffer<float4> gOut : register(u0). gOut is
// laid out as [pixel * 8 + channel]. Deterministic. Always emits COMPILABLE HLSL (every node type
// has at least a passthrough fallback), so any graph can be taken to the GPU.
std::string GenerateComputeHlsl(const Graph& g);

} // namespace hbe::mat
