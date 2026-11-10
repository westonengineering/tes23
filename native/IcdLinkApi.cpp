#include "IcdLinkApi.h"
#include "IcdMessages.h"
#include "Ftdi4232H.h"
#include "SerialCom.h"
#include "SimDevice.h"
#include "LinkEngine.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

using namespace icd;

static_assert(sizeof(IL_ChannelConfig) == sizeof(ChannelConfig), "config layout");
static_assert(sizeof(IL_ChannelStats) == sizeof(ChannelStats), "stats layout");
static_assert(sizeof(IL_MsgMeta) == sizeof(MsgMeta), "meta layout");

namespace {

struct Context {
    LinkEngine engine;
    SimDevice* sim[2] = { nullptr, nullptr }; // owned by the engine
    std::mutex warningMutex;
    std::string warning;
    Context() : engine(icdgen::GetSchema()) {}
};

Context* Ctx(IL_Handle h) { return static_cast<Context*>(h); }

ChannelConfig Convert(const IL_ChannelConfig* c, int ch) {
    if (c) { ChannelConfig out; std::memcpy(&out, c, sizeof(out)); return out; }
    IL_ChannelConfig d;
    IL_DefaultConfig(ch, &d);
    ChannelConfig out;
    std::memcpy(&out, &d, sizeof(out));
    return out;
}

int32_t CopyText(const std::string& s, char* buffer, int32_t capacity) {
    if (!buffer || capacity <= 0) return static_cast<int32_t>(s.size());
    const size_t n = (std::min)(s.size(), static_cast<size_t>(capacity - 1));
    std::memcpy(buffer, s.data(), n);
    buffer[n] = 0;
    return static_cast<int32_t>(n);
}

} // namespace

#define IL_CTX Context* c = Ctx(h); if (!c) return ErrArg

