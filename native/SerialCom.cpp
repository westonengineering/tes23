#include "SerialCom.h"
#include <algorithm>

namespace icd {

#define COM_LOCK std::lock_guard<std::mutex> lock_(m_mutex)

SerialCom::SerialCom() { ZeroMemory(&m_commOv, sizeof(m_commOv)); }
SerialCom::~SerialCom() { Close(); }

bool SerialCom::Fail(const char* what) {
    const DWORD code = GetLastError();
    char text[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, sizeof(text) - 1, nullptr);
    m_lastError = std::string(what) + ": " + text;
    return false;
}

std::vector<std::string> SerialCom::EnumeratePorts() {
    std::vector<std::string> ports;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return ports;
    for (DWORD i = 0;; ++i) {
        char name[256], value[64];
        DWORD nameLen = sizeof(name), valueLen = sizeof(value) - 1, type = 0;
        const LONG r = RegEnumValueA(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<LPBYTE>(value), &valueLen);
        if (r != ERROR_SUCCESS) break;
        if (type == REG_SZ) { value[valueLen] = 0; ports.push_back(value); }
    }
    RegCloseKey(key);
    std::sort(ports.begin(), ports.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ports;
}

bool SerialCom::Open(const std::string& portName, const SerialConfig& cfg) {
    Close();
    std::lock_guard<std::mutex> lock_(m_mutex);
    const std::string path = portName.rfind("\\\\.\\", 0) == 0 ? portName : "\\\\.\\" + portName;
    m_handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) return Fail("CreateFile");
    m_readEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_commEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_readEvent || !m_writeEvent || !m_commEvent) return Fail("CreateEvent");

    if (!SetupComm(m_handle, cfg.rxQueueBytes, cfg.txQueueBytes)) return Fail("SetupComm");

    COMMTIMEOUTS t = {0};
    t.ReadIntervalTimeout = MAXDWORD; // ReadFile returns immediately with what is queued
    if (!SetCommTimeouts(m_handle, &t)) return Fail("SetCommTimeouts");
    if (!SetCommMask(m_handle, EV_RXCHAR)) return Fail("SetCommMask");
    m_writeTimeoutMs = cfg.writeTimeoutMs;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_handle, &dcb)) return Fail("GetCommState");
    dcb.BaudRate = cfg.baud;
    dcb.ByteSize = cfg.dataBits;
    dcb.Parity = static_cast<BYTE>(cfg.parity);
    dcb.fParity = cfg.parity != SerialParity::None;
    dcb.StopBits = cfg.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = cfg.flow == SerialFlow::RtsCts;
    dcb.fOutxDsrFlow = cfg.flow == SerialFlow::DtrDsr;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = dcb.fInX = cfg.flow == SerialFlow::XonXoff;
    dcb.fDtrControl = cfg.flow == SerialFlow::DtrDsr ? DTR_CONTROL_HANDSHAKE : (cfg.dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE);
    switch (cfg.flow == SerialFlow::RtsCts ? SerialRts::Handshake : cfg.rts) {
    case SerialRts::On:        dcb.fRtsControl = RTS_CONTROL_ENABLE; break;
    case SerialRts::Handshake: dcb.fRtsControl = RTS_CONTROL_HANDSHAKE; break;
    case SerialRts::Toggle:    dcb.fRtsControl = RTS_CONTROL_TOGGLE; break;
    default:                   dcb.fRtsControl = RTS_CONTROL_DISABLE; break;
    }
    dcb.fAbortOnError = FALSE;
    dcb.fNull = FALSE;
    dcb.fErrorChar = FALSE;
    if (!SetCommState(m_handle, &dcb)) return Fail("SetCommState");
    PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    return true;
}

bool SerialCom::Configure(const SerialConfig& cfg) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) { m_lastError = "Configure: port is not open"; return false; }
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_handle, &dcb)) return Fail("GetCommState");
    dcb.BaudRate = cfg.baud;
    dcb.ByteSize = cfg.dataBits;
    dcb.Parity = static_cast<BYTE>(cfg.parity);
    dcb.fParity = cfg.parity != SerialParity::None;
    dcb.StopBits = cfg.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    if (!SetCommState(m_handle, &dcb)) return Fail("SetCommState");
    return true;
}

bool SerialCom::IsOpen() const { COM_LOCK; return m_handle != INVALID_HANDLE_VALUE; }

