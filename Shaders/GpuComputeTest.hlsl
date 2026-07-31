// GpuComputeTest.hlsl - proof kernel for the general compute + GPU-writable
// structured buffer seam (IRenderDevice::CreateGpuBuffer / CreateComputePipeline /
// QueueCompute). Driven by HeartbreakEditor --test-gpucompute, which dispatches it
// on the active backend and reads the result back; it exists so the RHI plumbing is
// VERIFIED on real hardware rather than merely compiled.
//
// It is also the worked example of the binding convention documented on
// ComputePipelineDesc in RHI.h - one constant block, then the UAVs, then the SRVs,
// each with an explicit [[vk::binding]] (without which ShaderCompile.cmake's
// -fvk-*-shift remapping would put the Vulkan bindings somewhere else entirely):
//
//   constants  : register(b0)    [[vk::binding(0, 0)]]
//   uav  i     : register(u<i>)  [[vk::binding(1 + i, 0)]]
//   srv  i     : register(t<i>)  [[vk::binding(1 + uavCount + i, 0)]]

[[vk::binding(0, 0)]]
cbuffer TestCB : register(b0) {
    uint gCount;   // elements to process
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<uint> gOut : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>   gIn  : register(t0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= gCount) return;
    // Deliberately reads the SRV, writes the UAV, and mixes in the constant so a
    // failure in ANY of the three bindings changes the result.
    gOut[i] = gIn[i] * 2u + 1u + (gCount << 16);
}
