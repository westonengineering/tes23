#include "LinkEngine.h"
#include <mmsystem.h>
#include <algorithm>
#include <cstring>
#include <limits>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Synchronization.lib")

namespace icd {

int64_t NowUs() {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq) * 1000000 + (c.QuadPart % freq) * 1000000 / freq;
}

// Non-real-time callers run at time-critical priority while holding the image
// lock, so a preempted GUI thread can never make a channel thread miss a slot.
class LinkEngine::Guard {
public:
    explicit Guard(SRWLOCK& lock) : m_lock(lock) {
        m_thread = GetCurrentThread();
        m_priority = GetThreadPriority(m_thread);
        SetThreadPriority(m_thread, THREAD_PRIORITY_TIME_CRITICAL);
        AcquireSRWLockExclusive(&m_lock);
    }
    ~Guard() {
        ReleaseSRWLockExclusive(&m_lock);
        SetThreadPriority(m_thread, m_priority);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    SRWLOCK& m_lock;
    HANDLE   m_thread;
    int      m_priority;
};

LinkEngine::LinkEngine(const Schema& schema) : m_schema(schema) {
    InitializeSRWLock(&m_lock);
    m_crc.Init(schema.crc.poly, schema.crc.init, schema.crc.xorOut, schema.crc.reflectIn, schema.crc.reflectOut);
    m_tx.assign(schema.txTotal, 0);
    m_rx.assign(schema.rxTotal, 0);
    m_msgs.reset(new MsgState[schema.messageCount]);

    uint32_t txOffset = 0, rxOffset = 0, maxImage = 16;
    for (int i = 0; i < schema.messageCount; ++i) {
        const MsgDesc& m = schema.messages[i];
        m_msgs[i].txOffset = txOffset;
        m_msgs[i].rxOffset = rxOffset;
        m_msgs[i].cyclic = m.cyclic;
        m_tx[txOffset] = m.id;
        m_rx[rxOffset] = m.rxId;
        for (int f = 0; f < m.fieldCount; ++f) {
            const FieldDesc& d = m.fields[f];
            if (d.dir & DirTo) SetValue(&m_tx[txOffset], d, d.defaultValue);
        }
        txOffset += m.txLen;
        rxOffset += m.rxLen;
        maxImage = (std::max)(maxImage, static_cast<uint32_t>((std::max)(m.txLen, m.rxLen)));
        const int ch = ChannelIndex(m.channel);
        if (ch >= 0) m_channels[ch].messages.push_back(i);
    }
    for (int ch = 0; ch < 2; ++ch) {
        Channel& c = m_channels[ch];
        c.index = ch;
        c.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        c.frame.resize(maxImage + 8);
        c.rxBuffer.resize((maxImage + 8) * 4);
        c.drainBuffer.resize(4096);
    }
    // Windows 11 otherwise ignores the timer request below for processes with no
    // visible window, which stretches short waits to about 15 ms and costs slots.
    PROCESS_POWER_THROTTLING_STATE throttle = {};
    throttle.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttle.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttle.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttle, sizeof(throttle));
    timeBeginPeriod(1);
}

LinkEngine::~LinkEngine() {
    DetachAll();
    for (Channel& c : m_channels)
        if (c.wake) CloseHandle(c.wake);
    timeEndPeriod(1);
}

std::string LinkEngine::LastError() const {
    std::lock_guard<std::mutex> g(m_errorMutex);
    return m_lastError;
}

void LinkEngine::SetError(const std::string& text) {
    std::lock_guard<std::mutex> g(m_errorMutex);
    m_lastError = text;
}

void LinkEngine::Bump() {
    m_progress.fetch_add(1, std::memory_order_acq_rel);
    WakeByAddressAll(&m_progress);
}

template <class Pred>
Status LinkEngine::WaitFor(uint32_t timeoutMs, Pred pred) {
    const int64_t deadline = NowUs() + static_cast<int64_t>(timeoutMs) * 1000;
    for (;;) {
        uint64_t seen = m_progress.load(std::memory_order_acquire);
        Status result = Ok;
        if (pred(result)) return result;
        const int64_t left = deadline - NowUs();
        if (left <= 0) return ErrTimeout;
        WaitOnAddress(&m_progress, &seen, sizeof(seen), static_cast<DWORD>((left + 999) / 1000));
    }
}

