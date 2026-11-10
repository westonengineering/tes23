// LinkEngine.h - deterministic per-channel send/receive engine.
//
// Rules that make this free of races and torn updates:
//  * One thread per channel owns that channel's transport. Nothing else does I/O.
//  * Every slot copies the whole TX image under the image lock, so a slot sees
//    a commit entirely or not at all. A commit may span messages and channels.
//  * Writers must hold the transaction gate. Only one holder exists at a time,
//    so a set / check / change sequence cannot interleave with another writer.
//  * Each commit gets a sequence number. Each frame sent is tagged with the
//    newest commit it contains and the response inherits that tag, which is how
//    WaitApplied proves a reading came from a frame carrying the commit.
#pragma once
#include "IByteTransport.h"
#include "IcdSchema.h"
#include "Crc32.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace icd {

enum Status : int32_t {
    Ok = 0,
    ErrArg = -1,
    ErrNotOpen = -2,
    ErrTimeout = -3,
    ErrBusy = -4,
    ErrIo = -5,
    ErrNotOwner = -6,
    ErrLongOp = -7,
    ErrSchedule = -8,
    ErrClosed = -9,
    ErrNotRunning = -10,
};

int64_t NowUs();

// Layout is mirrored in IcdLinkApi.h and in the C# runtime.
struct ChannelConfig {
    uint32_t baud;
    uint32_t periodUs;      // 0 means no periodic traffic is possible
    uint32_t windowUs;      // a send later than this after its slot counts as a miss
    uint32_t rxTimeoutUs;
    uint32_t spinUs;        // busy-wait this long before a slot for accuracy
    uint32_t trailerFf;     // non-zero appends 0xFF on transmit
    uint32_t rs485Echo;     // non-zero discards our own bytes heard back
    uint32_t reserved;
};

struct ChannelStats {
    uint64_t slots, txFrames, rxFrames, crcErrors, rxTimeouts;
    uint64_t windowMisses, missedSlots, strayBytes, ioErrors, echoErrors;
    uint64_t oneShots, longOps, longOpWatchdogs;
    uint64_t lastLatenessUs, maxLatenessUs, lastExchangeUs;
};

struct MsgMeta {
    uint64_t rxSeq;      // responses accepted for this message
    uint64_t rxTag;      // commit contained in the frame this response answered
    uint64_t lastCommit; // newest commit that changed this message
    uint64_t sentTag;    // commit contained in the last frame transmitted
    int64_t  rxAgeUs;    // -1 until the first response
    uint32_t rxValid;
    uint32_t reserved;
};

class LinkEngine {
public:
    explicit LinkEngine(const Schema& schema);
    ~LinkEngine();
    LinkEngine(const LinkEngine&) = delete;
    LinkEngine& operator=(const LinkEngine&) = delete;

    const Schema& GetSchema() const { return m_schema; }

    // Channel lifetime. ch is 0 for C, 1 for D. Attach starts the channel thread.
    Status Attach(int ch, std::unique_ptr<IByteTransport> transport, const ChannelConfig& cfg);
    Status Detach(int ch);
    void   DetachAll();
    bool   IsOpen(int ch) const;

    // Periodic traffic
    Status SetCyclic(int ch, bool enable);
    Status SetCyclicMessage(int msg, bool enable);
    Status SetPeriod(int ch, uint32_t periodUs);

    // Transactions. Commit() needs the token from TxnBegin().
    Status TxnBegin(uint32_t timeoutMs, uint64_t* token);
    Status TxnEnd(uint64_t token);
    // flat holds the images for msgs[0..n) back to back, txLen bytes each.
    Status Commit(uint64_t token, const int32_t* msgs, int32_t n, const uint8_t* flat, uint64_t* seq);
    // Waits until every periodic message changed by commits up to seq has been
    // transmitted and, when it has a response, answered with a good frame.
    Status WaitApplied(uint64_t seq, uint32_t timeoutMs);

    // Reads every image atomically. Buffers are txTotal, rxTotal and messageCount entries.
    Status Snapshot(uint8_t* tx, uint8_t* rx, MsgMeta* meta, uint64_t* commitSeq);
    // side is 0 for the TX image, 1 for the RX image.
    Status ReadMsg(int msg, int side, uint8_t* out, MsgMeta* meta);

