// Core/BinaryStream.h - minimal little-endian binary serialization.
#pragma once

#include "Core/Types.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace hbe {

// Appends POD values, strings and vectors to an in-memory byte buffer.
class BinaryWriter {
public:
    void Bytes(const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        data_.insert(data_.end(), b, b + n);
    }
    template <typename T>
    void Pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "Pod requires trivially copyable T");
        Bytes(&v, sizeof(T));
    }
    void Str(const std::string& s) {
        Pod<u32>(static_cast<u32>(s.size()));
        Bytes(s.data(), s.size());
    }
    template <typename T>
    void Vec(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>, "Vec requires trivially copyable element");
        Pod<u32>(static_cast<u32>(v.size()));
        if (!v.empty()) Bytes(v.data(), v.size() * sizeof(T));
    }

    const std::vector<u8>& Data() const { return data_; }

    bool SaveToFile(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data_.data()),
                  static_cast<std::streamsize>(data_.size()));
        return out.good();
    }

private:
    std::vector<u8> data_;
};

// Reads back values written by BinaryWriter. Tracks an error/overflow flag.
class BinaryReader {
public:
    BinaryReader(const u8* data, usize size) : p_(data), end_(data + size) {}
    explicit BinaryReader(const std::vector<u8>& v) : p_(v.data()), end_(v.data() + v.size()) {}

    bool Bytes(void* out, usize n) {
        if (ok_ && static_cast<usize>(end_ - p_) >= n) {
            std::memcpy(out, p_, n);
            p_ += n;
            return true;
        }
        ok_ = false;
        return false;
    }
    template <typename T>
    bool Pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "Pod requires trivially copyable T");
        return Bytes(&v, sizeof(T));
    }
    bool Str(std::string& s) {
        u32 n = 0;
        if (!Pod(n)) return false;
        if (static_cast<usize>(end_ - p_) < n) { ok_ = false; return false; }
        s.assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return true;
    }
    template <typename T>
    bool Vec(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>, "Vec requires trivially copyable element");
        u32 n = 0;
        if (!Pod(n)) return false;
        const usize bytes = static_cast<usize>(n) * sizeof(T);
        if (static_cast<usize>(end_ - p_) < bytes) { ok_ = false; return false; }
        v.resize(n);
        if (n) { std::memcpy(v.data(), p_, bytes); p_ += bytes; }
        return true;
    }

    bool Ok() const { return ok_; }

    static std::vector<u8> LoadFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return {};
        const std::streamsize size = in.tellg();
        in.seekg(0);
        std::vector<u8> data(static_cast<usize>(size));
        in.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }

private:
    const u8* p_;
    const u8* end_;
    bool ok_ = true;
};

} // namespace hbe