uint32_t LinkEngine::FrameLen(uint32_t imageLen, const ChannelConfig& cfg) const {
    return imageLen + 4 + (cfg.trailerFf ? 1u : 0u);
}

Status LinkEngine::CheckFeasible(int ch, const ChannelConfig& cfg) {
    if (cfg.baud == 0) { SetError("baud must be non-zero"); return ErrArg; }
    if (cfg.periodUs == 0) return Ok;
    for (int i : m_channels[ch].messages) {
        const MsgDesc& m = m_schema.messages[i];
        if (!m.cyclic) continue;
        uint64_t bytes = FrameLen(m.txLen, cfg);
        if (m.hasResponse) bytes += FrameLen(m.rxLen, cfg);
        // start + 8 data + parity + stop = 11 bits per byte, plus USB latency.
        const uint64_t wireUs = bytes * 11ull * 1000000ull / cfg.baud + 1000ull;
        if (wireUs >= cfg.periodUs) {
            SetError(std::string("message ") + m.name + " needs " + std::to_string(wireUs) +
                     " us on the wire, which does not fit a period of " + std::to_string(cfg.periodUs) + " us");
            return ErrSchedule;
        }
        // Only a timeout that can never be met is rejected.
        const uint64_t lineUs = wireUs - 1000ull;
        if (m.hasResponse && cfg.rxTimeoutUs != 0 && lineUs >= cfg.rxTimeoutUs) {
            SetError(std::string("message ") + m.name + " needs " + std::to_string(lineUs) +
                     " us for send and response, more than the rx timeout of " + std::to_string(cfg.rxTimeoutUs) + " us");
            return ErrSchedule;
        }
    }
    return Ok;
}

Status LinkEngine::Attach(int ch, std::unique_ptr<IByteTransport> transport, const ChannelConfig& cfg) {
    if (ch < 0 || ch > 1 || !transport) return ErrArg;
    Detach(ch);
    const Status feasible = CheckFeasible(ch, cfg);
    if (feasible != Ok) return feasible;

    Channel& c = m_channels[ch];
    c.transport = std::move(transport);
    c.cfg = cfg;
    c.periodUs = cfg.periodUs;
    c.stop = false;
    c.longOp = false;
    c.longOpAck = false;
    c.cyclic = false;
    c.rotate = 0;
    ResetEvent(c.wake);
    c.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!c.timer) c.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    c.running = true;
    c.thread = std::thread([this, &c] { Run(c); });
    return Ok;
}

Status LinkEngine::Detach(int ch) {
    if (ch < 0 || ch > 1) return ErrArg;
    Channel& c = m_channels[ch];
    if (!c.running.load()) return Ok;
    c.stop = true;
    SetEvent(c.wake);
    if (c.thread.joinable()) c.thread.join();
    c.running = false;
    c.cyclic = false;
    c.longOp = false;
    c.transport.reset();
    if (c.timer) { CloseHandle(c.timer); c.timer = nullptr; }
    Bump();
    return Ok;
}

void LinkEngine::DetachAll() {
    Detach(0);
    Detach(1);
}

bool LinkEngine::IsOpen(int ch) const {
    return ch >= 0 && ch < 2 && m_channels[ch].running.load();
}

Status LinkEngine::SetCyclic(int ch, bool enable) {
    if (ch < 0 || ch > 1) return ErrArg;
    Channel& c = m_channels[ch];
    if (!c.running.load()) return ErrNotOpen;
    if (enable && c.periodUs.load() == 0) { SetError("the period is 0, set a period first"); return ErrArg; }
    c.cyclic = enable;
    SetEvent(c.wake);
    return Ok;
}

Status LinkEngine::SetCyclicMessage(int msg, bool enable) {
    if (msg < 0 || msg >= m_schema.messageCount) return ErrArg;
    m_msgs[msg].cyclic = enable;
    SetEvent(m_channels[ChannelIndex(m_schema.messages[msg].channel)].wake);
    return Ok;
}

Status LinkEngine::SetPeriod(int ch, uint32_t periodUs) {
    if (ch < 0 || ch > 1) return ErrArg;
    Channel& c = m_channels[ch];
    if (c.running.load()) {
        ChannelConfig check = c.cfg;
        check.periodUs = periodUs;
        const Status s = CheckFeasible(ch, check);
        if (s != Ok) return s;
    }
    c.periodUs = periodUs;
    SetEvent(c.wake);
    return Ok;
}