    // Sends the committed image of msg once and waits for its response. With
    // periodic traffic running it takes the next slot so the grid is never
    // disturbed. rxOut may be null, otherwise it must hold rxLen bytes.
    Status Exchange(int msg, uint32_t timeoutMs, uint8_t* rxOut);

    // Pauses periodic traffic on a channel for slow device operations. While a
    // long operation is active, Exchange() runs immediately. If the owner never
    // ends it, the watchdog restarts periodic traffic after watchdogMs.
    Status LongOpBegin(int ch, uint32_t watchdogMs, uint32_t timeoutMs);
    Status LongOpEnd(int ch);
    bool   IsLongOpActive(int ch) const;

    Status GetStats(int ch, ChannelStats* out) const;
    Status ResetStats(int ch);
    std::string LastError() const;
    void SetError(const std::string& text);

private:
    struct AtomicStats {
        std::atomic<uint64_t> slots{0}, txFrames{0}, rxFrames{0}, crcErrors{0}, rxTimeouts{0};
        std::atomic<uint64_t> windowMisses{0}, missedSlots{0}, strayBytes{0}, ioErrors{0}, echoErrors{0};
        std::atomic<uint64_t> oneShots{0}, longOps{0}, longOpWatchdogs{0};
        std::atomic<uint64_t> lastLatenessUs{0}, maxLatenessUs{0}, lastExchangeUs{0};
    };

    struct MsgState {
        uint32_t txOffset = 0, rxOffset = 0;
        uint64_t lastCommit = 0, sentTag = 0, rxTag = 0, rxSeq = 0;
        int64_t  rxTimeUs = 0;
        bool     rxValid = false;
        std::atomic<bool> cyclic{false};
    };

    struct Request {
        int32_t  msg = -1;
        uint32_t timeoutUs = 0;
        Status   result = Ok;
        std::vector<uint8_t> rx;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> done{false};
    };

    struct Channel {
        int  index = 0;
        std::unique_ptr<IByteTransport> transport;
        ChannelConfig cfg{};
        std::thread thread;
        HANDLE wake = nullptr;
        HANDLE timer = nullptr;
        std::atomic<bool> running{false};
        std::atomic<bool> stop{false};
        std::atomic<bool> cyclic{false};
        std::atomic<uint32_t> periodUs{0};
        std::atomic<bool> longOp{false};
        std::atomic<bool> longOpAck{false};
        std::atomic<int64_t> longOpDeadlineUs{0};
        std::atomic<int64_t> longOpWatchdogUs{0};
        std::vector<int> messages;
        size_t rotate = 0;
        std::deque<std::shared_ptr<Request>> queue;
        std::vector<uint8_t> frame, rxBuffer, drainBuffer;
        AtomicStats stats;
    };

    class Guard;

    void   Run(Channel& c);
    bool   WaitUntil(Channel& c, int64_t targetUs);
    Status RunExchange(Channel& c, int msg, int64_t rxBudgetUs, Request* req, int64_t slotTargetUs);
    void   Drain(Channel& c);
    int    NextCyclic(Channel& c);
    bool   HasEnabledCyclic(const Channel& c) const;
    std::shared_ptr<Request> PopRequest(Channel& c);
    void   FinishRequest(const std::shared_ptr<Request>& req, Status status);
    void   Bump();
    Status CheckFeasible(int ch, const ChannelConfig& cfg);
    template <class Pred> Status WaitFor(uint32_t timeoutMs, Pred pred);
    uint32_t FrameLen(uint32_t imageLen, const ChannelConfig& cfg) const;

    const Schema& m_schema;
    Crc32 m_crc;
    mutable SRWLOCK m_lock;
    std::vector<uint8_t> m_tx, m_rx;
    std::unique_ptr<MsgState[]> m_msgs;
    uint64_t m_commitSeq = 0;
    Channel  m_channels[2];
    std::atomic<uint64_t> m_gate{0};
    std::mutex m_gateMutex;
    std::deque<uint64_t> m_gateQueue;
    std::atomic<uint64_t> m_tokenSource{0};
    std::atomic<uint64_t> m_progress{0};
    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};

} // namespace icd