extern "C" {

int32_t IL_Create(IL_Handle* out) {
    if (!out) return ErrArg;
    *out = new Context();
    return Ok;
}

void IL_Destroy(IL_Handle h) { delete Ctx(h); }

uint32_t IL_SchemaHash(void) { return icdgen::GetSchema().hash; }

int32_t IL_DefaultConfig(int32_t ch, IL_ChannelConfig* out) {
    if (ch < 0 || ch > 1 || !out) return ErrArg;
    const ChannelDefaults& d = icdgen::GetSchema().channels[ch];
    out->baud = d.baud; out->periodUs = d.periodUs; out->windowUs = d.windowUs;
    out->rxTimeoutUs = d.rxTimeoutUs; out->spinUs = 400;
    out->trailerFf = d.trailerFf ? 1 : 0; out->rs485Echo = d.rs485Echo ? 1 : 0;
    out->reserved = 0;
    return Ok;
}

int32_t IL_ListFtdi(char* buffer, int32_t capacity) {
    std::vector<FtdiDeviceInfo> list;
    std::string error;
    if (!Ftdi4232H::Enumerate(list, &error)) { CopyText(error, buffer, capacity); return ErrIo; }
    std::string text;
    for (const FtdiDeviceInfo& d : list) {
        text += std::to_string(d.index) + "\t" + d.SerialBase() + "\t" +
                (d.channel ? std::string(1, d.channel) : std::string("?")) + "\t" + d.description + "\t" +
                (d.isQuad ? "quad" : "other") + "\t" + (d.isOpen ? "open" : "free") + "\n";
    }
    return CopyText(text, buffer, capacity);
}

int32_t IL_ListCom(char* buffer, int32_t capacity) {
    std::string text;
    for (const std::string& p : SerialCom::EnumeratePorts()) text += p + "\n";
    return CopyText(text, buffer, capacity);
}

int32_t IL_LastError(IL_Handle h, char* buffer, int32_t capacity) {
    IL_CTX;
    return CopyText(c->engine.LastError(), buffer, capacity);
}

int32_t IL_LastWarning(IL_Handle h, char* buffer, int32_t capacity) {
    IL_CTX;
    std::lock_guard<std::mutex> g(c->warningMutex);
    const int32_t n = CopyText(c->warning, buffer, capacity);
    c->warning.clear();
    return n;
}

int32_t IL_OpenFtdi(IL_Handle h, int32_t ch, const char* serialBase, const IL_ChannelConfig* cfg) {
    IL_CTX;
    if (ch < 0 || ch > 1) return ErrArg;
    c->engine.Detach(ch);
    c->sim[ch] = nullptr;
    const ChannelConfig config = Convert(cfg, ch);
    const char letter = ch == 0 ? 'C' : 'D';

    auto port = std::make_unique<Ftdi4232H>();
    if (!port->Open(letter, serialBase ? serialBase : "")) { c->engine.SetError(port->LastError()); return ErrNotOpen; }

    FtdiUartConfig uart;
    uart.baud = config.baud;
    uart.dataBits = 8;
    uart.stopBits = 1;
    uart.parity = FtdiParity::Odd;
    uart.flow = FtdiFlow::None;
    uart.latencyMs = 1;
    if (!port->ConfigureUart(uart)) { c->engine.SetError(port->LastError()); return ErrIo; }

    if (ch == 0) {
        // Channel C drives RS-485 from TXDEN, a setting held in the FTDI EEPROM.
        bool txden = false;
        std::string warn;
        if (!port->IsRiTxDen('C', &txden)) warn = "could not read the FTDI EEPROM to confirm channel C RS-485 TXDEN";
        else if (!txden) warn = "FTDI EEPROM has RI-as-TXDEN turned off for channel C, the RS-485 driver may never enable";
        if (!warn.empty()) { std::lock_guard<std::mutex> g(c->warningMutex); c->warning = warn; }
    }
    return c->engine.Attach(ch, std::move(port), config);
}

int32_t IL_OpenCom(IL_Handle h, int32_t ch, const char* name, const IL_ChannelConfig* cfg) {
    IL_CTX;
    if (ch < 0 || ch > 1 || !name) return ErrArg;
    c->engine.Detach(ch);
    c->sim[ch] = nullptr;
    const ChannelConfig config = Convert(cfg, ch);
    auto port = std::make_unique<SerialCom>();
    SerialConfig sc;
    sc.baud = config.baud;
    sc.parity = SerialParity::Odd;
    if (!port->Open(name, sc)) { c->engine.SetError(port->LastError()); return ErrNotOpen; }
    return c->engine.Attach(ch, std::move(port), config);
}

int32_t IL_OpenSim(IL_Handle h, int32_t ch, const IL_ChannelConfig* cfg) {
    IL_CTX;
    if (ch < 0 || ch > 1) return ErrArg;
    c->engine.Detach(ch);
    const ChannelConfig config = Convert(cfg, ch);
    auto sim = std::make_unique<SimDevice>(icdgen::GetSchema(), ch == 0 ? 'C' : 'D', config);
    c->sim[ch] = sim.get();
    return c->engine.Attach(ch, std::move(sim), config);
}

int32_t IL_SimSetFaults(IL_Handle h, int32_t ch, uint32_t dropEvery, uint32_t corruptEvery, uint32_t delayUs) {
    IL_CTX;
    if (ch < 0 || ch > 1 || !c->sim[ch]) return ErrArg;
    c->sim[ch]->SetFaults(dropEvery, corruptEvery, delayUs);
    return Ok;
}

int32_t IL_Close(IL_Handle h, int32_t ch) {
    IL_CTX;
    const Status s = c->engine.Detach(ch);
    if (ch >= 0 && ch < 2) c->sim[ch] = nullptr;
    return s;
}

int32_t IL_IsOpen(IL_Handle h, int32_t ch) { IL_CTX; return c->engine.IsOpen(ch) ? 1 : 0; }
int32_t IL_SetCyclic(IL_Handle h, int32_t ch, int32_t e) { IL_CTX; return c->engine.SetCyclic(ch, e != 0); }
int32_t IL_SetCyclicMessage(IL_Handle h, int32_t m, int32_t e) { IL_CTX; return c->engine.SetCyclicMessage(m, e != 0); }
int32_t IL_SetPeriod(IL_Handle h, int32_t ch, uint32_t us) { IL_CTX; return c->engine.SetPeriod(ch, us); }
int32_t IL_TxnBegin(IL_Handle h, uint32_t ms, uint64_t* t) { IL_CTX; return c->engine.TxnBegin(ms, t); }
int32_t IL_TxnEnd(IL_Handle h, uint64_t t) { IL_CTX; return c->engine.TxnEnd(t); }
int32_t IL_Commit(IL_Handle h, uint64_t t, const int32_t* m, int32_t n, const uint8_t* f, uint64_t* s) { IL_CTX; return c->engine.Commit(t, m, n, f, s); }
int32_t IL_WaitApplied(IL_Handle h, uint64_t s, uint32_t ms) { IL_CTX; return c->engine.WaitApplied(s, ms); }
int32_t IL_Snapshot(IL_Handle h, uint8_t* tx, uint8_t* rx, IL_MsgMeta* m, uint64_t* s) { IL_CTX; return c->engine.Snapshot(tx, rx, reinterpret_cast<MsgMeta*>(m), s); }
int32_t IL_ReadMsg(IL_Handle h, int32_t m, int32_t side, uint8_t* o, IL_MsgMeta* meta) { IL_CTX; return c->engine.ReadMsg(m, side, o, reinterpret_cast<MsgMeta*>(meta)); }
int32_t IL_Exchange(IL_Handle h, int32_t m, uint32_t ms, uint8_t* o) { IL_CTX; return c->engine.Exchange(m, ms, o); }
int32_t IL_LongOpBegin(IL_Handle h, int32_t ch, uint32_t w, uint32_t ms) { IL_CTX; return c->engine.LongOpBegin(ch, w, ms); }
int32_t IL_LongOpEnd(IL_Handle h, int32_t ch) { IL_CTX; return c->engine.LongOpEnd(ch); }
int32_t IL_GetStats(IL_Handle h, int32_t ch, IL_ChannelStats* o) { IL_CTX; return c->engine.GetStats(ch, reinterpret_cast<ChannelStats*>(o)); }
int32_t IL_ResetStats(IL_Handle h, int32_t ch) { IL_CTX; return c->engine.ResetStats(ch); }

} // extern "C"
