// MatrixTests.cpp - runs every message on its channel, with periodic sending on
// and off, and with the receiver hearing its own transmit (half duplex) or not.
// Driven by the schema, so it works unchanged with any generated ICD.
// Usage: IcdMatrixTests [dump-file]. Exit code is the number of failed checks.
#include "LinkEngine.h"
#include "SimDevice.h"
#include "IcdMessages.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

using namespace icd;

static int g_failures = 0, g_checks = 0, g_caseFailures = 0;

#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_failures; ++g_caseFailures; printf("    FAIL line %d: %s  ", __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Sits between the engine and the simulated device and records the raw bytes.
class Spy : public IByteTransport {
public:
    explicit Spy(std::unique_ptr<SimDevice> sim) : m_sim(std::move(sim)) {}
    SimDevice& Sim() { return *m_sim; }
    bool IsOpen() const override { return m_sim->IsOpen(); }
    void Close() override { m_sim->Close(); }
    std::string LastError() const override { return m_sim->LastError(); }
    bool WaitRx(uint32_t us) override { return m_sim->WaitRx(us); }
    bool Write(const uint8_t* d, uint32_t n) override {
        std::lock_guard<std::mutex> g(m_mutex);
        m_lastTx.assign(d, d + n);
        m_times.push_back(NowUs());
        return m_sim->Write(d, n);
    }
    int32_t Read(uint8_t* d, uint32_t max) override {
        const int32_t n = m_sim->Read(d, max);
        if (n > 0) { std::lock_guard<std::mutex> g(m_mutex); m_rx.insert(m_rx.end(), d, d + n); }
        return n;
    }
    size_t Writes() { std::lock_guard<std::mutex> g(m_mutex); return m_times.size(); }
    std::vector<int64_t> Times() { std::lock_guard<std::mutex> g(m_mutex); return m_times; }
    std::vector<uint8_t> LastTx() { std::lock_guard<std::mutex> g(m_mutex); return m_lastTx; }
    std::vector<uint8_t> Rx() { std::lock_guard<std::mutex> g(m_mutex); return m_rx; }
    void ClearRx() { std::lock_guard<std::mutex> g(m_mutex); m_rx.clear(); }
private:
    std::unique_ptr<SimDevice> m_sim;
    std::mutex m_mutex;
    std::vector<uint8_t> m_lastTx, m_rx;
    std::vector<int64_t> m_times;
};

static uint64_t g_state = 0x9E3779B97F4A7C15ull;
static uint64_t Next() { g_state ^= g_state << 13; g_state ^= g_state >> 7; g_state ^= g_state << 17; return g_state; }

static std::vector<uint8_t> RandomImage(const MsgDesc& m) {
    std::vector<uint8_t> image(m.txLen, 0);
    image[0] = m.id;
    for (int f = 0; f < m.fieldCount; ++f) SetRaw(image.data(), m.fields[f], Next());
    return image;
}

// What the device should return for a given sent image.
static std::vector<uint8_t> ExpectedReply(const MsgDesc& m, const std::vector<uint8_t>& tx) {
    std::vector<uint8_t> e(tx.begin(), tx.begin() + m.rxLen);
    e[0] = m.rxId;
    for (int f = 0; f < m.fieldCount; ++f) {
        const FieldDesc& d = m.fields[f];
        if (!(d.dir & DirFrom) && d.byteOffset + d.byteCount <= m.rxLen) SetRaw(e.data(), d, 0);
    }
    return e;
}

struct Rig {
    LinkEngine engine;
    Spy* spy = nullptr;
    int ch;
    ChannelConfig cfg;
    Rig(const Schema& s, int channel, bool echo, bool ff) : engine(s), ch(channel) {
        const ChannelDefaults& d = s.channels[channel];
        cfg = ChannelConfig{ d.baud, d.periodUs, d.windowUs, d.periodUs - d.periodUs / 10, 400, ff ? 1u : 0u, echo ? 1u : 0u, 0 };
        auto sim = std::make_unique<SimDevice>(s, channel == 0 ? 'C' : 'D', cfg);
        auto owned = std::make_unique<Spy>(std::move(sim));
        spy = owned.get();
        const Status st = engine.Attach(channel, std::move(owned), cfg);
        CHECK(st == Ok, "attach returned %d: %s", int(st), engine.LastError().c_str());
    }
    uint64_t Commit(int msg, const std::vector<uint8_t>& image) {
        uint64_t token = 0, seq = 0;
        engine.TxnBegin(1000, &token);
        int32_t id = msg;
        engine.Commit(token, &id, 1, image.data(), &seq);
        engine.TxnEnd(token);
        return seq;
    }
    void OnlyThisMessage(const Schema& s, int msg) {
        for (int i = 0; i < s.messageCount; ++i) engine.SetCyclicMessage(i, i == msg);
    }
};

static void Dump(FILE* f, const char* name, const MsgDesc& m, const std::vector<uint8_t>& tx, const uint8_t* rx, size_t rxBytes) {
    if (!f) return;
    fprintf(f, "%s %d %d ", name, m.txLen, m.rxLen);
    for (uint8_t b : tx) fprintf(f, "%02x", b);
    fputc(' ', f);
    for (size_t i = 0; i < rxBytes; ++i) fprintf(f, "%02x", rx[i]);
    fputc('\n', f);
}

