#include "Ftdi4232H.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace icd {

#define FT_LOCK std::lock_guard<std::recursive_mutex> lock_(m_mutex)

std::string FtdiDeviceInfo::SerialBase() const {
    if (channel != 0 && !serial.empty() && serial.back() == channel)
        return serial.substr(0, serial.size() - 1);
    return serial;
}

static char ChannelFrom(const std::string& description, const std::string& serial) {
    if (description.size() >= 2 && description[description.size() - 2] == ' ') {
        const char c = description.back();
        if (c >= 'A' && c <= 'D') return c;
    }
    if (!serial.empty()) {
        const char c = serial.back();
        if (c >= 'A' && c <= 'D') return c;
    }
    return 0;
}

static bool IsQuadType(uint32_t type) {
    return type == FT_DEVICE_4232H || type == FT_DEVICE_4232HP || type == FT_DEVICE_4233HP ||
           type == FT_DEVICE_4232HA || type == FT_DEVICE_4232HPN || type == FT_DEVICE_4233HPN;
}

const char* Ftdi4232H::StatusText(FT_STATUS s) {
    switch (s) {
    case FT_OK: return "OK";
    case FT_INVALID_HANDLE: return "invalid handle";
    case FT_DEVICE_NOT_FOUND: return "device not found";
    case FT_DEVICE_NOT_OPENED: return "device not opened (in use by another program or the VCP driver?)";
    case FT_IO_ERROR: return "I/O error";
    case FT_INSUFFICIENT_RESOURCES: return "insufficient resources";
    case FT_INVALID_PARAMETER: return "invalid parameter";
    case FT_INVALID_BAUD_RATE: return "invalid baud rate";
    case FT_DEVICE_NOT_OPENED_FOR_ERASE: return "not opened for erase";
    case FT_DEVICE_NOT_OPENED_FOR_WRITE: return "not opened for write";
    case FT_FAILED_TO_WRITE_DEVICE: return "failed to write device";
    case FT_EEPROM_READ_FAILED: return "EEPROM read failed";
    case FT_EEPROM_WRITE_FAILED: return "EEPROM write failed";
    case FT_EEPROM_ERASE_FAILED: return "EEPROM erase failed";
    case FT_EEPROM_NOT_PRESENT: return "EEPROM not present";
    case FT_EEPROM_NOT_PROGRAMMED: return "EEPROM not programmed";
    case FT_INVALID_ARGS: return "invalid arguments";
    case FT_NOT_SUPPORTED: return "not supported";
    case FT_OTHER_ERROR: return "other error";
    case FT_DEVICE_LIST_NOT_READY: return "device list not ready";
    default: return "unknown FT_STATUS";
    }
}

Ftdi4232H::Ftdi4232H() {}
Ftdi4232H::~Ftdi4232H() { Close(); }

bool Ftdi4232H::Check(FT_STATUS status, const char* what) {
    m_lastStatus = status;
    if (status == FT_OK) return true;
    m_lastError = std::string(what) + ": " + StatusText(status);
    return false;
}

bool Ftdi4232H::RequireOpen(const char* what) {
    if (m_handle) return true;
    m_lastStatus = FT_INVALID_HANDLE;
    m_lastError = std::string(what) + ": device is not open";
    return false;
}

bool Ftdi4232H::IsDriverAvailable() {
    HMODULE h = LoadLibraryA("ftd2xx.dll");
    if (!h) return false;
    FreeLibrary(h);
    return true;
}

bool Ftdi4232H::GetLibraryVersion(uint32_t* version) {
    if (!IsDriverAvailable()) return false;
    DWORD v = 0;
    if (FT_GetLibraryVersion(&v) != FT_OK) return false;
    if (version) *version = v;
    return true;
}

bool Ftdi4232H::Rescan() { return IsDriverAvailable() && FT_Rescan() == FT_OK; }
bool Ftdi4232H::Reload(uint16_t vid, uint16_t pid) { return IsDriverAvailable() && FT_Reload(vid, pid) == FT_OK; }

