// CliMain.cpp - IcdLinkCli.exe, the same engine as IcdLink.dll driven from the
// command line for .bat automation. Run "IcdLinkCli help" for the command list.
//
// Exit codes: 0 pass, 1 expect failed, 2 bad command, 3 timeout, 4 device error.
#include "IcdLinkApi.h"
#include "IcdMessages.h"
#include "LinkEngine.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace icd;

namespace {

const int kPass = 0, kExpectFailed = 1, kUsage = 2, kTimeoutExit = 3, kDevice = 4;
const char* kPipePrefix = "\\\\.\\pipe\\IcdLink_";

typedef std::vector<std::string> Args;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

bool EqualsNoCase(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

Args Split(const std::string& text) {
    Args out;
    std::string cur;
    bool quoted = false, any = false;
    for (char ch : text) {
        if (ch == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
            if (any) out.push_back(cur);
            cur.clear(); any = false;
            continue;
        }
        cur.push_back(ch); any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) { sprintf_s(b, "%02X", p[i]); if (i) s += ' '; s += b; }
    return s;
}

std::string FormatValue(double v, const FieldDesc& f) {
    char b[64];
    switch (f.kind) {
    case FieldKind::Float32: case FieldKind::Float64: sprintf_s(b, "%.9g", v); return b;
    case FieldKind::Bool: return v != 0 ? "1" : "0";
    default:
        if (f.scale != 1.0 || f.offset != 0.0) sprintf_s(b, "%.9g", v);
        else sprintf_s(b, "%.0f", v);
        return b;
    }
}

bool ParseUInt(const std::string& s, uint32_t* out) {
    char* end = nullptr;
    const unsigned long v = strtoul(s.c_str(), &end, 0);
    if (s.empty() || *end) return false;
    *out = v;
    return true;
}

class Cli {
public:
    Cli() : m_schema(icdgen::GetSchema()) {
        IL_Create(&m_h);
        IL_DefaultConfig(0, &m_cfg[0]);
        IL_DefaultConfig(1, &m_cfg[1]);
        m_tx.assign(m_schema.txTotal, 0);
        m_rx.assign(m_schema.rxTotal, 0);
        m_dirty.assign(m_schema.messageCount, false);
        uint32_t t = 0, r = 0;
        for (int i = 0; i < m_schema.messageCount; ++i) {
            m_txOff.push_back(t); m_rxOff.push_back(r);
            t += m_schema.messages[i].txLen; r += m_schema.messages[i].rxLen;
        }
    }
    ~Cli() { IL_Destroy(m_h); }

    bool ShutdownRequested() const { return m_shutdown; }

    // Runs "a then b then c". Stops at the first non-zero result.
    int RunChain(const Args& args, std::ostream& out) {
        Args cur;
        for (size_t i = 0; i <= args.size(); ++i) {
            if (i == args.size() || EqualsNoCase(args[i], "then")) {
                if (!cur.empty()) {
                    const int r = Exec(cur, out);
                    if (r != kPass) return r;
                }
                cur.clear();
            } else cur.push_back(args[i]);
        }
        return kPass;
    }

    int RunScript(std::istream& in, std::ostream& out) {
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            const Args a = Split(line);
            if (a.empty()) continue;
            const int r = RunChain(a, out);
            if (r != kPass) { out << "stopped at line " << number << ": " << line << "\n"; return r; }
        }
        return kPass;
    }

private:
    struct Path { int msg = -1; int field = -1; int side = 0; };

    int Fail(int status, std::ostream& out, const char* what) {
        char text[512] = {0};
        IL_LastError(m_h, text, sizeof(text));
        out << "error: " << what << " failed (status " << status << ")";
        if (text[0]) out << ": " << text;
        out << "\n";
        if (status == ErrTimeout || status == ErrBusy) return kTimeoutExit;
        return (status == ErrArg) ? kUsage : kDevice;
    }

    bool Resolve(const std::string& text, Path* p, bool needField, std::ostream& out) {
        std::vector<std::string> parts;
        std::stringstream ss(text);
        std::string item;
        while (std::getline(ss, item, '.')) parts.push_back(item);
        if (parts.size() != (needField ? 3u : 2u) && !(parts.size() == 3 && !needField)) {
            out << "error: expected ToC.Message" << (needField ? ".Field" : "") << ", got " << text << "\n";
            return false;
        }
        const std::string head = Lower(parts[0]);
        char channel = 0;
        if (head == "toc") { p->side = 0; channel = 'C'; }
        else if (head == "tod") { p->side = 0; channel = 'D'; }
        else if (head == "fromc") { p->side = 1; channel = 'C'; }
        else if (head == "fromd") { p->side = 1; channel = 'D'; }
        else { out << "error: path must start with ToC, ToD, FromC or FromD\n"; return false; }

        for (int i = 0; i < m_schema.messageCount; ++i) {
            const MsgDesc& m = m_schema.messages[i];
            if (m.channel != channel || !EqualsNoCase(m.name, parts[1])) continue;
            p->msg = i;
            if (parts.size() == 3) {
                const uint8_t dirBit = p->side == 0 ? DirTo : DirFrom;
                for (int f = 0; f < m.fieldCount; ++f) {
                    if (EqualsNoCase(m.fields[f].name, parts[2])) {
                        if (!(m.fields[f].dir & dirBit)) {
                            out << "error: " << text << " does not exist in that direction\n";
                            return false;
                        }
                        p->field = f;
                        return true;
                    }
                }
                out << "error: no field " << parts[2] << " in " << m.name << "\n";
                return false;
            }
            return true;
        }
        out << "error: no message " << parts[1] << " on channel " << channel << "\n";
        return false;
    }

    bool ParseValue(const std::string& text, const FieldDesc& f, double* out) const {
        const std::string t = Lower(text);
        if (t == "true" || t == "on") { *out = 1; return true; }
        if (t == "false" || t == "off") { *out = 0; return true; }
        if (f.kind == FieldKind::Enum) {
            std::stringstream ss(f.enumText);
            std::string entry;
            while (std::getline(ss, entry, ',')) {
                const size_t eq = entry.find('=');
                if (eq != std::string::npos && EqualsNoCase(entry.substr(eq + 1), text)) {
                    *out = atof(entry.substr(0, eq).c_str());
                    return true;
                }
            }
        }
        char* end = nullptr;
        double v;
        if (t.rfind("0x", 0) == 0) v = static_cast<double>(strtoull(t.c_str(), &end, 16));
        else v = strtod(t.c_str(), &end);
        if (t.empty() || *end) return false;
        *out = v;
        return true;
    }

    void RefreshAll() { IL_Snapshot(m_h, nullptr, m_rx.data(), nullptr, nullptr); }

    int Begin(uint32_t ms, std::ostream& out) {
        if (m_token) { out << "error: a transaction is already open\n"; return kUsage; }
        const int s = IL_TxnBegin(m_h, ms, &m_token);
        if (s != Ok) { m_token = 0; return Fail(s, out, "begin"); }
        IL_Snapshot(m_h, m_tx.data(), m_rx.data(), nullptr, nullptr);
        std::fill(m_dirty.begin(), m_dirty.end(), false);
        return kPass;
    }

    int Publish(std::ostream& out) {
        std::vector<int32_t> ids;
        std::vector<uint8_t> flat;
        for (int i = 0; i < m_schema.messageCount; ++i) {
            if (!m_dirty[i]) continue;
            ids.push_back(i);
            flat.insert(flat.end(), m_tx.begin() + m_txOff[i], m_tx.begin() + m_txOff[i] + m_schema.messages[i].txLen);
        }
        if (ids.empty()) return kPass;
        const int s = IL_Commit(m_h, m_token, ids.data(), static_cast<int32_t>(ids.size()), flat.data(), &m_lastSeq);
        if (s != Ok) return Fail(s, out, "commit");
        std::fill(m_dirty.begin(), m_dirty.end(), false);
        return kPass;
    }

    int EndTxn(bool commit, std::ostream& out) {
        if (!m_token) { out << "error: no transaction is open\n"; return kUsage; }
        int r = kPass;
        if (commit) r = Publish(out);
        IL_TxnEnd(m_h, m_token);
        m_token = 0;
        return r;
    }

    bool ReadField(const Path& p, double* value) {
        const MsgDesc& m = m_schema.messages[p.msg];
        std::vector<uint8_t> image(p.side == 0 ? m.txLen : m.rxLen);
        if (m_token) {
            const uint8_t* src = p.side == 0 ? &m_tx[m_txOff[p.msg]] : &m_rx[m_rxOff[p.msg]];
            std::copy(src, src + image.size(), image.begin());
        } else if (IL_ReadMsg(m_h, p.msg, p.side, image.data(), nullptr) != Ok) return false;
        *value = GetValue(image.data(), m.fields[p.field]);
        return true;
    }

    int CmdSet(const Args& a, std::ostream& out) {
        if (a.size() != 3) { out << "usage: set ToC.Msg.Field value\n"; return kUsage; }
        Path p;
        if (!Resolve(a[1], &p, true, out)) return kUsage;
        if (p.side != 0) { out << "error: only To paths can be set\n"; return kUsage; }
        const FieldDesc& f = m_schema.messages[p.msg].fields[p.field];
        double v;
        if (!ParseValue(a[2], f, &v)) { out << "error: bad value " << a[2] << "\n"; return kUsage; }

        const bool autoTxn = m_token == 0;
        if (autoTxn) { const int r = Begin(2000, out); if (r != kPass) return r; }
        if (!SetValue(&m_tx[m_txOff[p.msg]], f, v)) {
            out << "error: value " << a[2] << " does not fit " << f.name << "\n";
            if (autoTxn) EndTxn(false, out);
            return kUsage;
        }
        m_dirty[p.msg] = true;
        return autoTxn ? EndTxn(true, out) : kPass;
    }

    int CmdExpect(const Args& a, std::ostream& out) {
        // expect Path op value [within ms]
        if (a.size() != 4 && a.size() != 6) { out << "usage: expect FromC.Msg.Field == value [within ms]\n"; return kUsage; }
        Path p;
        if (!Resolve(a[1], &p, true, out)) return kUsage;
        const FieldDesc& f = m_schema.messages[p.msg].fields[p.field];
        double want, have = 0;
        if (!ParseValue(a[3], f, &want)) { out << "error: bad value " << a[3] << "\n"; return kUsage; }
        uint32_t within = 0;
        if (a.size() == 6 && (!EqualsNoCase(a[4], "within") || !ParseUInt(a[5], &within))) { out << "error: expected: within ms\n"; return kUsage; }
        if (m_token && within) { out << "error: 'within' cannot be used inside a transaction, use sync\n"; return kUsage; }

        const std::string& op = a[2];
        const int64_t deadline = NowUs() + static_cast<int64_t>(within) * 1000;
        for (;;) {
            if (!ReadField(p, &have)) { out << "error: read failed\n"; return kDevice; }
            bool ok;
            if (op == "==") ok = have == want;
            else if (op == "!=") ok = have != want;
            else if (op == "<") ok = have < want;
            else if (op == "<=") ok = have <= want;
            else if (op == ">") ok = have > want;
            else if (op == ">=") ok = have >= want;
            else { out << "error: unknown operator " << op << "\n"; return kUsage; }
            if (ok) { out << "PASS " << a[1] << " = " << FormatValue(have, f) << "\n"; return kPass; }
            if (NowUs() >= deadline) break;
            Sleep(1);
        }
        out << "FAIL " << a[1] << " = " << FormatValue(have, f) << ", expected " << op << " " << a[3] << "\n";
        return kExpectFailed;
    }

    int CmdDump(const Args& a, std::ostream& out) {
        if (a.size() != 2) { out << "usage: dump ToC.Msg\n"; return kUsage; }
        Path p;
        if (!Resolve(a[1], &p, false, out)) return kUsage;
        const MsgDesc& m = m_schema.messages[p.msg];
        const uint16_t len = p.side == 0 ? m.txLen : m.rxLen;
        std::vector<uint8_t> image(len);
        MsgMeta meta = {};
        if (m_token) {
            const uint8_t* src = p.side == 0 ? &m_tx[m_txOff[p.msg]] : &m_rx[m_rxOff[p.msg]];
            std::copy(src, src + len, image.begin());
        }
        const int s = IL_ReadMsg(m_h, p.msg, p.side, m_token ? nullptr : image.data(), reinterpret_cast<IL_MsgMeta*>(&meta));
        if (s != Ok) return Fail(s, out, "read");
        out << a[1] << " [" << Hex(image.data(), len) << "]";
        if (p.side == 1) out << " seq=" << meta.rxSeq << " valid=" << meta.rxValid;
        out << "\n";
        const uint8_t bit = p.side == 0 ? DirTo : DirFrom;
        for (int i = 0; i < m.fieldCount; ++i)
            if (m.fields[i].dir & bit)
                out << "  " << m.fields[i].name << " = " << FormatValue(GetValue(image.data(), m.fields[i]), m.fields[i]) << "\n";
        return kPass;
    }

    int ChannelArg(const std::string& s, std::ostream& out) {
        if (EqualsNoCase(s, "C")) return 0;
        if (EqualsNoCase(s, "D")) return 1;
        out << "error: channel must be C or D\n";
        return -1;
    }

    int OpenOne(int ch, const std::string& kind, const std::string& arg, std::ostream& out) {
        int s;
        if (kind == "sim") s = IL_OpenSim(m_h, ch, &m_cfg[ch]);
        else if (kind == "ftdi") s = IL_OpenFtdi(m_h, ch, arg.empty() ? nullptr : arg.c_str(), &m_cfg[ch]);
        else s = IL_OpenCom(m_h, ch, arg.c_str(), &m_cfg[ch]);
        if (s != Ok) return Fail(s, out, "open");
        char warn[512] = {0};
        IL_LastWarning(m_h, warn, sizeof(warn));
        if (warn[0]) out << "warning: " << warn << "\n";
        return kPass;
    }

    int CmdOpen(const Args& a, std::ostream& out) {
        if (a.size() < 2) { out << "usage: open sim | open ftdi [serial] | open com C=COM5 D=COM6\n"; return kUsage; }
        const std::string kind = Lower(a[1]);
        if (kind == "sim" || kind == "ftdi") {
            const std::string serial = a.size() > 2 ? a[2] : std::string();
            int r = OpenOne(0, kind, serial, out);
            if (r == kPass) r = OpenOne(1, kind, serial, out);
            return r;
        }
        if (kind == "com") {
            for (size_t i = 2; i < a.size(); ++i) {
                const size_t eq = a[i].find('=');
                const int ch = eq == std::string::npos ? -1 : ChannelArg(a[i].substr(0, eq), out);
                if (ch < 0) return kUsage;
                const int r = OpenOne(ch, "com", a[i].substr(eq + 1), out);
                if (r != kPass) return r;
            }
            return kPass;
        }
        out << "error: unknown transport " << a[1] << "\n";
        return kUsage;
    }

    int CmdConfig(const Args& a, std::ostream& out) {
        if (a.size() < 3) { out << "usage: config C baud=N period=us window=us rxtimeout=us ff=0|1 echo=0|1\n"; return kUsage; }
        const int ch = ChannelArg(a[1], out);
        if (ch < 0) return kUsage;
        for (size_t i = 2; i < a.size(); ++i) {
            const size_t eq = a[i].find('=');
            uint32_t v = 0;
            if (eq == std::string::npos || !ParseUInt(a[i].substr(eq + 1), &v)) { out << "error: bad setting " << a[i] << "\n"; return kUsage; }
            const std::string key = Lower(a[i].substr(0, eq));
            if (key == "baud") m_cfg[ch].baud = v;
            else if (key == "period") m_cfg[ch].periodUs = v;
            else if (key == "window") m_cfg[ch].windowUs = v;
            else if (key == "rxtimeout") m_cfg[ch].rxTimeoutUs = v;
            else if (key == "ff") m_cfg[ch].trailerFf = v;
            else if (key == "echo") m_cfg[ch].rs485Echo = v;
            else { out << "error: unknown setting " << key << "\n"; return kUsage; }
        }
        return kPass;
    }

    int CmdStats(const Args& a, std::ostream& out) {
        if (a.size() != 2) { out << "usage: stats C|D\n"; return kUsage; }
        const int ch = ChannelArg(a[1], out);
        if (ch < 0) return kUsage;
        IL_ChannelStats s;
        const int st = IL_GetStats(m_h, ch, &s);
        if (st != Ok) return Fail(st, out, "stats");
        out << "slots=" << s.slots << " tx=" << s.txFrames << " rx=" << s.rxFrames << " crcErrors=" << s.crcErrors
            << " rxTimeouts=" << s.rxTimeouts << " windowMisses=" << s.windowMisses << " missedSlots=" << s.missedSlots
            << " strayBytes=" << s.strayBytes << " ioErrors=" << s.ioErrors << " oneShots=" << s.oneShots
            << " longOps=" << s.longOps << " maxLatenessUs=" << s.maxLatenessUs << " lastExchangeUs=" << s.lastExchangeUs << "\n";
        return kPass;
    }

    int CmdSchema(std::ostream& out) {
        for (int i = 0; i < m_schema.messageCount; ++i) {
            const MsgDesc& m = m_schema.messages[i];
            char head[128];
            sprintf_s(head, "%c 0x%02X %s to=%d from=%d %s", m.channel, m.id, m.name, m.txLen, m.rxLen, m.cyclic ? "cyclic" : "on-demand");
            out << head << "\n";
            for (int f = 0; f < m.fieldCount; ++f)
                out << "  " << m.fields[f].name << " byte=" << m.fields[f].byteOffset << " low=" << m.fields[f].bitLow
                    << " width=" << m.fields[f].bitWidth << " dir=" << static_cast<int>(m.fields[f].dir) << "\n";
        }
        return kPass;
    }

    int Exec(const Args& a, std::ostream& out) {
        const std::string c = Lower(a[0]);
        if (c == "help") { PrintHelp(out); return kPass; }
        if (c == "list") {
            char text[8192] = {0};
            const int n = IL_ListFtdi(text, sizeof(text));
            out << "FTDI interfaces (index, chip, channel, description, kind, state):\n" << text;
            if (n < 0) out << "\n";
            IL_ListCom(text, sizeof(text));
            out << "COM ports:\n" << text;
            return kPass;
        }
        if (c == "schema") return CmdSchema(out);
        if (c == "config") return CmdConfig(a, out);
        if (c == "open") return CmdOpen(a, out);
        if (c == "close") { IL_Close(m_h, 0); IL_Close(m_h, 1); return kPass; }
        if (c == "wait") {
            uint32_t ms = 0;
            if (a.size() != 2 || !ParseUInt(a[1], &ms)) { out << "usage: wait ms\n"; return kUsage; }
            Sleep(ms);
            return kPass;
        }
        if (c == "echo") { for (size_t i = 1; i < a.size(); ++i) out << (i > 1 ? " " : "") << a[i]; out << "\n"; return kPass; }
        if (c == "cyclic") {
            if (a.size() != 3) { out << "usage: cyclic C on|off\n"; return kUsage; }
            const int ch = ChannelArg(a[1], out);
            if (ch < 0) return kUsage;
            const int s = IL_SetCyclic(m_h, ch, EqualsNoCase(a[2], "on") ? 1 : 0);
            return s == Ok ? kPass : Fail(s, out, "cyclic");
        }
        if (c == "cyclicmsg") {
            if (a.size() != 3) { out << "usage: cyclicmsg ToC.Msg on|off\n"; return kUsage; }
            Path p;
            if (!Resolve(a[1], &p, false, out)) return kUsage;
            const int s = IL_SetCyclicMessage(m_h, p.msg, EqualsNoCase(a[2], "on") ? 1 : 0);
            return s == Ok ? kPass : Fail(s, out, "cyclicmsg");
        }
        if (c == "period") {
            uint32_t us = 0;
            if (a.size() != 3 || !ParseUInt(a[2], &us)) { out << "usage: period C us\n"; return kUsage; }
            const int ch = ChannelArg(a[1], out);
            if (ch < 0) return kUsage;
            const int s = IL_SetPeriod(m_h, ch, us);
            return s == Ok ? kPass : Fail(s, out, "period");
        }
        if (c == "begin") { uint32_t ms = 2000; if (a.size() > 1) ParseUInt(a[1], &ms); return Begin(ms, out); }
        if (c == "commit") { if (!m_token) { out << "error: no transaction is open\n"; return kUsage; } return Publish(out); }
        if (c == "end") return EndTxn(true, out);
        if (c == "abort") return EndTxn(false, out);
        if (c == "sync") {
            uint32_t ms = 1000;
            if (a.size() > 1 && !ParseUInt(a[1], &ms)) { out << "usage: sync [timeoutMs]\n"; return kUsage; }
            if (m_token) { const int r = Publish(out); if (r != kPass) return r; }
            uint64_t seq = m_lastSeq;
            if (!m_token) IL_Snapshot(m_h, nullptr, nullptr, nullptr, &seq);
            const int s = IL_WaitApplied(m_h, seq, ms);
            if (s != Ok) return Fail(s, out, "sync");
            if (m_token) RefreshAll();
            return kPass;
        }
        if (c == "set") return CmdSet(a, out);
        if (c == "get") {
            if (a.size() != 2) { out << "usage: get FromC.Msg.Field\n"; return kUsage; }
            Path p;
            if (!Resolve(a[1], &p, true, out)) return kUsage;
            double v;
            if (!ReadField(p, &v)) { out << "error: read failed\n"; return kDevice; }
            out << FormatValue(v, m_schema.messages[p.msg].fields[p.field]) << "\n";
            return kPass;
        }
        if (c == "expect") return CmdExpect(a, out);
        if (c == "dump") return CmdDump(a, out);
        if (c == "send") {
            if (a.size() < 2) { out << "usage: send ToC.Msg [timeoutMs]\n"; return kUsage; }
            Path p;
            if (!Resolve(a[1], &p, false, out)) return kUsage;
            uint32_t ms = 1000;
            if (a.size() > 2) ParseUInt(a[2], &ms);
            if (m_token) { const int r = Publish(out); if (r != kPass) return r; }
            std::vector<uint8_t> rx(m_schema.messages[p.msg].rxLen);
            const int s = IL_Exchange(m_h, p.msg, ms, rx.data());
            if (s != Ok) return Fail(s, out, "send");
            if (m_token) RefreshAll();
            if (m_schema.messages[p.msg].hasResponse) out << "rx [" << Hex(rx.data(), rx.size()) << "]\n";
            return kPass;
        }
        if (c == "longop") {
            if (a.size() < 3) { out << "usage: longop begin|end C [watchdogMs]\n"; return kUsage; }
            const int ch = ChannelArg(a[2], out);
            if (ch < 0) return kUsage;
            if (EqualsNoCase(a[1], "begin")) {
                uint32_t ms = 30000;
                if (a.size() > 3) ParseUInt(a[3], &ms);
                const int s = IL_LongOpBegin(m_h, ch, ms, 1000);
                return s == Ok ? kPass : Fail(s, out, "longop begin");
            }
            const int s = IL_LongOpEnd(m_h, ch);
            return s == Ok ? kPass : Fail(s, out, "longop end");
        }
        if (c == "stats") return CmdStats(a, out);
        if (c == "resetstats") { IL_ResetStats(m_h, 0); IL_ResetStats(m_h, 1); return kPass; }
        if (c == "shutdown") { m_shutdown = true; return kPass; }
        out << "error: unknown command " << a[0] << " (try help)\n";
        return kUsage;
    }

public:
    static void PrintHelp(std::ostream& out) {
        out <<
            "IcdLinkCli <command> [then <command>]...   run in this process\n"
            "IcdLinkCli script <file|->                 run a command file (or stdin)\n"
            "IcdLinkCli serve [name]                    hold the link open for 'call'\n"
            "IcdLinkCli call [--pipe name] <command>... send a command to 'serve'\n"
            "\nCommands:\n"
            "  list | schema | help | echo text | wait ms\n"
            "  config C baud=N period=us window=us rxtimeout=us ff=0|1 echo=0|1\n"
            "  open sim | open ftdi [serial] | open com C=COM5 D=COM6 | close\n"
            "  cyclic C on|off | cyclicmsg ToC.Msg on|off | period C us\n"
            "  set ToC.Msg.Field value | get FromC.Msg.Field | dump FromC.Msg\n"
            "  expect FromC.Msg.Field == value [within ms]   (== != < <= > >=)\n"
            "  send ToC.Msg [timeoutMs]\n"
            "  begin [ms] | commit | sync [ms] | end | abort\n"
            "  longop begin C [watchdogMs] | longop end C\n"
            "  stats C | resetstats | shutdown (serve only)\n"
            "\nExit codes: 0 pass, 1 expect failed, 2 bad command, 3 timeout, 4 device error\n";
    }

private:
    const Schema& m_schema;
    IL_Handle m_h = nullptr;
    IL_ChannelConfig m_cfg[2];
    uint64_t m_token = 0, m_lastSeq = 0;
    std::vector<uint8_t> m_tx, m_rx;
    std::vector<uint32_t> m_txOff, m_rxOff;
    std::vector<bool> m_dirty;
    bool m_shutdown = false;
};

int Serve(const std::string& name) {
    const std::string path = kPipePrefix + name;
    Cli cli;
    std::cout << "serving on " << path << std::endl;
    while (!cli.ShutdownRequested()) {
        HANDLE pipe = CreateNamedPipeA(path.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { std::cerr << "cannot create pipe " << path << "\n"; return kDevice; }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buffer[65536];
            DWORD got = 0;
            if (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr)) {
                std::ostringstream out;
                const int code = cli.RunChain(Split(std::string(buffer, got)), out);
                const std::string reply = std::to_string(code) + "\n" + out.str();
                DWORD sent = 0;
                WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &sent, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
    return kPass;
}

int Call(Args args) {
    std::string name = "default";
    if (args.size() >= 2 && args[0] == "--pipe") { name = args[1]; args.erase(args.begin(), args.begin() + 2); }
    if (args.empty()) { std::cerr << "usage: call [--pipe name] <command>\n"; return kUsage; }
    std::string line;
    for (const std::string& a : args) line += "\"" + a + "\" ";
    const std::string path = kPipePrefix + name;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 500; ++tries) {
        pipe = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        Sleep(10);
    }
    if (pipe == INVALID_HANDLE_VALUE) { std::cerr << "error: no server on " << path << "\n"; return kDevice; }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    DWORD sent = 0, got = 0;
    if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &sent, nullptr)) { CloseHandle(pipe); return kDevice; }
    std::vector<char> buffer(65536);
    const BOOL ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr);
    CloseHandle(pipe);
    if (!ok) { std::cerr << "error: no reply from server\n"; return kDevice; }
    const std::string reply(buffer.data(), got);
    const size_t nl = reply.find('\n');
    if (nl == std::string::npos) return kDevice;
    std::cout << reply.substr(nl + 1);
    return atoi(reply.substr(0, nl).c_str());
}

} // namespace

int main(int argc, char** argv) {
    Args args(argv + 1, argv + argc);
    if (args.empty()) { Cli::PrintHelp(std::cout); return kUsage; }
    const std::string head = Lower(args[0]);
    if (head == "serve") return Serve(args.size() > 1 ? args[1] : "default");
    if (head == "call") { args.erase(args.begin()); return Call(args); }

    Cli cli;
    int code;
    if (head == "script") {
        if (args.size() != 2) { std::cerr << "usage: script <file|->\n"; return kUsage; }
        if (args[1] == "-") code = cli.RunScript(std::cin, std::cout);
        else {
            std::ifstream in(args[1]);
            if (!in) { std::cerr << "error: cannot open " << args[1] << "\n"; return kUsage; }
            code = cli.RunScript(in, std::cout);
        }
    } else {
        code = cli.RunChain(args, std::cout);
    }
    std::cout.flush();
    return code;
}