Status LinkEngine::TxnBegin(uint32_t timeoutMs, uint64_t* token) {
    if (!token) return ErrArg;
    const uint64_t mine = m_tokenSource.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> g(m_gateMutex);
        m_gateQueue.push_back(mine);
    }
    // First come, first served, so a busy writer cannot starve the others.
    const Status s = WaitFor(timeoutMs, [&](Status&) {
        std::lock_guard<std::mutex> g(m_gateMutex);
        if (m_gateQueue.front() != mine) return false;
        uint64_t expected = 0;
        if (!m_gate.compare_exchange_strong(expected, mine, std::memory_order_acq_rel)) return false;
        m_gateQueue.pop_front();
        return true;
    });
    if (s == Ok) { *token = mine; return Ok; }
    {
        std::lock_guard<std::mutex> g(m_gateMutex);
        m_gateQueue.erase(std::remove(m_gateQueue.begin(), m_gateQueue.end(), mine), m_gateQueue.end());
    }
    Bump(); // the next in line may now be at the front
    return s == ErrTimeout ? ErrBusy : s;
}

Status LinkEngine::TxnEnd(uint64_t token) {
    uint64_t expected = token;
    if (token == 0 || !m_gate.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
        return ErrNotOwner;
    Bump();
    return Ok;
}

Status LinkEngine::Commit(uint64_t token, const int32_t* msgs, int32_t n, const uint8_t* flat, uint64_t* seq) {
    if (n < 0 || (n > 0 && (!msgs || !flat))) return ErrArg;
    if (token == 0 || m_gate.load(std::memory_order_acquire) != token) return ErrNotOwner;
    for (int32_t i = 0; i < n; ++i)
        if (msgs[i] < 0 || msgs[i] >= m_schema.messageCount) return ErrArg;
    {
        Guard g(m_lock);
        if (n > 0) ++m_commitSeq;
        const uint8_t* p = flat;
        for (int32_t i = 0; i < n; ++i) {
            const MsgDesc& d = m_schema.messages[msgs[i]];
            MsgState& s = m_msgs[msgs[i]];
            std::memcpy(&m_tx[s.txOffset], p, d.txLen);
            m_tx[s.txOffset] = d.id;     // the type byte is never user data
            s.lastCommit = m_commitSeq;
            p += d.txLen;
        }
        if (seq) *seq = m_commitSeq;
    }
    Bump();
    return Ok;
}

Status LinkEngine::WaitApplied(uint64_t seq, uint32_t timeoutMs) {
    return WaitFor(timeoutMs, [&](Status& result) {
        Guard g(m_lock);
        for (int i = 0; i < m_schema.messageCount; ++i) {
            const MsgDesc& d = m_schema.messages[i];
            const MsgState& s = m_msgs[i];
            const uint64_t need = (std::min)(s.lastCommit, seq);
            if (need == 0) continue;
            if ((d.hasResponse ? s.rxTag : s.sentTag) >= need) continue;
            if (!s.cyclic.load(std::memory_order_relaxed)) continue; // sent only by Exchange
            const Channel& c = m_channels[ChannelIndex(d.channel)];
            if (!c.running.load()) { result = ErrNotOpen; return true; }
            if (c.longOp.load()) { result = ErrLongOp; return true; }
            if (!c.cyclic.load()) { result = ErrNotRunning; return true; }
            return false;
        }
        return true;
    });
}

Status LinkEngine::Snapshot(uint8_t* tx, uint8_t* rx, MsgMeta* meta, uint64_t* commitSeq) {
    const int64_t now = NowUs();
    Guard g(m_lock);
    if (tx) std::memcpy(tx, m_tx.data(), m_tx.size());
    if (rx) std::memcpy(rx, m_rx.data(), m_rx.size());
    if (meta) {
        for (int i = 0; i < m_schema.messageCount; ++i) {
            const MsgState& s = m_msgs[i];
            meta[i].rxSeq = s.rxSeq; meta[i].rxTag = s.rxTag;
            meta[i].lastCommit = s.lastCommit; meta[i].sentTag = s.sentTag;
            meta[i].rxAgeUs = s.rxValid ? now - s.rxTimeUs : -1;
            meta[i].rxValid = s.rxValid ? 1 : 0;
            meta[i].reserved = 0;
        }
    }
    if (commitSeq) *commitSeq = m_commitSeq;
    return Ok;
}

Status LinkEngine::ReadMsg(int msg, int side, uint8_t* out, MsgMeta* meta) {
    if (msg < 0 || msg >= m_schema.messageCount || (side != 0 && side != 1)) return ErrArg;
    const MsgDesc& d = m_schema.messages[msg];
    const int64_t now = NowUs();
    Guard g(m_lock);
    const MsgState& s = m_msgs[msg];
    if (out) {
        if (side == 0) std::memcpy(out, &m_tx[s.txOffset], d.txLen);
        else           std::memcpy(out, &m_rx[s.rxOffset], d.rxLen);
    }
    if (meta) {
        meta->rxSeq = s.rxSeq; meta->rxTag = s.rxTag;
        meta->lastCommit = s.lastCommit; meta->sentTag = s.sentTag;
        meta->rxAgeUs = s.rxValid ? now - s.rxTimeUs : -1;
        meta->rxValid = s.rxValid ? 1 : 0;
        meta->reserved = 0;
    }
    return Ok;
}

Status LinkEngine::Exchange(int msg, uint32_t timeoutMs, uint8_t* rxOut) {
    if (msg < 0 || msg >= m_schema.messageCount) return ErrArg;
    const MsgDesc& d = m_schema.messages[msg];
    Channel& c = m_channels[ChannelIndex(d.channel)];
    if (!c.running.load()) return ErrNotOpen;

    auto req = std::make_shared<Request>();
    req->msg = msg;
    req->timeoutUs = timeoutMs * 1000u;
    {
        Guard g(m_lock);
        c.queue.push_back(req);
    }
    // While the periodic grid runs, the request simply takes the next slot.
    if (!c.cyclic.load() || c.longOp.load()) SetEvent(c.wake);

    Status status = WaitFor(timeoutMs + 50, [&](Status& result) {
        if (!req->done.load(std::memory_order_acquire)) return false;
        result = req->result;
        return true;
    });
    if (status == ErrTimeout && !req->done.load(std::memory_order_acquire)) {
        // A send that the caller has given up on must not go out later.
        req->cancelled = true;
        return ErrTimeout;
    }
    if (status == Ok && rxOut && d.hasResponse && req->rx.size() == d.rxLen)
        std::memcpy(rxOut, req->rx.data(), d.rxLen);
    return status;
}

Status LinkEngine::LongOpBegin(int ch, uint32_t watchdogMs, uint32_t timeoutMs) {
    if (ch < 0 || ch > 1) return ErrArg;
    Channel& c = m_channels[ch];
    if (!c.running.load()) return ErrNotOpen;
    bool expected = false;
    if (!c.longOp.compare_exchange_strong(expected, true)) return ErrBusy;
    c.longOpWatchdogUs = static_cast<int64_t>(watchdogMs) * 1000;
    c.longOpDeadlineUs = NowUs() + c.longOpWatchdogUs.load();
    c.longOpAck = false;
    SetEvent(c.wake);
    const Status s = WaitFor(timeoutMs, [&](Status&) { return c.longOpAck.load(); });
    if (s != Ok) { c.longOp = false; SetEvent(c.wake); return s; }
    return Ok;
}

Status LinkEngine::LongOpEnd(int ch) {
    if (ch < 0 || ch > 1) return ErrArg;
    Channel& c = m_channels[ch];
    c.longOp = false;
    c.longOpAck = false;
    SetEvent(c.wake);
    Bump();
    return Ok;
}

bool LinkEngine::IsLongOpActive(int ch) const {
    return ch >= 0 && ch < 2 && m_channels[ch].longOp.load();
}

Status LinkEngine::GetStats(int ch, ChannelStats* o) const {
    if (ch < 0 || ch > 1 || !o) return ErrArg;
    const AtomicStats& s = m_channels[ch].stats;
    o->slots = s.slots; o->txFrames = s.txFrames; o->rxFrames = s.rxFrames;
    o->crcErrors = s.crcErrors; o->rxTimeouts = s.rxTimeouts;
    o->windowMisses = s.windowMisses; o->missedSlots = s.missedSlots;
    o->strayBytes = s.strayBytes; o->ioErrors = s.ioErrors; o->echoErrors = s.echoErrors;
    o->oneShots = s.oneShots; o->longOps = s.longOps; o->longOpWatchdogs = s.longOpWatchdogs;
    o->lastLatenessUs = s.lastLatenessUs; o->maxLatenessUs = s.maxLatenessUs; o->lastExchangeUs = s.lastExchangeUs;
    return Ok;
}

Status LinkEngine::ResetStats(int ch) {
    if (ch < 0 || ch > 1) return ErrArg;
    AtomicStats& s = m_channels[ch].stats;
    s.slots = 0; s.txFrames = 0; s.rxFrames = 0; s.crcErrors = 0; s.rxTimeouts = 0;
    s.windowMisses = 0; s.missedSlots = 0; s.strayBytes = 0; s.ioErrors = 0; s.echoErrors = 0;
    s.oneShots = 0; s.longOps = 0; s.longOpWatchdogs = 0;
    s.lastLatenessUs = 0; s.maxLatenessUs = 0; s.lastExchangeUs = 0;
    return Ok;
}

// ------------------------------------------------------------- channel thread

bool LinkEngine::WaitUntil(Channel& c, int64_t targetUs) {
    const int64_t spin = c.cfg.spinUs;
    for (;;) {
        const int64_t left = targetUs - NowUs();
        if (left <= 0) return true;
        if (left <= spin) {
            while (NowUs() < targetUs) YieldProcessor();
            return true;
        }
        LARGE_INTEGER due;
        due.QuadPart = -10LL * (left - spin);
        HANDLE handles[2] = { c.wake, c.timer };
        SetWaitableTimer(c.timer, &due, 0, nullptr, nullptr, FALSE);
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(left / 1000 + 5));
        if (w == WAIT_OBJECT_0) return false;
    }
}