bool Ftdi4232H::Enumerate(std::vector<FtdiDeviceInfo>& out, std::string* error) {
    out.clear();
    if (!IsDriverAvailable()) {
        if (error) *error = "ftd2xx.dll was not found. Install the FTDI D2XX driver.";
        return false;
    }
    DWORD count = 0;
    FT_STATUS s = FT_CreateDeviceInfoList(&count);
    if (s != FT_OK) {
        if (error) *error = std::string("FT_CreateDeviceInfoList: ") + StatusText(s);
        return false;
    }
    if (count == 0) return true;
    std::vector<FT_DEVICE_LIST_INFO_NODE> nodes(count);
    s = FT_GetDeviceInfoList(nodes.data(), &count);
    if (s != FT_OK) {
        if (error) *error = std::string("FT_GetDeviceInfoList: ") + StatusText(s);
        return false;
    }
    for (DWORD i = 0; i < count; ++i) {
        FtdiDeviceInfo d;
        d.index = i;
        d.flags = nodes[i].Flags;
        d.type = nodes[i].Type;
        d.id = nodes[i].ID;
        d.locationId = nodes[i].LocId;
        d.serial = nodes[i].SerialNumber;
        d.description = nodes[i].Description;
        d.channel = ChannelFrom(d.description, d.serial);
        d.isOpen = (nodes[i].Flags & FT_FLAGS_OPENED) != 0;
        d.isHighSpeed = (nodes[i].Flags & FT_FLAGS_HISPEED) != 0;
        d.isQuad = IsQuadType(d.type);
        out.push_back(d);
    }
    return true;
}

bool Ftdi4232H::AttachRxEvent() {
    if (!m_rxEvent) m_rxEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_rxEvent) { m_lastError = "CreateEvent failed"; return false; }
    return Check(FT_SetEventNotification(m_handle, FT_EVENT_RXCHAR, m_rxEvent), "FT_SetEventNotification");
}

bool Ftdi4232H::Open(char channel, const std::string& serialBase) {
    FT_LOCK;
    if (channel < 'A' || channel > 'D') { m_lastError = "channel must be A..D"; return false; }
    std::vector<FtdiDeviceInfo> list;
    std::string err;
    if (!Enumerate(list, &err)) { m_lastError = err; return false; }

    std::set<std::string> chips;
    const FtdiDeviceInfo* match = nullptr;
    for (const FtdiDeviceInfo& d : list) {
        if (!d.isQuad || d.channel != channel) continue;
        if (!serialBase.empty() && d.SerialBase() != serialBase) continue;
        chips.insert(d.SerialBase() + "@" + std::to_string(d.locationId >> 4));
        if (!match) match = &d;
    }
    if (!match) {
        m_lastError = std::string("no FT4232H interface ") + channel + " found" +
                      (serialBase.empty() ? "" : " for serial " + serialBase);
        return false;
    }
    if (chips.size() > 1) {
        // Never guess which board to drive.
        m_lastError = "more than one FT4232H attached, specify the serial";
        return false;
    }
    if (match->isOpen) {
        m_lastError = std::string("interface ") + channel + " is already open in another program";
        return false;
    }
    return OpenByIndex(match->index);
}

bool Ftdi4232H::OpenByIndex(uint32_t index) {
    FT_LOCK;
    Close();
    if (!IsDriverAvailable()) { m_lastError = "ftd2xx.dll was not found"; return false; }
    if (!Check(FT_Open(static_cast<int>(index), &m_handle), "FT_Open")) { m_handle = nullptr; return false; }
    return AttachRxEvent();
}

bool Ftdi4232H::OpenBySerial(const std::string& serial) {
    FT_LOCK;
    Close();
    if (!IsDriverAvailable()) { m_lastError = "ftd2xx.dll was not found"; return false; }
    if (!Check(FT_OpenEx(const_cast<char*>(serial.c_str()), FT_OPEN_BY_SERIAL_NUMBER, &m_handle), "FT_OpenEx(serial)")) { m_handle = nullptr; return false; }
    return AttachRxEvent();
}

