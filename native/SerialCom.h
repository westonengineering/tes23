// SerialCom.h - Win32 COM-port wrapper using overlapped I/O.
// Use this when an interface is reached through a COM port (for example the
// FTDI VCP driver) instead of D2XX. Every public method is thread-safe.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "IByteTransport.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace icd {

enum class SerialParity : uint8_t { None = NOPARITY, Odd = ODDPARITY, Even = EVENPARITY, Mark = MARKPARITY, Space = SPACEPARITY };
enum class SerialFlow : uint8_t { None, RtsCts, DtrDsr, XonXoff };
enum class SerialRts : uint8_t { Off, On, Handshake, Toggle }; // Toggle: RTS high while transmitting (RS-485)

struct SerialConfig {
    uint32_t     baud = 115200;
    uint8_t      dataBits = 8;
    uint8_t      stopBits = 1;
    SerialParity parity = SerialParity::Odd;
    SerialFlow   flow = SerialFlow::None;
    SerialRts    rts = SerialRts::Off;
    bool         dtr = false;
    uint32_t     rxQueueBytes = 8192;
    uint32_t     txQueueBytes = 8192;
    uint32_t     writeTimeoutMs = 100;
};

struct SerialLineStatus {
    bool cts = false, dsr = false, ring = false, rlsd = false;
    uint32_t rxQueued = 0, txQueued = 0;
    uint32_t errors = 0; // CE_* bits since the last call
};

class SerialCom : public IByteTransport {
public:
    SerialCom();
    ~SerialCom() override;
    SerialCom(const SerialCom&) = delete;
    SerialCom& operator=(const SerialCom&) = delete;

    static std::vector<std::string> EnumeratePorts();

    bool Open(const std::string& portName, const SerialConfig& cfg); // "COM5" or "\\\\.\\COM12"
    bool Configure(const SerialConfig& cfg);

    // IByteTransport
    bool IsOpen() const override;
    void Close() override;
    bool Write(const uint8_t* data, uint32_t n) override;
    int32_t Read(uint8_t* data, uint32_t maxBytes) override;
    bool WaitRx(uint32_t timeoutUs) override;
    std::string LastError() const override;

    bool Purge(bool rx, bool tx);
    bool Flush();
    bool SetDtr(bool on);
    bool SetRts(bool on);
    bool SetBreak(bool on);
    bool GetLineStatus(SerialLineStatus* status);
    uint32_t RxQueued();

private:
    bool Fail(const char* what);
    uint32_t RxQueuedLocked();

    mutable std::mutex m_mutex;
    HANDLE      m_handle = INVALID_HANDLE_VALUE;
    HANDLE      m_readEvent = nullptr, m_writeEvent = nullptr, m_commEvent = nullptr;
    OVERLAPPED  m_commOv;
    DWORD       m_commMask = 0;
    bool        m_commPending = false;
    uint32_t    m_writeTimeoutMs = 100;
    std::string m_lastError;
};

} // namespace icd
