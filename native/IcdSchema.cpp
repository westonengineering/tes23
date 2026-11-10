#include "IcdSchema.h"
#include <cmath>
#include <cstring>

namespace icd {

static uint64_t Assemble(const uint8_t* p, const FieldDesc& f) {
    uint64_t v = 0;
    for (unsigned i = 0; i < f.byteCount; ++i) {
        unsigned index = f.bigEndian ? i : (f.byteCount - 1u - i);
        v = (v << 8) | p[f.byteOffset + index];
    }
    return v;
}

static void Disassemble(uint8_t* p, const FieldDesc& f, uint64_t v) {
    for (unsigned i = 0; i < f.byteCount; ++i) {
        unsigned index = f.bigEndian ? (f.byteCount - 1u - i) : i;
        p[f.byteOffset + index] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

static uint64_t WidthMask(unsigned width) {
    return width >= 64 ? ~0ull : ((1ull << width) - 1ull);
}

uint64_t GetRaw(const uint8_t* image, const FieldDesc& f) {
    return (Assemble(image, f) >> f.bitLow) & WidthMask(f.bitWidth);
}

void SetRaw(uint8_t* image, const FieldDesc& f, uint64_t raw) {
    const uint64_t mask = WidthMask(f.bitWidth) << f.bitLow;
    uint64_t v = Assemble(image, f);
    v = (v & ~mask) | ((raw << f.bitLow) & mask);
    Disassemble(image, f, v);
}

int64_t SignExtend(uint64_t raw, unsigned width) {
    if (width >= 64) return static_cast<int64_t>(raw);
    const uint64_t sign = 1ull << (width - 1u);
    return static_cast<int64_t>((raw ^ sign) - sign);
}

double GetValue(const uint8_t* image, const FieldDesc& f) {
    const uint64_t raw = GetRaw(image, f);
    switch (f.kind) {
    case FieldKind::Bool:    return raw ? 1.0 : 0.0;
    case FieldKind::Float32: { uint32_t u = static_cast<uint32_t>(raw); float x; std::memcpy(&x, &u, 4); return x * f.scale + f.offset; }
    case FieldKind::Float64: { double x; std::memcpy(&x, &raw, 8); return x * f.scale + f.offset; }
    case FieldKind::Int:     return static_cast<double>(SignExtend(raw, f.bitWidth)) * f.scale + f.offset;
    default:                 return static_cast<double>(raw) * f.scale + f.offset;
    }
}

bool SetValue(uint8_t* image, const FieldDesc& f, double value) {
    if (std::isnan(value)) return false;
    const double scaled = (value - f.offset) / f.scale;
    switch (f.kind) {
    case FieldKind::Bool:
        if (value != 0.0 && value != 1.0) return false;
        SetRaw(image, f, value != 0.0 ? 1u : 0u);
        return true;
    case FieldKind::Float32: {
        float x = static_cast<float>(scaled); uint32_t u; std::memcpy(&u, &x, 4);
        SetRaw(image, f, u); return true;
    }
    case FieldKind::Float64: {
        uint64_t u; std::memcpy(&u, &scaled, 8);
        SetRaw(image, f, u); return true;
    }
    case FieldKind::Int: {
        const double r = std::floor(scaled + 0.5);
        const double hi = std::ldexp(1.0, f.bitWidth - 1), lo = -hi;
        if (r < lo || r >= hi) return false;
        SetRaw(image, f, static_cast<uint64_t>(static_cast<int64_t>(r)));
        return true;
    }
    default: {
        const double r = std::floor(scaled + 0.5);
        if (r < 0 || r >= std::ldexp(1.0, f.bitWidth)) return false;
        SetRaw(image, f, static_cast<uint64_t>(r));
        return true;
    }
    }
}

} // namespace icd
