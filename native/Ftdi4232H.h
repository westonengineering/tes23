// Ftdi4232H.h - full-featured wrapper around ftd2xx.dll for the FT4232H(L).
//
// Every public method is safe to call from any thread. The wrapper links
// ftd2xx.dll with /DELAYLOAD, so programs still start on a machine without the
// FTDI driver. Call IsDriverAvailable() before touching a device.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "ftd2xx.h"
#include "IByteTransport.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace icd {

struct FtdiDeviceInfo {
    uint32_t    index = 0;
    uint32_t    flags = 0;
    uint32_t    type = 0;
    uint32_t    id = 0;
    uint32_t    locationId = 0;
    std::string serial;       // interface serial, for example "FT7ABCDEC"
    std::string description;  // for example "Quad RS232-HS C"
    char        channel = 0;  // 'A'..'D', or 0 when it cannot be determined
    bool        isOpen = false;
    bool        isHighSpeed = false;
    bool        isQuad = false;
    std::string SerialBase() const; // serial without the trailing channel letter
};

enum class FtdiParity : uint8_t { None = FT_PARITY_NONE, Odd = FT_PARITY_ODD, Even = FT_PARITY_EVEN, Mark = FT_PARITY_MARK, Space = FT_PARITY_SPACE };
enum class FtdiFlow : uint16_t  { None = FT_FLOW_NONE, RtsCts = FT_FLOW_RTS_CTS, DtrDsr = FT_FLOW_DTR_DSR, XonXoff = FT_FLOW_XON_XOFF };

struct FtdiUartConfig {
    uint32_t   baud = 115200;
    uint8_t    dataBits = 8;
    uint8_t    stopBits = 1;                 // 1 or 2
    FtdiParity parity = FtdiParity::Odd;
    FtdiFlow   flow = FtdiFlow::None;
    uint8_t    xon = 0x11, xoff = 0x13;
    uint8_t    latencyMs = 1;                // 1 gives the lowest RX latency
    uint32_t   usbInBytes = 4096;            // multiple of 64, 64..65536
    uint32_t   usbOutBytes = 4096;
    uint32_t   readTimeoutMs = 50;           // FT_Read only, the engine never blocks here
    uint32_t   writeTimeoutMs = 50;
};

struct FtdiLineStatus {
    bool cts = false, dsr = false, ri = false, dcd = false;
    bool overrun = false, parityError = false, framingError = false, breakInterrupt = false;
};

struct Ftdi4232HEeprom {
    FT_EEPROM_4232H data;
    std::string manufacturer, manufacturerId, description, serial;
    Ftdi4232HEeprom() { ZeroMemory(&data, sizeof(data)); data.common.deviceType = FT_DEVICE_4232H; }
};

class Ftdi4232H : public IByteTransport {
public:
    Ftdi4232H();
    ~Ftdi4232H() override;
    Ftdi4232H(const Ftdi4232H&) = delete;
    Ftdi4232H& operator=(const Ftdi4232H&) = delete;

    // Driver and enumeration
    static bool IsDriverAvailable();
    static bool GetLibraryVersion(uint32_t* version);
    static bool Enumerate(std::vector<FtdiDeviceInfo>& out, std::string* error = nullptr);
    static bool Rescan();
    static bool Reload(uint16_t vid, uint16_t pid);

    // Open. Channel is 'A'..'D'. serialBase selects the physical chip and may be
    // empty only when exactly one FT4232H is attached.
    bool Open(char channel, const std::string& serialBase = std::string());
    bool OpenByIndex(uint32_t index);
    bool OpenBySerial(const std::string& serial);
    bool OpenByDescription(const std::string& description);
    bool OpenByLocation(uint32_t locationId);

    // IByteTransport
    bool IsOpen() const override;
    void Close() override;
    bool Write(const uint8_t* data, uint32_t n) override;
    int32_t Read(uint8_t* data, uint32_t maxBytes) override;
    bool WaitRx(uint32_t timeoutUs) override;
    std::string LastError() const override;

    FT_HANDLE Handle() const { return m_handle; } // escape hatch for FTDI calls not wrapped here
    FT_STATUS LastStatus() const;

    // UART setup
    bool ConfigureUart(const FtdiUartConfig& cfg);
    bool SetBaudRate(uint32_t baud);
    bool SetDivisor(uint16_t divisor);
    bool SetDataCharacteristics(uint8_t dataBits, uint8_t stopBits, FtdiParity parity);
    bool SetFlowControl(FtdiFlow flow, uint8_t xon = 0x11, uint8_t xoff = 0x13);
    bool SetChars(uint8_t eventChar, bool eventEnable, uint8_t errorChar, bool errorEnable);
    bool SetTimeouts(uint32_t readMs, uint32_t writeMs);
    bool SetLatencyTimer(uint8_t ms);
    bool GetLatencyTimer(uint8_t* ms);
    bool SetUsbParameters(uint32_t inBytes, uint32_t outBytes);
    bool SetDeadmanTimeout(uint32_t ms);

    // Modem and line control
    bool SetDtr(bool on);
    bool SetRts(bool on);
    bool SetBreak(bool on);
    bool GetLineStatus(FtdiLineStatus* status);
    uint32_t LastLineErrorBits(); // sticky OE/PE/FE/BI bits since the last call, cleared on read

    // Queues
    bool GetQueueStatus(uint32_t* rxBytes);
    bool GetStatus(uint32_t* rxBytes, uint32_t* txBytes, uint32_t* events);
    bool Purge(bool rx, bool tx);
    bool SetEventNotification(uint32_t mask, HANDLE event);
    bool StopInTask();
    bool RestartInTask();

    // Device control
    bool ResetDevice();
    bool ResetPort();
    bool CyclePort();
    bool GetDeviceInfo(FtdiDeviceInfo* info);
    bool GetDriverVersion(uint32_t* version);
    bool GetComPortNumber(int32_t* com);

    // Bit modes (FT_BITMODE_*). MPSSE is available on interfaces A and B only.
    bool SetBitMode(uint8_t mask, uint8_t mode);
    bool GetBitMode(uint8_t* pins);

    // EEPROM. These are slow. Do not call them while cyclic traffic must run.
    bool ReadEeprom(Ftdi4232HEeprom* out);
    bool ProgramEeprom(const Ftdi4232HEeprom& in);
    bool ReadEepromWord(uint32_t wordOffset, uint16_t* value);
    bool WriteEepromWord(uint32_t wordOffset, uint16_t value);
    bool EraseEeprom();
    bool GetUserAreaSize(uint32_t* bytes);
    bool ReadUserArea(uint8_t* data, uint32_t n, uint32_t* got);
    bool WriteUserArea(const uint8_t* data, uint32_t n);

    // RS-485 helper: whether the RI pin of channel is configured as TXDEN.
    bool IsRiTxDen(char channel, bool* enabled);

    // Friendly name for an FT_STATUS value.
    static const char* StatusText(FT_STATUS status);

private:
    bool Check(FT_STATUS status, const char* what);
    bool RequireOpen(const char* what);
    bool AttachRxEvent();

    mutable std::recursive_mutex m_mutex;
    FT_HANDLE   m_handle = nullptr;
    HANDLE      m_rxEvent = nullptr;
    FT_STATUS   m_lastStatus = FT_OK;
    std::string m_lastError;
    uint32_t    m_stickyLineErrors = 0;
};

} // namespace icd