static void RunManual(const Schema& s, int msg, bool echo, bool ff, FILE* dump) {
    const MsgDesc& m = s.messages[msg];
    const int ch = ChannelIndex(m.channel);
    Rig rig(s, ch, echo, ff);
    const size_t tail = ff ? 1u : 0u, txWire = m.txLen + 4u + tail, rxWire = m.rxLen + 4u + tail;
    const size_t perExchange = rxWire + (echo ? txWire : 0);
    const int rounds = 20;

    for (int r = 0; r < rounds; ++r) {
        const std::vector<uint8_t> tx = RandomImage(m);
        rig.Commit(msg, tx);
        rig.spy->ClearRx();
        std::vector<uint8_t> rx(m.rxLen);
        const Status st = rig.engine.Exchange(msg, 1000, rx.data());
        CHECK(st == Ok, "exchange returned %d", int(st));
        CHECK(std::memcmp(rx.data(), ExpectedReply(m, tx).data(), m.rxLen) == 0, "reply content differs, round %d", r);
        const std::vector<uint8_t> wireTx = rig.spy->LastTx();
        const std::vector<uint8_t> wireRx = rig.spy->Rx();
        CHECK(wireTx.size() == txWire, "tx wire %zu", wireTx.size());
        CHECK(wireRx.size() == perExchange, "rx wire %zu, expected %zu", wireRx.size(), perExchange);
        if (r == 0 && wireRx.size() == perExchange) {
            char name[64];
            sprintf_s(name, "%c_%s_manual_echo%d_ff%d", m.channel, m.name, echo ? 1 : 0, ff ? 1 : 0);
            if (ff) CHECK(wireTx.back() == 0xFF && wireRx.back() == 0xFF, "missing end marker");
            Dump(dump, name, m, std::vector<uint8_t>(wireTx.begin(), wireTx.end() - tail), wireRx.data() + (echo ? txWire : 0), rxWire - tail);
        }
    }
    const size_t writes = rig.spy->Writes();
    CHECK(writes == size_t(rounds), "with periodic off exactly %d frames should go out, saw %zu", rounds, writes);
    Sleep(rig.cfg.periodUs * 5 / 1000);
    CHECK(rig.spy->Writes() == writes, "the link transmitted with periodic off");

    ChannelStats st;
    rig.engine.GetStats(ch, &st);
    CHECK(st.rxFrames == uint64_t(rounds), "rx %llu", (unsigned long long)st.rxFrames);
    CHECK(st.crcErrors == 0 && st.rxTimeouts == 0 && st.strayBytes == 0 && st.echoErrors == 0 && st.ioErrors == 0,
          "crc %llu timeouts %llu stray %llu echo %llu", (unsigned long long)st.crcErrors, (unsigned long long)st.rxTimeouts,
          (unsigned long long)st.strayBytes, (unsigned long long)st.echoErrors);
    printf("  %c %-12s manual   echo %-3s ff %-3s  tx %3zu  rx %3zu  exchange %6llu us  %d frames\n", m.channel, m.name, echo ? "on" : "off", ff ? "on" : "off",
           txWire, rxWire, (unsigned long long)st.lastExchangeUs, rounds);
}

