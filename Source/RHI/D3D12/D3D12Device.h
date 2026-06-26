// RHI/D3D12/D3D12Device.h - factory entry point for the Direct3D 12 backend.
//
// The concrete device type is defined privately in D3D12Device.cpp so that this
// header (included by the cross-backend factory) stays free of <d3d12.h>.
#pragma once

#include "RHI/RHI.h"

#include <memory>

namespace hbe::rhi {

std::unique_ptr<IRenderDevice> CreateD3D12Device(const RenderDeviceDesc& desc);

} // namespace hbe::rhi