bool Ftdi4232H::OpenByDescription(const std::string& description) {
    FT_LOCK;
    Close();
    if (!IsDriverAvailable()) { m_lastError = "ftd2xx.dll was not found"; return false; }
    if (!Check(FT_OpenEx(const_cast<char*>(description.c_str()), FT_OPEN_BY_DESCRIPTION, &m_handle), "FT_OpenEx(description)")) { m_handle = nullptr; return false; }
    return AttachRxEvent();
}

bool Ftdi4232H::OpenByLocation(uint32_t locationId) {
    FT_LOCK;
    Close();
    if (!IsDriverAvailable()) { m_lastError = "ftd2xx.dll was not found"; return false; }
    if (!Check(FT_OpenEx(reinterpret_cast<PVOID>(static_cast<uintptr_t>(locationId)), FT_OPEN_BY_LOCATION, &m_handle), "FT_OpenEx(location)")) { m_handle = nullptr; return false; }
    return AttachRxEvent();
}

bool Ftdi4232H::IsOpen() const { FT_LOCK; return m_handle != nullptr; }

void Ftdi4232H::Close() {
    FT_LOCK;
    if (m_handle) {
        FT_SetEventNotification(m_handle, 0, nullptr);
        FT_Close(m_handle);
        m_handle = nullptr;
    }
    if (m_rxEvent) { CloseHandle(m_rxEvent); m_rxEvent = nullptr; }
}

std::string Ftdi4232H::LastError() const { FT_LOCK; return m_lastError; }
FT_STATUS Ftdi4232H::LastStatus() const { FT_LOCK; return m_lastStatus; }

bool Ftdi4232H::Write(const uint8_t* data, uint32_t n) {
    FT_LOCK;
    if (!RequireOpen("Write")) return false;
    DWORD written = 0;
    if (!Check(FT_Write(m_handle, const_cast<uint8_t*>(data), n, &written), "FT_Write")) return false;
    if (written != n) { m_lastError = "FT_Write: short write (TX timeout)"; return false; }
    return true;
}

int32_t Ftdi4232H::Read(uint8_t* data, uint32_t maxBytes) {
    FT_LOCK;
    if (!RequireOpen("Read")) return -1;
    DWORD available = 0;
    if (!Check(FT_GetQueueStatus(m_handle, &available), "FT_GetQueueStatus")) return -1;
    if (available == 0) return 0;
    DWORD got = 0;
    const DWORD want = (std::min)(available, static_cast<DWORD>(maxBytes));
    if (!Check(FT_Read(m_handle, data, want, &got), "FT_Read")) return -1;
    return static_cast<int32_t>(got);
}