static void RunPeriodic(const Schema& s, int msg, bool echo, bool ff, FILE* dump) {
    const MsgDesc& m = s.messages[msg];
    const int ch = ChannelIndex(m.channel);
    Rig rig(s, ch, echo, ff);
    rig.OnlyThisMessage(s, msg);
    const uint32_t period = rig.cfg.periodUs;
    const size_t tail = ff ? 1u : 0u, txWire = m.txLen + 4u + tail, rxWire = m.rxLen + 4u + tail;
    const size_t perExchange = rxWire + (echo ? txWire : 0);

    CHECK(rig.engine.SetCyclic(ch, true) == Ok, "cyclic on");
    for (int r = 0; r < 20; ++r) {
        const std::vector<uint8_t> tx = RandomImage(m);
        const uint64_t seq = rig.Commit(msg, tx);
        const Status st = rig.engine.WaitApplied(seq, period / 1000 * 10);
        CHECK(st == Ok, "sync returned %d, round %d", int(st), r);
        std::vector<uint8_t> rx(m.rxLen);
        MsgMeta meta;
        rig.engine.ReadMsg(msg, 1, rx.data(), &meta);
        CHECK(meta.rxTag >= seq, "reply predates the commit");
        CHECK(std::memcmp(rx.data(), ExpectedReply(m, tx).data(), m.rxLen) == 0, "reply content differs, round %d", r);
    }
    Sleep(period / 1000 * 40);
    rig.engine.SetCyclic(ch, false);
    Sleep(period / 1000 * 3);

    ChannelStats st;
    rig.engine.GetStats(ch, &st);
    const std::vector<int64_t> times = rig.spy->Times();
    CHECK(times.size() > 40, "only %zu frames", times.size());
    double meanGap = 0;
    int64_t minGap = INT64_MAX, maxGap = 0;
    if (times.size() > 1) {
        meanGap = double(times.back() - times.front()) / double(times.size() - 1);
        for (size_t i = 1; i < times.size(); ++i) {
            minGap = (std::min)(minGap, times[i] - times[i - 1]);
            maxGap = (std::max)(maxGap, times[i] - times[i - 1]);
        }
    }
    CHECK(std::abs(meanGap - period) < period * 0.01, "mean gap %.1f us, period %u", meanGap, period);
    CHECK(st.missedSlots == 0, "missed slots %llu", (unsigned long long)st.missedSlots);
    CHECK(st.windowMisses == 0, "window misses %llu", (unsigned long long)st.windowMisses);
    CHECK(st.rxFrames == st.txFrames, "tx %llu rx %llu", (unsigned long long)st.txFrames, (unsigned long long)st.rxFrames);
    CHECK(st.crcErrors == 0 && st.rxTimeouts == 0 && st.strayBytes == 0 && st.echoErrors == 0 && st.ioErrors == 0,
          "crc %llu timeouts %llu stray %llu echo %llu", (unsigned long long)st.crcErrors, (unsigned long long)st.rxTimeouts,
          (unsigned long long)st.strayBytes, (unsigned long long)st.echoErrors);
    const std::vector<uint8_t> wireRx = rig.spy->Rx();
    CHECK(wireRx.size() == st.txFrames * perExchange, "rx bytes %zu, expected %llu", wireRx.size(), (unsigned long long)(st.txFrames * perExchange));
    CHECK(rig.spy->LastTx().size() == txWire, "tx wire %zu", rig.spy->LastTx().size());
    if (wireRx.size() >= perExchange) {
        char name[64];
        sprintf_s(name, "%c_%s_periodic_echo%d_ff%d", m.channel, m.name, echo ? 1 : 0, ff ? 1 : 0);
        const std::vector<uint8_t> lastTx = rig.spy->LastTx();
        Dump(dump, name, m, std::vector<uint8_t>(lastTx.begin(), lastTx.end() - tail), wireRx.data() + wireRx.size() - rxWire, rxWire - tail);
    }
    printf("  %c %-12s periodic echo %-3s ff %-3s  tx %3zu  rx %3zu  exchange %6llu us  %zu frames  gap mean %.1f min %lld max %lld us  late max %llu us\n",
           m.channel, m.name, echo ? "on" : "off", ff ? "on" : "off", txWire, rxWire, (unsigned long long)st.lastExchangeUs, times.size(), meanGap,
           (long long)minGap, (long long)maxGap, (unsigned long long)st.maxLatenessUs);
}

// A silent device must not cost the next window.
static void RunSilentDevice(const Schema& s, int msg, bool echo, bool ff) {
    const MsgDesc& m = s.messages[msg];
    if (!m.hasResponse) return;
    const int ch = ChannelIndex(m.channel);
    Rig rig(s, ch, echo, ff);
    rig.OnlyThisMessage(s, msg);
    rig.spy->Sim().SetFaults(5, 0, 200);
    CHECK(rig.engine.SetCyclic(ch, true) == Ok, "cyclic on");
    Sleep(rig.cfg.periodUs / 1000 * 50);
    rig.engine.SetCyclic(ch, false);
    Sleep(rig.cfg.periodUs / 1000 * 3);
    ChannelStats st;
    rig.engine.GetStats(ch, &st);
    CHECK(st.rxTimeouts > 0, "the fault was not injected");
    CHECK(st.missedSlots == 0, "missed slots %llu after a silent response", (unsigned long long)st.missedSlots);
    CHECK(st.rxFrames + st.rxTimeouts == st.txFrames, "tx %llu rx %llu timeouts %llu",
          (unsigned long long)st.txFrames, (unsigned long long)st.rxFrames, (unsigned long long)st.rxTimeouts);
    CHECK(st.crcErrors == 0, "crc %llu", (unsigned long long)st.crcErrors);
    printf("  %c %-12s no reply every 5th, echo %-3s ff %-3s  tx %llu  rx %llu  timeouts %llu  missed slots %llu\n", m.channel, m.name, echo ? "on" : "off", ff ? "on" : "off",
           (unsigned long long)st.txFrames, (unsigned long long)st.rxFrames, (unsigned long long)st.rxTimeouts, (unsigned long long)st.missedSlots);
}

int main(int argc, char** argv) {
    const Schema& s = icdgen::GetSchema();
    FILE* dump = nullptr;
    if (argc > 1) fopen_s(&dump, argv[1], "w");

    for (int msg = 0; msg < s.messageCount; ++msg) {
        for (int echo = 0; echo < 2; ++echo) {
            for (int ff = 0; ff < 2; ++ff) {
                RunManual(s, msg, echo != 0, ff != 0, dump);
                RunPeriodic(s, msg, echo != 0, ff != 0, dump);
                RunSilentDevice(s, msg, echo != 0, ff != 0);
            }
        }
    }
    if (dump) fclose(dump);
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}
