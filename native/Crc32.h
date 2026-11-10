// Crc32.h - parameterised CRC-32. The default parameters from the ICD project
// file are the IEEE 802.3 CRC (check value for "123456789" is 0xCBF43926).
#pragma once
#include <cstdint>
#include <cstddef>

namespace icd {

class Crc32 {
public:
    Crc32() { Init(0x04C11DB7u, 0xFFFFFFFFu, 0xFFFFFFFFu, true, true); }

    void Init(uint32_t poly, uint32_t init, uint32_t xorOut, bool reflectIn, bool reflectOut) {
        m_init = init; m_xorOut = xorOut; m_refIn = reflectIn; m_refOut = reflectOut;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int b = 0; b < 8; ++b)
                c = (c & 0x80000000u) ? (c << 1) ^ poly : (c << 1);
            m_table[i] = c;
            uint8_t r = 0;
            for (int b = 0; b < 8; ++b)
                if (i & (1u << b)) r = static_cast<uint8_t>(r | (1u << (7 - b)));
            m_reflect[i] = r;
        }
    }

    uint32_t Compute(const uint8_t* data, size_t n) const {
        uint32_t c = m_init;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t b = m_refIn ? m_reflect[data[i]] : data[i];
            c = (c << 8) ^ m_table[((c >> 24) ^ b) & 0xFF];
        }
        if (m_refOut) {
            c = (static_cast<uint32_t>(m_reflect[c & 0xFF]) << 24) |
                (static_cast<uint32_t>(m_reflect[(c >> 8) & 0xFF]) << 16) |
                (static_cast<uint32_t>(m_reflect[(c >> 16) & 0xFF]) << 8) |
                 static_cast<uint32_t>(m_reflect[(c >> 24) & 0xFF]);
        }
        return c ^ m_xorOut;
    }

private:
    uint32_t m_table[256];
    uint8_t  m_reflect[256];
    uint32_t m_init, m_xorOut;
    bool     m_refIn, m_refOut;
};

} // namespace icd
