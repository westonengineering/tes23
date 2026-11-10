// SimDevice.h - software model of the target, used in place of hardware.
// It answers each valid frame the way the real device is expected to: the same
// bytes back, with To-only fields cleared and the frame cut to the From length.
#pragma once
#include "IByteTransport.h"
#include "IcdSchema.h"
#include "Crc32.h"
#include "LinkEngine.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace icd {

class SimDevice : public IByteTransport {
public:
    SimDevice(const Schema& schema, char channel, const ChannelConfig& cfg);

    bool IsOpen() const override { return m_open; }
    void Close() override { m_open = false; }
    bool Write(const uint8_t* data, uint32_t n) override;
    int32_t Read(uint8_t* data, uint32_t maxBytes) override;
    bool WaitRx(uint32_t timeoutUs) override;
    std::string LastError() const override { return "simulated device closed"; }

    // Faults: every Nth frame is ignored or returned corrupted (0 disables).
    void SetFaults(uint32_t dropEvery, uint32_t corruptEvery, uint32_t responseDelayUs);
    // Called on the channel thread with every valid frame the device receives.
    using FrameHook = std::function<void(const MsgDesc&, const uint8_t* image, int64_t timeUs)>;
    void SetFrameHook(FrameHook hook);
    uint64_t FramesSeen() const { return m_frames.load(); }
    uint64_t BadFrames() const { return m_bad.load(); }

private:
    struct Chunk { int64_t readyUs; std::vector<uint8_t> bytes; size_t pos; };
    void Queue(const uint8_t* data, size_t n, int64_t readyUs);

    const Schema& m_schema;
    char m_channel;
    ChannelConfig m_cfg;
    Crc32 m_crc;
    bool m_open = true;
    mutable std::mutex m_mutex;
    std::deque<Chunk> m_rx;
    FrameHook m_hook;
    std::atomic<uint32_t> m_dropEvery{0}, m_corruptEvery{0}, m_delayUs{200};
    std::atomic<uint64_t> m_frames{0}, m_bad{0};
};

} // namespace icd