std::shared_ptr<LinkEngine::Request> LinkEngine::PopRequest(Channel& c) {
    AcquireSRWLockExclusive(&m_lock);
    std::shared_ptr<Request> req;
    while (!c.queue.empty()) {
        req = c.queue.front();
        c.queue.pop_front();
        if (!req->cancelled.load()) break;
        req.reset();
    }
    ReleaseSRWLockExclusive(&m_lock);
    return req;
}

void LinkEngine::FinishRequest(const std::shared_ptr<Request>& req, Status status) {
    req->result = status;
    req->done.store(true, std::memory_order_release);
    Bump();
}

bool LinkEngine::HasEnabledCyclic(const Channel& c) const {
    for (int i : c.messages)
        if (m_msgs[i].cyclic.load(std::memory_order_relaxed)) return true;
    return false;
}

int LinkEngine::NextCyclic(Channel& c) {
    for (size_t n = 0; n < c.messages.size(); ++n) {
        const int m = c.messages[c.rotate % c.messages.size()];
        c.rotate = (c.rotate + 1) % c.messages.size();
        if (m_msgs[m].cyclic.load(std::memory_order_relaxed)) return m;
    }
    return -1;
}

void LinkEngine::Drain(Channel& c) {
    for (;;) {
        const int32_t n = c.transport->Read(c.drainBuffer.data(), static_cast<uint32_t>(c.drainBuffer.size()));
        if (n <= 0) { if (n < 0) c.stats.ioErrors++; return; }
        // A lone 0xFF is the optional end marker of the previous response.
        if (!(n == 1 && c.drainBuffer[0] == 0xFF)) c.stats.strayBytes += static_cast<uint64_t>(n);
    }
}

