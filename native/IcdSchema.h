// IcdSchema.h - runtime description of the ICD. The tables are produced by
// tools/icdgen.py into generated/IcdMessages.cpp. Do not hand-edit those tables.
#pragma once
#include <cstdint>
#include <cstddef>

namespace icd {

enum class FieldKind : uint8_t { Bool = 0, UInt = 1, Int = 2, Float32 = 3, Float64 = 4, Enum = 5 };

// Which direction a field exists in. A field missing from a direction still
// occupies its bytes, so offsets always line up between To and From.
enum FieldDir : uint8_t { DirTo = 1, DirFrom = 2, DirBoth = 3 };

struct FieldDesc {
    const char* name;
    uint16_t    byteOffset;   // offset in the image, where the message-type byte is 0
    uint16_t    byteCount;    // 1..8
    uint16_t    bitLow;       // bit 0 is the LSB of the assembled value
    uint16_t    bitWidth;     // 1..64
    FieldKind   kind;
    uint8_t     dir;          // FieldDir
    bool        bigEndian;
    double      scale;        // engineering = raw * scale + offset
    double      offset;
    double      defaultValue; // engineering units
    const char* unit;
    const char* enumText;     // "0=Off,1=On" or ""
};

struct MsgDesc {
    const char*      name;
    char             channel;      // 'C' or 'D'
    uint8_t          id;           // first byte on the wire (To direction)
    uint8_t          rxId;         // first byte expected back
    uint16_t         txLen;        // type byte + payload, no CRC, no trailer
    uint16_t         rxLen;        // type byte + payload, no CRC, no trailer
    bool             hasResponse;
    bool             cyclic;       // sent in the periodic window by default
    const FieldDesc* fields;
    uint16_t         fieldCount;
};

struct CrcDesc {
    uint32_t poly, init, xorOut;
    bool     reflectIn, reflectOut;
    bool     bigEndianOnWire;
    bool     coversType;           // CRC includes the message-type byte
};

struct ChannelDefaults {
    uint32_t baud, periodUs, windowUs, rxTimeoutUs;
    bool     trailerFf;            // append 0xFF on transmit
    bool     rs485Echo;            // receiver hears its own transmit
};

struct Schema {
    const MsgDesc*  messages;
    uint16_t        messageCount;
    uint32_t        txTotal;       // sum of txLen over all messages
    uint32_t        rxTotal;       // sum of rxLen over all messages
    uint32_t        hash;          // compared against the C# side on open
    CrcDesc         crc;
    ChannelDefaults channels[2];   // [0]=C, [1]=D
};

inline int ChannelIndex(char c) { return c == 'C' ? 0 : c == 'D' ? 1 : -1; }

// Bit-level image access shared by the generated code and the CLI.
uint64_t GetRaw(const uint8_t* image, const FieldDesc& f);
void     SetRaw(uint8_t* image, const FieldDesc& f, uint64_t raw);
int64_t  SignExtend(uint64_t raw, unsigned widthBits);
// Engineering-value conversions (bool as 0/1, scale/offset applied).
double   GetValue(const uint8_t* image, const FieldDesc& f);
// False if the value does not fit the field.
bool     SetValue(uint8_t* image, const FieldDesc& f, double value);

} // namespace icd
