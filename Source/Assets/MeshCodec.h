// Assets/MeshCodec.h - compact, portable geometry (de)serialization for .uaf v10.
//
// WHY. Before v10 a mesh submesh stored a RAW dump of the 72-byte interleaved Vertex
// (48 bytes of attributes + 24 bytes of skinning that are ALWAYS ZERO on a static mesh),
// plus raw 32-bit indices, plus the same raw dump again per distance LOD. That is ~4-6x
// larger than it needs to be, and - being a raw C++ struct dump - is endianness/ABI
// coupled, which the platform-independence goal forbids.
//
// v10 instead QUANTIZES each vertex (position to u16 in the submesh AABB, normal/tangent
// octahedral-encoded to i16x2 + a handedness bit, UV to u16 in the submesh UV bounds,
// skin weights to u8) and then runs meshoptimizer's vertex/index CODEC over the compact
// stream. The encoded byte streams are explicitly little-endian and version-tagged by
// meshopt (portable across any platform build), so the FORMAT stops depending on the C++
// struct layout. On load the block is decoded and dequantized back into the EXACT 72-byte
// Vertex the RHI already expects - so no input layout, PSO, or shader changes at all.
//
// Static meshes drop the 24 skinning bytes entirely (reconstructed as zeros); skinned
// meshes keep joints EXACT (they are skeleton indices) and weights as renormalized u8.
#pragma once

#include "Assets/Mesh.h"
#include "Core/BinaryStream.h"
#include "Core/Types.h"

#include <vector>

namespace hbe::meshcodec {

// Appends one quantized+encoded geometry block (vertices + indices) to `w`. Vertices are
// NOT reordered, so caller-parallel arrays (morph-target deltas) stay aligned. Skinning
// is detected from the vertices themselves. Deterministic for a given input.
void WriteGeometry(BinaryWriter& w, const std::vector<Vertex>& vertices,
                   const std::vector<u32>& indices);

// Reads a block written by WriteGeometry, reconstituting the full 72-byte Vertex layout
// (static meshes get zeroed joints/weights). Returns false on corruption / decode error.
bool ReadGeometry(BinaryReader& r, std::vector<Vertex>& vertices, std::vector<u32>& indices);

// --test-meshcodec: round-trips static + skinned geometry, asserting positions within
// the quantization epsilon, indices EXACT, tangent.w handedness sign EXACT, skin weights
// summing to ~1, static skinning reconstructed as exact zeros, and byte-identical
// determinism across two encodes. Headless.
bool SelfTest();

} // namespace hbe::meshcodec
