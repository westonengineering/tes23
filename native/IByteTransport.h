// IByteTransport.h - the only interface the link engine uses to touch a wire.
// Implemented by Ftdi4232H, SerialCom and SimDevice.
#pragma once
#include <cstdint>
#include <string>

namespace icd {

class IByteTransport {
public:
    virtual ~IByteTransport() {}
    virtual bool IsOpen() const = 0;
    virtual void Close() = 0;
    // Queues all n bytes for transmit. False means not all bytes were accepted.
    virtual bool Write(const uint8_t* data, uint32_t n) = 0;
    // Non-blocking. Returns bytes copied (0 when none are waiting), -1 on error.
    virtual int32_t Read(uint8_t* data, uint32_t maxBytes) = 0;
    // Blocks until RX data is waiting or the timeout passes. True when data is waiting.
    virtual bool WaitRx(uint32_t timeoutUs) = 0;
    virtual std::string LastError() const = 0;
};

} // namespace icd
