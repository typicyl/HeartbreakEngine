// Core/Types.h - fundamental fixed-width type aliases used across the engine.
#pragma once

#include <cstdint>
#include <cstddef>

// ONE STL EXCEPTION ABI ACROSS THE WHOLE BUILD. This header is reachable from
// essentially every TU, which is exactly why the check lives here.
//
// `_HAS_EXCEPTIONS=0` does not merely "turn off exceptions" - it selects a
// different, smaller std::exception (16 bytes instead of 24) with different
// ownership of the message buffer. Mixing the two across a library boundary is an
// ODR violation on std::exception itself, and the way it shows up is a bad free:
// a `throw`/`catch` inside a TU compiled one way, unwound by machinery compiled
// the other, hands the CRT a pointer it never allocated. It cost a long hunt once
// already (it was blamed on nlohmann/json), so it is a hard compile error now.
//
// The historical source was JoltPhysics, which pushes the define PUBLIC unless
// CPP_EXCEPTIONS_ENABLED is ON - see cmake/Dependencies.cmake, which forces it ON
// and additionally strips the define from Jolt's interface.
#if defined(_MSC_VER) && defined(_HAS_EXCEPTIONS) && _HAS_EXCEPTIONS == 0
#  error "_HAS_EXCEPTIONS=0 in a Heartbreak TU: this splits std::exception's ABI \
between the engine libraries and the executables and corrupts the heap on every \
throw. See cmake/Dependencies.cmake (CPP_EXCEPTIONS_ENABLED)."
#endif

namespace hbe {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;

// Non-copyable mixin for resource-owning types (devices, swapchains, ...).
class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace hbe
