// EngineTests.cpp - tests of the deterministic engine against SimDevice.
// Tests that depend on a specific ICD use the Fake ICD (channel C) and the
// DemoChannelD placeholder. Exit code is the number of failed checks.
#include "LinkEngine.h"
#include "SimDevice.h"
#include "IcdMessages.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace icd;
using namespace icdgen;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++g_failures; printf("  FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); } } while (0)

static const int C = 0, D = 1;
static const int kFake = C_FakeIcd_To::kMsgIndex;
static const int kTelemetry = D_Telemetry_To::kMsgIndex;
static const int kEeprom = D_EepromWrite_To::kMsgIndex;

static ChannelConfig FastConfig(uint32_t periodUs = 2000) {
    ChannelConfig c = {};
    c.baud = 921600; c.periodUs = periodUs; c.windowUs = 1000; c.rxTimeoutUs = periodUs / 2; c.spinUs = 400;
    return c;
}

struct Rig {
    LinkEngine engine;
    SimDevice* sim[2];
    explicit Rig(ChannelConfig cfg = FastConfig()) : engine(GetSchema()) {
        for (int ch = 0; ch < 2; ++ch) {
            auto s = std::make_unique<SimDevice>(GetSchema(), ch == 0 ? 'C' : 'D', cfg);
            sim[ch] = s.get();
            CHECK_EQ(engine.Attach(ch, std::move(s), cfg), Ok);
        }
    }
    void CommitFake(const C_FakeIcd_To& m, uint64_t* seq = nullptr) {
        uint64_t token = 0;
        CHECK_EQ(engine.TxnBegin(1000, &token), Ok);
        uint8_t image[C_FakeIcd_To::kLen] = {0};
        m.Pack(image);
        int32_t id = kFake;
        CHECK_EQ(engine.Commit(token, &id, 1, image, seq), Ok);
        CHECK_EQ(engine.TxnEnd(token), Ok);
    }
};

static void TestCrc() {
    puts("crc");
    Crc32 crc;
    CHECK_EQ(crc.Compute(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
}

static void TestPack() {
    puts("pack and unpack against the Fake ICD bit positions");
    C_FakeIcd_To t;
    t.Power = true; t.Test4 = 0xA; t.Test1 = 3; t.Test2 = 5; t.Test3 = 0x1F;
    uint8_t image[5] = {0};
    t.Pack(image);
    CHECK_EQ(image[0], 0x90);
    CHECK_EQ(image[1], 0x15);  // bit 0 Power, bits 4-1 Test4
    CHECK_EQ(image[2], 0x03);  // bits 3-0 Test1
    CHECK_EQ(image[3], 0x50);  // bits 7-4 Test2
    CHECK_EQ(image[4], 0x1F);  // bits 4-0 Test3
    C_FakeIcd_From f;
    f.Unpack(image);
    CHECK(f.Power); CHECK_EQ(f.Test4, 0xA); CHECK_EQ(f.Test1, 3); CHECK_EQ(f.Test2, 5); CHECK_EQ(f.Test3, 0x1F);

    D_Telemetry_To d;
    d.Enable = true; d.Mode = D_Telemetry_Mode::Fault; d.TempC = -12.3; d.Gain = 2.5f; d.CommandOnly = 0x7E;
    uint8_t di[9] = {0};
    d.Pack(di);
    D_Telemetry_To back;
    back.Unpack(di);
    CHECK(back.Enable);
    CHECK(back.Mode == D_Telemetry_Mode::Fault);
    CHECK(std::abs(back.TempC + 12.3) < 0.051);
    CHECK_EQ(back.Gain, 2.5f);
    CHECK_EQ(di[2] | (di[3] << 8), 0x10000 - 123); // -12.3 / 0.1 as little-endian int16
}

static void TestWireFormat() {
    puts("wire format and CRC");
    Rig rig;
    std::mutex m;
    std::vector<uint8_t> captured;
    rig.sim[C]->SetFrameHook([&](const MsgDesc&, const uint8_t* image, int64_t) {
        std::lock_guard<std::mutex> g(m);
        captured.assign(image, image + 9);
    });
    C_FakeIcd_To t;
    t.Power = true; t.Test4 = 0xA; t.Test1 = 3; t.Test2 = 5; t.Test3 = 0x1F;
    rig.CommitFake(t);
    uint8_t rx[5] = {0};
    CHECK_EQ(rig.engine.Exchange(kFake, 200, rx), Ok);
    std::lock_guard<std::mutex> g(m);
    CHECK_EQ(captured.size(), 9u);
    const uint8_t expected[9] = { 0x90, 0x15, 0x03, 0x50, 0x1F, 0, 0, 0, 0 };
    CHECK(std::memcmp(captured.data(), expected, 5) == 0);
    // Reference value from Python zlib.crc32 of 90 15 03 50 1F.
    uint32_t crc = captured[5] | (captured[6] << 8) | (captured[7] << 16) | (uint32_t(captured[8]) << 24);
    CHECK_EQ(crc, 0x45169958u);
    CHECK(std::memcmp(rx, expected, 5) == 0);
}

static void TestNoTearing() {
    puts("no torn commits in any frame on the wire (2 s stress)");
    Rig rig;
    std::atomic<uint64_t> torn{0}, frames{0};
    rig.sim[C]->SetFrameHook([&](const MsgDesc& d, const uint8_t* image, int64_t) {
        if (d.id != 0x90) return;
        frames++;
        C_FakeIcd_From f;
        f.Unpack(image);
        if (f.Test1 != f.Test2 || f.Test1 != f.Test4) torn++;
    });
    CHECK_EQ(rig.engine.SetCyclic(C, true), Ok);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> commits{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&, w] {
            uint8_t v = static_cast<uint8_t>(w);
            while (!stop.load()) {
                C_FakeIcd_To t;
                v = (v + 1) & 0xF;
                t.Test1 = v; t.Test2 = v; t.Test4 = v;
                rig.CommitFake(t);
                commits++;
            }
        });
    }
    Sleep(2000);
    stop = true;
    for (auto& t : writers) t.join();
    printf("  %llu commits, %llu frames\n", (unsigned long long)commits.load(), (unsigned long long)frames.load());
    CHECK(commits.load() > 1000);
    CHECK(frames.load() > 500);
    CHECK_EQ(torn.load(), 0ull);
}

static void TestCrossChannelCommit() {
    puts("a commit spanning both channels is seen whole (1 s stress)");
    Rig rig;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> crossTorn{0}, crossReads{0};
    std::thread crossWriter([&] {
        uint8_t v = 0;
        while (!stop.load()) {
            uint64_t token = 0;
            if (rig.engine.TxnBegin(1000, &token) != Ok) continue;
            v = (v + 1) & 0x1F;
            C_FakeIcd_To c; c.Test3 = v;
            D_Telemetry_To d; d.CommandOnly = v;
            std::vector<uint8_t> flat(C_FakeIcd_To::kLen + D_Telemetry_To::kLen);
            c.Pack(flat.data());
            d.Pack(flat.data() + C_FakeIcd_To::kLen);
            int32_t ids[2] = { kFake, kTelemetry };
            rig.engine.Commit(token, ids, 2, flat.data(), nullptr);
            rig.engine.TxnEnd(token);
        }
    });
    std::thread crossReader([&] {
        const Schema& s = GetSchema();
        std::vector<uint8_t> tx(s.txTotal);
        while (!stop.load()) {
            rig.engine.Snapshot(tx.data(), nullptr, nullptr, nullptr);
            C_FakeIcd_To c; c.Unpack(tx.data());
            D_Telemetry_To d; d.Unpack(tx.data() + C_FakeIcd_To::kLen);
            crossReads++;
            if (c.Test3 != d.CommandOnly) crossTorn++;
        }
    });
    Sleep(1000);
    stop = true;
    crossWriter.join();
    crossReader.join();
    printf("  %llu cross-channel reads\n", (unsigned long long)crossReads.load());
    CHECK(crossReads.load() > 1000);
    CHECK_EQ(crossTorn.load(), 0ull);
}

static void TestWaitApplied() {
    puts("Sync: reading after WaitApplied always reflects the commit");
    Rig rig;
    CHECK_EQ(rig.engine.SetCyclic(C, true), Ok);
    for (int i = 0; i < 300; ++i) {
        C_FakeIcd_To t;
        t.Test3 = static_cast<uint8_t>(i & 0x1F);
        uint64_t seq = 0;
        rig.CommitFake(t, &seq);
        CHECK_EQ(rig.engine.WaitApplied(seq, 500), Ok);
        uint8_t rx[5];
        MsgMeta meta;
        CHECK_EQ(rig.engine.ReadMsg(kFake, 1, rx, &meta), Ok);
        C_FakeIcd_From f;
        f.Unpack(rx);
        CHECK_EQ(f.Test3, t.Test3);
        CHECK(meta.rxTag >= seq);
    }
}

static void TestGate() {
    puts("transaction gate serialises read-modify-write");
    Rig rig;
    uint64_t rogue = 123456;
    int32_t id = kFake;
    uint8_t image[5] = {0x90};
    CHECK_EQ(rig.engine.Commit(rogue, &id, 1, image, nullptr), ErrNotOwner);

    const int perThread = 4000;
    std::vector<std::thread> threads;
    for (int w = 0; w < 4; ++w) {
        threads.emplace_back([&] {
            for (int i = 0; i < perThread; ++i) {
                uint64_t token = 0;
                while (rig.engine.TxnBegin(1000, &token) != Ok) {}
                uint8_t img[5];
                rig.engine.ReadMsg(kFake, 0, img, nullptr);
                uint32_t count = img[1] | (img[2] << 8) | (img[3] << 16);
                count++;
                img[1] = uint8_t(count); img[2] = uint8_t(count >> 8); img[3] = uint8_t(count >> 16);
                int32_t idx = kFake;
                rig.engine.Commit(token, &idx, 1, img, nullptr);
                rig.engine.TxnEnd(token);
            }
        });
    }
    for (auto& t : threads) t.join();
    uint8_t img[5];
    rig.engine.ReadMsg(kFake, 0, img, nullptr);
    CHECK_EQ(img[1] | (img[2] << 8) | (img[3] << 16), perThread * 4);

    uint64_t a = 0, b = 0;
    CHECK_EQ(rig.engine.TxnBegin(100, &a), Ok);
    CHECK_EQ(rig.engine.TxnBegin(20, &b), ErrBusy);
    CHECK_EQ(rig.engine.TxnEnd(a), Ok);
}

static void TestTiming() {
    puts("periodic slots stay on the grid (3 s at 5 ms)");
    ChannelConfig cfg = FastConfig(5000);
    Rig rig(cfg);
    std::mutex m;
    std::vector<int64_t> times;
    times.reserve(2000);
    rig.sim[C]->SetFrameHook([&](const MsgDesc&, const uint8_t*, int64_t t) {
        std::lock_guard<std::mutex> g(m);
        times.push_back(t);
    });
    CHECK_EQ(rig.engine.SetCyclic(C, true), Ok);
    Sleep(3000);
    rig.engine.SetCyclic(C, false);
    Sleep(50);
    ChannelStats s;
    rig.engine.GetStats(C, &s);
    std::lock_guard<std::mutex> g(m);
    CHECK(times.size() > 500);
    std::vector<int64_t> gaps;
    for (size_t i = 1; i < times.size(); ++i) gaps.push_back(times[i] - times[i - 1]);
    std::sort(gaps.begin(), gaps.end());
    const double meanGap = double(times.back() - times.front()) / double(times.size() - 1);
    printf("  frames %zu  mean gap %.1f us  min %lld  p99 %lld  max %lld  window misses %llu  missed slots %llu  max lateness %llu us\n",
           times.size(), meanGap, (long long)gaps.front(), (long long)gaps[gaps.size() * 99 / 100], (long long)gaps.back(),
           (unsigned long long)s.windowMisses, (unsigned long long)s.missedSlots, (unsigned long long)s.maxLatenessUs);
    CHECK(std::abs(meanGap - 5000.0) < 50.0); // no accumulated drift
    CHECK_EQ(s.rxTimeouts, 0ull);
}

static void TestLongOp() {
    puts("long operation pauses periodic traffic, then it resumes");
    Rig rig;
    std::atomic<uint64_t> telemetry{0}, eeprom{0};
    rig.sim[D]->SetFrameHook([&](const MsgDesc& d, const uint8_t*, int64_t) {
        if (d.id == 0x91) telemetry++;
        if (d.id == 0xA0) eeprom++;
    });
    CHECK_EQ(rig.engine.SetCyclic(D, true), Ok);
    Sleep(100);
    CHECK(telemetry.load() > 10);

    CHECK_EQ(rig.engine.LongOpBegin(D, 5000, 500), Ok);
    const uint64_t before = telemetry.load();
    uint64_t seq = 0, token = 0;
    CHECK_EQ(rig.engine.TxnBegin(100, &token), Ok);
    uint8_t image[D_Telemetry_To::kLen] = {0};
    rig.engine.ReadMsg(kTelemetry, 0, image, nullptr);
    int32_t id = kTelemetry;
    CHECK_EQ(rig.engine.Commit(token, &id, 1, image, &seq), Ok);
    CHECK_EQ(rig.engine.TxnEnd(token), Ok);
    CHECK_EQ(rig.engine.WaitApplied(seq, 100), ErrLongOp);
    for (int i = 0; i < 5; ++i) {
        uint8_t rx[D_EepromWrite_From::kLen];
        CHECK_EQ(rig.engine.Exchange(kEeprom, 200, rx), Ok);
        Sleep(30);
    }
    CHECK_EQ(telemetry.load(), before);   // heartbeat fully stopped
    CHECK_EQ(eeprom.load(), 5ull);
    CHECK_EQ(rig.engine.LongOpBegin(D, 5000, 50), ErrBusy);
    CHECK_EQ(rig.engine.LongOpEnd(D), Ok);
    Sleep(100);
    CHECK(telemetry.load() > before + 10); // heartbeat restarted

    puts("  watchdog restarts traffic if the owner never ends the op");
    CHECK_EQ(rig.engine.LongOpBegin(D, 100, 500), Ok);
    const uint64_t paused = telemetry.load();
    Sleep(50);
    CHECK_EQ(telemetry.load(), paused);
    Sleep(250);
    CHECK(!rig.engine.IsLongOpActive(D));
    CHECK(telemetry.load() > paused + 10);
    ChannelStats s;
    rig.engine.GetStats(D, &s);
    CHECK_EQ(s.longOpWatchdogs, 1ull);
}

static void TestOneShotOnGrid() {
    puts("one-shot messages use a slot and never disturb the grid");
    ChannelConfig cfg = FastConfig(5000);
    Rig rig(cfg);
    std::mutex m;
    std::vector<int64_t> times;
    std::atomic<uint64_t> eeprom{0};
    rig.sim[D]->SetFrameHook([&](const MsgDesc& d, const uint8_t*, int64_t t) {
        std::lock_guard<std::mutex> g(m);
        times.push_back(t);
        if (d.id == 0xA0) eeprom++;
    });
    CHECK_EQ(rig.engine.SetCyclic(D, true), Ok);
    for (int i = 0; i < 40; ++i) {
        CHECK_EQ(rig.engine.Exchange(kEeprom, 100, nullptr), Ok);
        Sleep(7);
    }
    rig.engine.SetCyclic(D, false);
    Sleep(30);
    std::lock_guard<std::mutex> g(m);
    CHECK_EQ(eeprom.load(), 40ull);
    int64_t minGap = INT64_MAX;
    for (size_t i = 1; i < times.size(); ++i) minGap = (std::min)(minGap, times[i] - times[i - 1]);
    printf("  smallest gap between any two frames: %lld us\n", (long long)minGap);
    CHECK(minGap > 3500);
}

static void TestFaults() {
    puts("corrupted and missing responses are detected and recovered from");
    Rig rig;
    rig.sim[C]->SetFaults(7, 5, 200);
    CHECK_EQ(rig.engine.SetCyclic(C, true), Ok);
    Sleep(500);
    rig.engine.SetCyclic(C, false);
    Sleep(20);
    ChannelStats s;
    rig.engine.GetStats(C, &s);
    printf("  tx %llu  rx %llu  crc errors %llu  timeouts %llu\n", (unsigned long long)s.txFrames,
           (unsigned long long)s.rxFrames, (unsigned long long)s.crcErrors, (unsigned long long)s.rxTimeouts);
    CHECK(s.crcErrors > 0);
    CHECK(s.rxTimeouts > 0);
    CHECK(s.rxFrames > 50);
    CHECK(s.rxFrames < s.txFrames);
}

static void TestVariants() {
    puts("half-duplex echo and FF trailer");
    ChannelConfig cfg = FastConfig();
    cfg.rs485Echo = 1;
    cfg.trailerFf = 1;
    Rig rig(cfg);
    C_FakeIcd_To t;
    t.Test3 = 21;
    rig.CommitFake(t);
    for (int i = 0; i < 50; ++i) {
        uint8_t rx[5];
        CHECK_EQ(rig.engine.Exchange(kFake, 200, rx), Ok);
        CHECK_EQ(rx[4], 21);
    }
    ChannelStats s;
    rig.engine.GetStats(C, &s);
    CHECK_EQ(s.echoErrors, 0ull);
    CHECK_EQ(s.strayBytes, 0ull);

    puts("From message shorter than To");
    CHECK_EQ(int(D_Telemetry_From::kLen), 8);
    CHECK_EQ(int(D_Telemetry_To::kLen), 9);
    uint8_t rx[D_Telemetry_From::kLen];
    CHECK_EQ(rig.engine.Exchange(kTelemetry, 200, rx), Ok);
}

static void TestSchedule() {
    puts("schedules that cannot fit are rejected");
    LinkEngine e(GetSchema());
    ChannelConfig cfg = FastConfig(500);
    cfg.baud = 9600;
    CHECK_EQ(e.Attach(C, std::make_unique<SimDevice>(GetSchema(), 'C', cfg), cfg), ErrSchedule);
    CHECK(!e.IsOpen(C));
    CHECK_EQ(e.SetCyclic(C, true), ErrNotOpen);
}

int main() {
    TestCrc();
    TestPack();
    TestWireFormat();
    TestNoTearing();
    TestCrossChannelCommit();
    TestWaitApplied();
    TestGate();
    TestTiming();
    TestLongOp();
    TestOneShotOnGrid();
    TestFaults();
    TestVariants();
    TestSchedule();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}