Status LinkEngine::RunExchange(Channel& c, int msgIndex, int64_t rxBudgetUs, Request* req, int64_t slotTargetUs) {
    const MsgDesc& d = m_schema.messages[msgIndex];
    MsgState& s = m_msgs[msgIndex];

    Drain(c);

    // The whole image is taken at once: a commit is in this frame entirely or not at all.
    uint64_t tag;
    AcquireSRWLockExclusive(&m_lock);
    std::memcpy(c.frame.data(), &m_tx[s.txOffset], d.txLen);
    tag = m_commitSeq;
    ReleaseSRWLockExclusive(&m_lock);

    const uint32_t crcFrom = m_schema.crc.coversType ? 0u : 1u;
    const uint32_t crc = m_crc.Compute(c.frame.data() + crcFrom, d.txLen - crcFrom);
    uint8_t* tail = c.frame.data() + d.txLen;
    for (int i = 0; i < 4; ++i)
        tail[m_schema.crc.bigEndianOnWire ? 3 - i : i] = static_cast<uint8_t>(crc >> (8 * i));
    uint32_t frameLen = d.txLen + 4u;
    if (c.cfg.trailerFf) c.frame[frameLen++] = 0xFF;

    const int64_t sendAt = NowUs();
    if (slotTargetUs != 0) {
        const uint64_t late = sendAt > slotTargetUs ? static_cast<uint64_t>(sendAt - slotTargetUs) : 0;
        c.stats.lastLatenessUs = late;
        if (late > c.stats.maxLatenessUs.load()) c.stats.maxLatenessUs = late;
        if (late > c.cfg.windowUs) c.stats.windowMisses++;
    }

    if (!c.transport->Write(c.frame.data(), frameLen)) {
        c.stats.ioErrors++;
        SetError(c.transport->LastError());
        return ErrIo;
    }
    c.stats.txFrames++;

    AcquireSRWLockExclusive(&m_lock);
    s.sentTag = tag;
    ReleaseSRWLockExclusive(&m_lock);

    if (!d.hasResponse) { Bump(); return Ok; }

    const uint32_t need = d.rxLen + 4u;
    uint32_t fill = 0, echoLeft = c.cfg.rs485Echo ? frameLen : 0, echoPos = 0;
    uint8_t* buf = c.rxBuffer.data();
    const uint32_t cap = static_cast<uint32_t>(c.rxBuffer.size());
    const int64_t deadline = sendAt + rxBudgetUs; // an echo overlaps the transmit, so it adds no time

    for (;;) {
        const int32_t got = c.transport->Read(buf + fill, cap - fill);
        if (got < 0) {
            c.stats.ioErrors++;
            SetError(c.transport->LastError());
            return ErrIo;
        }
        fill += static_cast<uint32_t>(got);

        // On half duplex the receiver may hear our own frame first.
        if (echoLeft != 0 && fill != 0) {
            const uint32_t n = (std::min)(echoLeft, fill);
            if (std::memcmp(buf, c.frame.data() + echoPos, n) != 0) c.stats.echoErrors++;
            std::memmove(buf, buf + n, fill - n);
            fill -= n; echoLeft -= n; echoPos += n;
        }

        if (echoLeft == 0) {
            while (fill != 0) {
                uint32_t start = 0;
                while (start < fill && buf[start] != d.rxId) ++start;
                if (start != 0) {
                    c.stats.strayBytes += start;
                    std::memmove(buf, buf + start, fill - start);
                    fill -= start;
                    if (fill == 0) break;
                }
                if (fill < need) break;

                const uint32_t rxCrc = m_crc.Compute(buf + crcFrom, d.rxLen - crcFrom);
                uint32_t wire = 0;
                for (int i = 0; i < 4; ++i)
                    wire |= static_cast<uint32_t>(buf[d.rxLen + (m_schema.crc.bigEndianOnWire ? 3 - i : i)]) << (8 * i);

                if (rxCrc != wire) {
                    // Not a frame boundary, or a corrupted frame: resync one byte on.
                    c.stats.crcErrors++;
                    std::memmove(buf, buf + 1, fill - 1);
                    fill -= 1;
                    continue;
                }

                const int64_t now = NowUs();
                AcquireSRWLockExclusive(&m_lock);
                std::memcpy(&m_rx[s.rxOffset], buf, d.rxLen);
                s.rxTag = tag;
                s.rxSeq++;
                s.rxTimeUs = now;
                s.rxValid = true;
                ReleaseSRWLockExclusive(&m_lock);

                if (req) req->rx.assign(buf, buf + d.rxLen);
                const uint32_t rest = fill - need;
                if (rest != 0 && !(rest == 1 && buf[need] == 0xFF)) c.stats.strayBytes += rest;
                if (c.cfg.trailerFf && rest == 0) {
                    // The end marker is still on the line. Take it now so the next
                    // send cannot start while the device is finishing its frame.
                    if (c.transport->WaitRx(static_cast<uint32_t>(33000000ull / c.cfg.baud) + 1u)) {
                        uint8_t tailByte = 0;
                        const int32_t tailRead = c.transport->Read(&tailByte, 1);
                        if (tailRead == 1 && tailByte != 0xFF) c.stats.strayBytes++;
                    }
                }
                c.stats.rxFrames++;
                c.stats.lastExchangeUs = static_cast<uint64_t>(now - sendAt);
                Bump();
                return Ok;
            }
        }

        const int64_t left = deadline - NowUs();
        if (left <= 0) {
            c.stats.rxTimeouts++;
            c.stats.strayBytes += fill;
            Bump();
            return ErrTimeout;
        }
        c.transport->WaitRx(static_cast<uint32_t>((std::min<int64_t>)(left, 2000)));
    }
}

