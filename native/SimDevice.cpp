#include "SimDevice.h"
#include <algorithm>
#include <cstring>

namespace icd {

SimDevice::SimDevice(const Schema& schema, char channel, const ChannelConfig& cfg)
    : m_schema(schema), m_channel(channel), m_cfg(cfg) {
    m_crc.Init(schema.crc.poly, schema.crc.init, schema.crc.xorOut, schema.crc.reflectIn, schema.crc.reflectOut);
}

void SimDevice::SetFaults(uint32_t dropEvery, uint32_t corruptEvery, uint32_t delayUs) {
    m_dropEvery = dropEvery;
    m_corruptEvery = corruptEvery;
    m_delayUs = delayUs;
}

void SimDevice::SetFrameHook(FrameHook hook) {
    std::lock_guard<std::mutex> g(m_mutex);
    m_hook = std::move(hook);
}

void SimDevice::Queue(const uint8_t* data, size_t n, int64_t readyUs) {
    Chunk c;
    c.readyUs = readyUs;
    c.bytes.assign(data, data + n);
    c.pos = 0;
    m_rx.push_back(std::move(c));
}

bool SimDevice::Write(const uint8_t* data, uint32_t n) {
    if (!m_open) return false;
    const int64_t now = NowUs();
    std::lock_guard<std::mutex> g(m_mutex);
    // Bytes arrive one at a time at the real line rate: start + 8 data + parity + stop.
    const double byteUs = 11e6 / m_cfg.baud;
    const int64_t txEnd = now + static_cast<int64_t>(n * byteUs);
    if (m_cfg.rs485Echo)
        for (uint32_t i = 0; i < n; ++i) Queue(data + i, 1, now + static_cast<int64_t>((i + 1) * byteUs));

    const MsgDesc* d = nullptr;
    for (int i = 0; i < m_schema.messageCount; ++i) {
        const MsgDesc& m = m_schema.messages[i];
        if (m.channel == m_channel && m.id == data[0]) { d = &m; break; }
    }
    if (!d || n < d->txLen + 4u) { m_bad++; return true; }

    const uint32_t from = m_schema.crc.coversType ? 0u : 1u;
    const uint32_t crc = m_crc.Compute(data + from, d->txLen - from);
    uint32_t wire = 0;
    for (int i = 0; i < 4; ++i)
        wire |= static_cast<uint32_t>(data[d->txLen + (m_schema.crc.bigEndianOnWire ? 3 - i : i)]) << (8 * i);
    if (crc != wire) { m_bad++; return true; }

    const uint64_t index = ++m_frames;
    if (m_hook) m_hook(*d, data, now);
    if (!d->hasResponse) return true;

    const uint32_t dropEvery = m_dropEvery.load();
    if (dropEvery != 0 && index % dropEvery == 0) return true;

    std::vector<uint8_t> reply(d->rxLen + 4u, 0);
    bool separateLayouts = false;
    for (int f = 0; f < d->fieldCount; ++f)
        if (d->fields[f].dir == DirFrom) separateLayouts = true;

    if (separateLayouts) {
        // Each returned field takes the value of the sent field with the same name.
        for (int f = 0; f < d->fieldCount; ++f) {
            const FieldDesc& from = d->fields[f];
            if (!(from.dir & DirFrom)) continue;
            for (int t = 0; t < d->fieldCount; ++t) {
                const FieldDesc& to = d->fields[t];
                if ((to.dir & DirTo) && std::strcmp(to.name, from.name) == 0) {
                    SetRaw(reply.data(), from, GetRaw(data, to));
                    break;
                }
            }
        }
    } else {
        std::memcpy(reply.data(), data, (std::min)(d->rxLen, d->txLen));
        for (int f = 0; f < d->fieldCount; ++f)
            if (!(d->fields[f].dir & DirFrom)) {
                const FieldDesc& fd = d->fields[f];
                if (fd.byteOffset + fd.byteCount <= d->rxLen) SetRaw(reply.data(), fd, 0);
            }
    }
    reply[0] = d->rxId;
    const uint32_t rc = m_crc.Compute(reply.data() + from, d->rxLen - from);
    for (int i = 0; i < 4; ++i)
        reply[d->rxLen + (m_schema.crc.bigEndianOnWire ? 3 - i : i)] = static_cast<uint8_t>(rc >> (8 * i));
    if (m_cfg.trailerFf) reply.push_back(0xFF);

    const uint32_t corruptEvery = m_corruptEvery.load();
    if (corruptEvery != 0 && index % corruptEvery == 0) reply[reply.size() / 2] ^= 0x5A;

    const int64_t replyStart = txEnd + m_delayUs.load();
    for (size_t i = 0; i < reply.size(); ++i)
        Queue(reply.data() + i, 1, replyStart + static_cast<int64_t>((i + 1) * byteUs));
    return true;
}

int32_t SimDevice::Read(uint8_t* data, uint32_t maxBytes) {
    if (!m_open) return -1;
    const int64_t now = NowUs();
    std::lock_guard<std::mutex> g(m_mutex);
    uint32_t total = 0;
    while (total < maxBytes && !m_rx.empty() && m_rx.front().readyUs <= now) {
        Chunk& c = m_rx.front();
        const size_t n = (std::min)(static_cast<size_t>(maxBytes - total), c.bytes.size() - c.pos);
        std::memcpy(data + total, c.bytes.data() + c.pos, n);
        c.pos += n;
        total += static_cast<uint32_t>(n);
        if (c.pos == c.bytes.size()) m_rx.pop_front();
    }
    return static_cast<int32_t>(total);
}

bool SimDevice::WaitRx(uint32_t timeoutUs) {
    const int64_t deadline = NowUs() + timeoutUs;
    for (;;) {
        const int64_t now = NowUs();
        {
            std::lock_guard<std::mutex> g(m_mutex);
            if (!m_rx.empty() && m_rx.front().readyUs <= now) return true;
        }
        if (now >= deadline) return false;
        SwitchToThread(); // Sleep(1) would hide the modelled response time
    }
}

} // namespace icd