bool Ftdi4232H::WaitRx(uint32_t timeoutUs) {
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    HANDLE event;
    { FT_LOCK; event = m_rxEvent; }
    for (;;) {
        DWORD available = 0;
        {
            FT_LOCK;
            if (!m_handle || FT_GetQueueStatus(m_handle, &available) != FT_OK) return false;
        }
        if (available > 0) return true;
        QueryPerformanceCounter(&now);
        const int64_t elapsedUs = (now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart;
        const int64_t left = static_cast<int64_t>(timeoutUs) - elapsedUs;
        if (left <= 0) return false;
        // The lock is not held while sleeping.
        if (left >= 1500 && event) WaitForSingleObject(event, static_cast<DWORD>(left / 1000));
        else YieldProcessor();
    }
}

bool Ftdi4232H::ConfigureUart(const FtdiUartConfig& c) {
    FT_LOCK;
    if (!RequireOpen("ConfigureUart")) return false;
    return Check(FT_SetBitMode(m_handle, 0, FT_BITMODE_RESET), "FT_SetBitMode") &&
           Check(FT_Purge(m_handle, FT_PURGE_RX | FT_PURGE_TX), "FT_Purge") &&
           Check(FT_SetUSBParameters(m_handle, c.usbInBytes, c.usbOutBytes), "FT_SetUSBParameters") &&
           Check(FT_SetChars(m_handle, 0, 0, 0, 0), "FT_SetChars") &&
           Check(FT_SetTimeouts(m_handle, c.readTimeoutMs, c.writeTimeoutMs), "FT_SetTimeouts") &&
           Check(FT_SetLatencyTimer(m_handle, c.latencyMs), "FT_SetLatencyTimer") &&
           Check(FT_SetFlowControl(m_handle, static_cast<USHORT>(c.flow), c.xon, c.xoff), "FT_SetFlowControl") &&
           Check(FT_SetBaudRate(m_handle, c.baud), "FT_SetBaudRate") &&
           Check(FT_SetDataCharacteristics(m_handle,
                     c.dataBits == 7 ? FT_BITS_7 : FT_BITS_8,
                     c.stopBits == 2 ? FT_STOP_BITS_2 : FT_STOP_BITS_1,
                     static_cast<UCHAR>(c.parity)), "FT_SetDataCharacteristics") &&
           Check(FT_Purge(m_handle, FT_PURGE_RX | FT_PURGE_TX), "FT_Purge");
}

#define FT_CALL(name, expr) FT_LOCK; if (!RequireOpen(name)) return false; return Check((expr), name)

bool Ftdi4232H::SetBaudRate(uint32_t baud) { FT_CALL("FT_SetBaudRate", FT_SetBaudRate(m_handle, baud)); }
bool Ftdi4232H::SetDivisor(uint16_t d) { FT_CALL("FT_SetDivisor", FT_SetDivisor(m_handle, d)); }
bool Ftdi4232H::SetDataCharacteristics(uint8_t bits, uint8_t stop, FtdiParity p) {
    FT_CALL("FT_SetDataCharacteristics", FT_SetDataCharacteristics(m_handle, bits == 7 ? FT_BITS_7 : FT_BITS_8, stop == 2 ? FT_STOP_BITS_2 : FT_STOP_BITS_1, static_cast<UCHAR>(p)));
}
bool Ftdi4232H::SetFlowControl(FtdiFlow f, uint8_t xon, uint8_t xoff) { FT_CALL("FT_SetFlowControl", FT_SetFlowControl(m_handle, static_cast<USHORT>(f), xon, xoff)); }
bool Ftdi4232H::SetChars(uint8_t ev, bool evOn, uint8_t er, bool erOn) { FT_CALL("FT_SetChars", FT_SetChars(m_handle, ev, evOn ? 1 : 0, er, erOn ? 1 : 0)); }
bool Ftdi4232H::SetTimeouts(uint32_t r, uint32_t w) { FT_CALL("FT_SetTimeouts", FT_SetTimeouts(m_handle, r, w)); }
bool Ftdi4232H::SetLatencyTimer(uint8_t ms) { FT_CALL("FT_SetLatencyTimer", FT_SetLatencyTimer(m_handle, ms)); }
bool Ftdi4232H::GetLatencyTimer(uint8_t* ms) { FT_CALL("FT_GetLatencyTimer", FT_GetLatencyTimer(m_handle, ms)); }
bool Ftdi4232H::SetUsbParameters(uint32_t in, uint32_t out) { FT_CALL("FT_SetUSBParameters", FT_SetUSBParameters(m_handle, in, out)); }
bool Ftdi4232H::SetDeadmanTimeout(uint32_t ms) { FT_CALL("FT_SetDeadmanTimeout", FT_SetDeadmanTimeout(m_handle, ms)); }
bool Ftdi4232H::SetDtr(bool on) { FT_CALL("FT_SetDtr", on ? FT_SetDtr(m_handle) : FT_ClrDtr(m_handle)); }
bool Ftdi4232H::SetRts(bool on) { FT_CALL("FT_SetRts", on ? FT_SetRts(m_handle) : FT_ClrRts(m_handle)); }
bool Ftdi4232H::SetBreak(bool on) { FT_CALL("FT_SetBreak", on ? FT_SetBreakOn(m_handle) : FT_SetBreakOff(m_handle)); }
bool Ftdi4232H::Purge(bool rx, bool tx) { FT_CALL("FT_Purge", FT_Purge(m_handle, (rx ? FT_PURGE_RX : 0u) | (tx ? FT_PURGE_TX : 0u))); }
bool Ftdi4232H::SetEventNotification(uint32_t mask, HANDLE ev) { FT_CALL("FT_SetEventNotification", FT_SetEventNotification(m_handle, mask, ev)); }
bool Ftdi4232H::StopInTask() { FT_CALL("FT_StopInTask", FT_StopInTask(m_handle)); }
bool Ftdi4232H::RestartInTask() { FT_CALL("FT_RestartInTask", FT_RestartInTask(m_handle)); }
bool Ftdi4232H::ResetDevice() { FT_CALL("FT_ResetDevice", FT_ResetDevice(m_handle)); }
bool Ftdi4232H::ResetPort() { FT_CALL("FT_ResetPort", FT_ResetPort(m_handle)); }
bool Ftdi4232H::CyclePort() { FT_CALL("FT_CyclePort", FT_CyclePort(m_handle)); }
bool Ftdi4232H::SetBitMode(uint8_t mask, uint8_t mode) { FT_CALL("FT_SetBitMode", FT_SetBitMode(m_handle, mask, mode)); }
bool Ftdi4232H::GetBitMode(uint8_t* pins) { FT_CALL("FT_GetBitMode", FT_GetBitMode(m_handle, pins)); }
bool Ftdi4232H::ReadEepromWord(uint32_t o, uint16_t* v) { FT_CALL("FT_ReadEE", FT_ReadEE(m_handle, o, reinterpret_cast<LPWORD>(v))); }
bool Ftdi4232H::WriteEepromWord(uint32_t o, uint16_t v) { FT_CALL("FT_WriteEE", FT_WriteEE(m_handle, o, v)); }
bool Ftdi4232H::EraseEeprom() { FT_CALL("FT_EraseEE", FT_EraseEE(m_handle)); }

bool Ftdi4232H::GetLineStatus(FtdiLineStatus* s) {
    FT_LOCK;
    if (!RequireOpen("GetLineStatus")) return false;
    ULONG v = 0;
    if (!Check(FT_GetModemStatus(m_handle, &v), "FT_GetModemStatus")) return false;
    const uint32_t line = (v >> 8) & 0xFF;
    m_stickyLineErrors |= line;
    if (s) {
        s->cts = (v & 0x10) != 0; s->dsr = (v & 0x20) != 0;
        s->ri  = (v & 0x40) != 0; s->dcd = (v & 0x80) != 0;
        s->overrun = (line & 0x02) != 0; s->parityError = (line & 0x04) != 0;
        s->framingError = (line & 0x08) != 0; s->breakInterrupt = (line & 0x10) != 0;
    }
    return true;
}

uint32_t Ftdi4232H::LastLineErrorBits() {
    FT_LOCK;
    FtdiLineStatus unused;
    GetLineStatus(&unused);
    const uint32_t v = m_stickyLineErrors & 0x1E;
    m_stickyLineErrors = 0;
    return v;
}

bool Ftdi4232H::GetQueueStatus(uint32_t* rx) {
    FT_LOCK;
    if (!RequireOpen("GetQueueStatus")) return false;
    DWORD v = 0;
    if (!Check(FT_GetQueueStatus(m_handle, &v), "FT_GetQueueStatus")) return false;
    if (rx) *rx = v;
    return true;
}

bool Ftdi4232H::GetStatus(uint32_t* rx, uint32_t* tx, uint32_t* ev) {
    FT_LOCK;
    if (!RequireOpen("GetStatus")) return false;
    DWORD a = 0, b = 0, c = 0;
    if (!Check(FT_GetStatus(m_handle, &a, &b, &c), "FT_GetStatus")) return false;
    if (rx) *rx = a;
    if (tx) *tx = b;
    if (ev) *ev = c;
    return true;
}

bool Ftdi4232H::GetDeviceInfo(FtdiDeviceInfo* info) {
    FT_LOCK;
    if (!RequireOpen("GetDeviceInfo")) return false;
    FT_DEVICE type = 0; DWORD id = 0; char serial[64] = {0}, desc[128] = {0};
    if (!Check(FT_GetDeviceInfo(m_handle, &type, &id, serial, desc, nullptr), "FT_GetDeviceInfo")) return false;
    if (info) {
        info->type = type; info->id = id; info->serial = serial; info->description = desc;
        info->channel = ChannelFrom(info->description, info->serial);
        info->isQuad = IsQuadType(type); info->isOpen = true;
    }
    return true;
}

bool Ftdi4232H::GetDriverVersion(uint32_t* version) {
    FT_LOCK;
    if (!RequireOpen("GetDriverVersion")) return false;
    DWORD v = 0;
    if (!Check(FT_GetDriverVersion(m_handle, &v), "FT_GetDriverVersion")) return false;
    if (version) *version = v;
    return true;
}

bool Ftdi4232H::GetComPortNumber(int32_t* com) {
    FT_LOCK;
    if (!RequireOpen("GetComPortNumber")) return false;
    LONG v = -1;
    if (!Check(FT_GetComPortNumber(m_handle, &v), "FT_GetComPortNumber")) return false;
    if (com) *com = v;
    return true;
}

bool Ftdi4232H::ReadEeprom(Ftdi4232HEeprom* out) {
    FT_LOCK;
    if (!RequireOpen("ReadEeprom") || !out) return false;
    char man[64] = {0}, manId[64] = {0}, desc[128] = {0}, ser[64] = {0};
    out->data.common.deviceType = FT_DEVICE_4232H;
    if (!Check(FT_EEPROM_Read(m_handle, &out->data, sizeof(out->data), man, manId, desc, ser), "FT_EEPROM_Read")) return false;
    out->manufacturer = man; out->manufacturerId = manId; out->description = desc; out->serial = ser;
    return true;
}

bool Ftdi4232H::ProgramEeprom(const Ftdi4232HEeprom& in) {
    FT_LOCK;
    if (!RequireOpen("ProgramEeprom")) return false;
    FT_EEPROM_4232H copy = in.data;
    copy.common.deviceType = FT_DEVICE_4232H;
    return Check(FT_EEPROM_Program(m_handle, &copy, sizeof(copy),
                     const_cast<char*>(in.manufacturer.c_str()), const_cast<char*>(in.manufacturerId.c_str()),
                     const_cast<char*>(in.description.c_str()), const_cast<char*>(in.serial.c_str())), "FT_EEPROM_Program");
}

bool Ftdi4232H::GetUserAreaSize(uint32_t* bytes) {
    FT_LOCK;
    if (!RequireOpen("FT_EE_UASize")) return false;
    DWORD v = 0;
    if (!Check(FT_EE_UASize(m_handle, &v), "FT_EE_UASize")) return false;
    if (bytes) *bytes = v;
    return true;
}

bool Ftdi4232H::ReadUserArea(uint8_t* data, uint32_t n, uint32_t* got) {
    FT_LOCK;
    if (!RequireOpen("FT_EE_UARead")) return false;
    DWORD v = 0;
    if (!Check(FT_EE_UARead(m_handle, data, n, &v), "FT_EE_UARead")) return false;
    if (got) *got = v;
    return true;
}

bool Ftdi4232H::WriteUserArea(const uint8_t* data, uint32_t n) {
    FT_CALL("FT_EE_UAWrite", FT_EE_UAWrite(m_handle, const_cast<uint8_t*>(data), n));
}

bool Ftdi4232H::IsRiTxDen(char channel, bool* enabled) {
    Ftdi4232HEeprom e;
    if (!ReadEeprom(&e)) return false;
    UCHAR v;
    switch (channel) {
    case 'A': v = e.data.ARIIsTXDEN; break;
    case 'B': v = e.data.BRIIsTXDEN; break;
    case 'C': v = e.data.CRIIsTXDEN; break;
    case 'D': v = e.data.DRIIsTXDEN; break;
    default:  return false;
    }
    if (enabled) *enabled = v != 0;
    return true;
}

} // namespace icd