void LinkEngine::Run(Channel& c) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int64_t next = NowUs() + c.periodUs.load();
    bool gridActive = false;

    while (!c.stop.load()) {
        if (c.longOp.load()) {
            gridActive = false;
            c.stats.longOps++;
            bool watchdogFired = false;
            while (c.longOp.load() && !c.stop.load()) {
                // Re-acknowledged each pass: a new op can start right after the last one ended.
                if (!c.longOpAck.exchange(true)) Bump();
                if (NowUs() >= c.longOpDeadlineUs.load()) { watchdogFired = true; break; }
                if (std::shared_ptr<Request> req = PopRequest(c)) {
                    const Status st = RunExchange(c, req->msg, static_cast<int64_t>(req->timeoutUs), req.get(), 0);
                    c.stats.oneShots++;
                    FinishRequest(req, st);
                    // The watchdog measures time with no activity from the owner.
                    c.longOpDeadlineUs = NowUs() + c.longOpWatchdogUs.load();
                    continue;
                }
                WaitForSingleObject(c.wake, 1);
            }
            if (watchdogFired) {
                c.stats.longOpWatchdogs++;
                c.longOp = false;
                c.longOpAck = false;
                Bump();
            }
            continue;
        }

        const uint32_t period = c.periodUs.load();
        const bool cyclic = c.cyclic.load() && period != 0 && HasEnabledCyclic(c);

        if (!cyclic) {
            gridActive = false;
            if (std::shared_ptr<Request> req = PopRequest(c)) {
                const Status st = RunExchange(c, req->msg, static_cast<int64_t>(req->timeoutUs), req.get(), 0);
                c.stats.oneShots++;
                FinishRequest(req, st);
                continue;
            }
            WaitForSingleObject(c.wake, 20);
            continue;
        }

        if (!gridActive) {
            // First slot after starting, or after a long op: anchor a new grid.
            gridActive = true;
            next = NowUs() + period;
        }

        if (!WaitUntil(c, next)) continue; // mode changed, re-evaluate
        if (c.longOp.load() || c.stop.load()) continue;

        std::shared_ptr<Request> req = PopRequest(c);
        const int msg = req ? req->msg : NextCyclic(c);
        if (msg < 0) continue;

        c.stats.slots++;
        const int64_t nextSlot = next + period;
        const int64_t guard = 300; // leave time to re-arm before the next slot
        int64_t budget = static_cast<int64_t>(c.cfg.rxTimeoutUs);
        const int64_t toNext = nextSlot - NowUs() - guard;
        if (budget == 0 || budget > toNext) budget = (std::max<int64_t>)(toNext, 0);

        const Status st = RunExchange(c, msg, budget, req.get(), next);
        if (req) { c.stats.oneShots++; FinishRequest(req, st); }

        next = nextSlot;
        const int64_t now = NowUs();
        if (now > next) {
            // Slots already gone are skipped, never sent in a burst.
            const int64_t skipped = (now - next) / period + 1;
            c.stats.missedSlots += static_cast<uint64_t>(skipped);
            next += skipped * period;
        }
    }

    std::deque<std::shared_ptr<Request>> pending;
    AcquireSRWLockExclusive(&m_lock);
    pending.swap(c.queue);
    ReleaseSRWLockExclusive(&m_lock);
    for (const auto& req : pending) FinishRequest(req, ErrClosed);
    c.longOpAck = false;
    Bump();
}

} // namespace icd