void SerialCom::Close() {
    COM_LOCK;
    if (m_handle != INVALID_HANDLE_VALUE) {
        SetCommMask(m_handle, 0); // completes any pending WaitCommEvent
        CancelIoEx(m_handle, nullptr);
        if (m_commPending) {
            DWORD ignored = 0;
            GetOverlappedResult(m_handle, &m_commOv, &ignored, TRUE);
            m_commPending = false;
        }
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (m_readEvent)  { CloseHandle(m_readEvent);  m_readEvent = nullptr; }
    if (m_writeEvent) { CloseHandle(m_writeEvent); m_writeEvent = nullptr; }
    if (m_commEvent)  { CloseHandle(m_commEvent);  m_commEvent = nullptr; }
}

std::string SerialCom::LastError() const { COM_LOCK; return m_lastError; }

bool SerialCom::Write(const uint8_t* data, uint32_t n) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) { m_lastError = "Write: port is not open"; return false; }
    OVERLAPPED ov = {0};
    ov.hEvent = m_writeEvent;
    ResetEvent(m_writeEvent);
    DWORD written = 0;
    if (!WriteFile(m_handle, data, n, &written, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return Fail("WriteFile");
        const DWORD w = WaitForSingleObject(m_writeEvent, m_writeTimeoutMs);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(m_handle, &ov);
            GetOverlappedResult(m_handle, &ov, &written, TRUE);
            m_lastError = "WriteFile: TX timeout";
            return false;
        }
        if (!GetOverlappedResult(m_handle, &ov, &written, FALSE)) return Fail("GetOverlappedResult");
    }
    if (written != n) { m_lastError = "WriteFile: short write"; return false; }
    return true;
}

int32_t SerialCom::Read(uint8_t* data, uint32_t maxBytes) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) { m_lastError = "Read: port is not open"; return -1; }
    const uint32_t queued = RxQueuedLocked();
    if (queued == 0) return 0;
    OVERLAPPED ov = {0};
    ov.hEvent = m_readEvent;
    ResetEvent(m_readEvent);
    DWORD got = 0;
    if (!ReadFile(m_handle, data, (std::min)(queued, maxBytes), &got, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) { Fail("ReadFile"); return -1; }
        if (!GetOverlappedResult(m_handle, &ov, &got, TRUE)) { Fail("GetOverlappedResult"); return -1; }
    }
    return static_cast<int32_t>(got);
}

uint32_t SerialCom::RxQueuedLocked() {
    COMSTAT st;
    DWORD errors = 0;
    if (!ClearCommError(m_handle, &errors, &st)) return 0;
    return st.cbInQue;
}

uint32_t SerialCom::RxQueued() {
    COM_LOCK;
    return m_handle == INVALID_HANDLE_VALUE ? 0 : RxQueuedLocked();
}

bool SerialCom::WaitRx(uint32_t timeoutUs) {
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (;;) {
        HANDLE ev = nullptr;
        {
            COM_LOCK;
            if (m_handle == INVALID_HANDLE_VALUE) return false;
            if (RxQueuedLocked() > 0) return true;
            if (!m_commPending) {
                ZeroMemory(&m_commOv, sizeof(m_commOv));
                m_commOv.hEvent = m_commEvent;
                ResetEvent(m_commEvent);
                if (WaitCommEvent(m_handle, &m_commMask, &m_commOv)) continue; // completed at once
                if (GetLastError() != ERROR_IO_PENDING) return Fail("WaitCommEvent");
                m_commPending = true;
            }
            ev = m_commEvent;
        }
        QueryPerformanceCounter(&now);
        const int64_t elapsed = (now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart;
        const int64_t left = static_cast<int64_t>(timeoutUs) - elapsed;
        if (left <= 0) return false;
        if (left >= 1500) {
            if (WaitForSingleObject(ev, static_cast<DWORD>(left / 1000)) == WAIT_OBJECT_0) {
                COM_LOCK;
                if (m_handle != INVALID_HANDLE_VALUE && m_commPending) {
                    DWORD ignored = 0;
                    GetOverlappedResult(m_handle, &m_commOv, &ignored, FALSE);
                    m_commPending = false;
                }
            }
        } else {
            YieldProcessor();
        }
    }
}

bool SerialCom::Purge(bool rx, bool tx) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    DWORD flags = 0;
    if (rx) flags |= PURGE_RXCLEAR;
    if (tx) flags |= PURGE_TXCLEAR;
    return PurgeComm(m_handle, flags) ? true : Fail("PurgeComm");
}

bool SerialCom::Flush() {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    return FlushFileBuffers(m_handle) ? true : Fail("FlushFileBuffers");
}

bool SerialCom::SetDtr(bool on) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    return EscapeCommFunction(m_handle, on ? SETDTR : CLRDTR) ? true : Fail("EscapeCommFunction");
}

bool SerialCom::SetRts(bool on) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    return EscapeCommFunction(m_handle, on ? SETRTS : CLRRTS) ? true : Fail("EscapeCommFunction");
}

bool SerialCom::SetBreak(bool on) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    return (on ? SetCommBreak(m_handle) : ClearCommBreak(m_handle)) ? true : Fail("SetCommBreak");
}

bool SerialCom::GetLineStatus(SerialLineStatus* s) {
    COM_LOCK;
    if (m_handle == INVALID_HANDLE_VALUE || !s) return false;
    DWORD modem = 0, errors = 0;
    COMSTAT st;
    if (!GetCommModemStatus(m_handle, &modem)) return Fail("GetCommModemStatus");
    if (!ClearCommError(m_handle, &errors, &st)) return Fail("ClearCommError");
    s->cts = (modem & MS_CTS_ON) != 0; s->dsr = (modem & MS_DSR_ON) != 0;
    s->ring = (modem & MS_RING_ON) != 0; s->rlsd = (modem & MS_RLSD_ON) != 0;
    s->rxQueued = st.cbInQue; s->txQueued = st.cbOutQue; s->errors = errors;
    return true;
}

} // namespace icd
