#include "Sis3316Daq.hpp"

#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <algorithm>
#include <cmath>

namespace sis {

// status byte error bits (mirror of __status_err_check in the Python driver)
static inline void checkStatus(uint8_t stat) {
    if (stat & (1 << 4)) throw DaqError("SIS3316: link interface lost grant (no-grant)");
    if (stat & (1 << 6)) throw DaqError("SIS3316: request command packet protocol error");
    // bit 5 = FIFO-empty/timeout — tolerated by callers that don't read data
}

Sis3316Daq::Sis3316Daq(std::string host, uint16_t port)
    : fHost(std::move(host)), fPort(port) {}

Sis3316Daq::~Sis3316Daq() { close(); }

uint8_t Sis3316Daq::nextId() {
    fReqId = fNextId;
    fNextId = (fNextId >= 0xFF) ? 0 : fNextId + 1;
    return fReqId;
}

void Sis3316Daq::open() {
    if (fSock >= 0) return;
    fSock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fSock < 0) throw DaqError("socket() failed");
    int one = 1;
    setsockopt(fSock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fSock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons(fPort);
    me.sin_addr.s_addr = INADDR_ANY;
    if (::bind(fSock, (sockaddr*)&me, sizeof(me)) < 0) {
        ::close(fSock); fSock = -1;
        throw DaqError("bind() failed on UDP port " + std::to_string(fPort));
    }
    // request link-interface grant
    writeLink(REG_INTERFACE_ARBITRATION, 0x1);
    if (!(readLink(REG_INTERFACE_ARBITRATION) & (1u << 20)))
        throw DaqError("Can't acquire link-interface grant bit");
}

void Sis3316Daq::close() {
    if (fSock < 0) return;
    try { writeLink(REG_INTERFACE_ARBITRATION, 0x0); } catch (...) {}
    ::close(fSock);
    fSock = -1;
}

// --------------------------------------------------------------------------
//  UDP send / recv
// --------------------------------------------------------------------------
void Sis3316Daq::reqSend(const uint8_t* msg, size_t len) {
    // drain any stale datagrams first
    cleanupSocket();
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(fPort);
    inet_pton(AF_INET, fHost.c_str(), &dst.sin_addr);
    ssize_t n = ::sendto(fSock, msg, len, 0, (sockaddr*)&dst, sizeof(dst));
    if (n < 0) throw DaqError("sendto() failed");
}

int Sis3316Daq::respRecv(uint8_t* buf, size_t cap, double timeoutSec) {
    fd_set rd; FD_ZERO(&rd); FD_SET(fSock, &rd);
    timeval tv{};
    tv.tv_sec = (long)timeoutSec;
    tv.tv_usec = (long)((timeoutSec - tv.tv_sec) * 1e6);
    int s = select(fSock + 1, &rd, nullptr, nullptr, &tv);
    if (s <= 0) throw DaqError("response timeout");
    ssize_t n = ::recv(fSock, buf, cap, 0);
    if (n < 0) throw DaqError("recv() failed");
    return (int)n;
}

void Sis3316Daq::cleanupSocket() {
    uint8_t tmp[8192];
    while (true) {
        fd_set rd; FD_ZERO(&rd); FD_SET(fSock, &rd);
        timeval tv{}; // 0 = poll
        if (select(fSock + 1, &rd, nullptr, nullptr, &tv) <= 0) break;
        if (::recv(fSock, tmp, sizeof(tmp), 0) <= 0) break;
    }
}

// --------------------------------------------------------------------------
//  Link interface (0x10 / 0x11)
// --------------------------------------------------------------------------
uint32_t Sis3316Daq::readLink(uint32_t addr) {
    uint8_t id = nextId();
    uint8_t msg[6];
    msg[0] = 0x10; msg[1] = id;
    std::memcpy(msg + 2, &addr, 4);
    reqSend(msg, sizeof(msg));

    uint8_t r[64];
    int n = respRecv(r, sizeof(r), fDefaultTimeout);
    if (n < 10) throw DaqError("malformed link-read response");
    uint32_t raddr, data;
    std::memcpy(&raddr, r + 2, 4);
    std::memcpy(&data, r + 6, 4);
    if (r[0] != 0x10 || raddr != addr) throw DaqError("wrong link-read response");
    if (r[1] != fReqId) throw DaqError("link-read packet-id mismatch");
    return data;
}

void Sis3316Daq::writeLink(uint32_t addr, uint32_t data) {
    uint8_t msg[9];
    msg[0] = 0x11;
    std::memcpy(msg + 1, &addr, 4);
    std::memcpy(msg + 5, &data, 4);
    reqSend(msg, sizeof(msg));   // no acknowledgement
}

// --------------------------------------------------------------------------
//  VME interface (0x20 / 0x21)
// --------------------------------------------------------------------------
uint32_t Sis3316Daq::readVme(uint32_t addr) {
    std::vector<uint32_t> out;
    readVme({addr}, out);
    return out.at(0);
}

void Sis3316Daq::readVme(const std::vector<uint32_t>& addrs, std::vector<uint32_t>& out) {
    for (size_t i = 0; i < addrs.size(); i += VME_READ_LIMIT) {
        size_t cnum = std::min<size_t>(VME_READ_LIMIT, addrs.size() - i);
        std::vector<uint8_t> msg;
        msg.push_back(0x20);
        msg.push_back(nextId());
        uint16_t cm1 = uint16_t(cnum - 1);
        msg.insert(msg.end(), (uint8_t*)&cm1, (uint8_t*)&cm1 + 2);
        for (size_t k = 0; k < cnum; ++k) {
            uint32_t a = addrs[i + k];
            msg.insert(msg.end(), (uint8_t*)&a, (uint8_t*)&a + 4);
        }
        reqSend(msg.data(), msg.size());

        std::vector<uint8_t> r(3 + 4 * cnum + 16);
        int n = respRecv(r.data(), r.size(), fDefaultTimeout);
        if (n < 3) throw DaqError("malformed vme-read response");
        if (r[0] != 0x20) throw DaqError("wrong vme-read response");
        if (r[1] != fReqId) throw DaqError("vme-read packet-id mismatch");
        checkStatus(r[2]);
        for (size_t k = 0; k < cnum; ++k) {
            uint32_t v;
            std::memcpy(&v, r.data() + 3 + 4 * k, 4);
            out.push_back(v);
        }
    }
}

void Sis3316Daq::writeVme(uint32_t addr, uint32_t data) {
    std::vector<uint8_t> msg;
    msg.push_back(0x21);
    msg.push_back(nextId());
    uint16_t cm1 = 0;
    msg.insert(msg.end(), (uint8_t*)&cm1, (uint8_t*)&cm1 + 2);
    msg.insert(msg.end(), (uint8_t*)&addr, (uint8_t*)&addr + 4);
    msg.insert(msg.end(), (uint8_t*)&data, (uint8_t*)&data + 4);
    reqSend(msg.data(), msg.size());

    uint8_t r[16];
    int n = respRecv(r, sizeof(r), fDefaultTimeout);
    if (n < 3) throw DaqError("malformed vme-write response");
    if (r[0] != 0x21) throw DaqError("wrong vme-write response");
    if (r[1] != fReqId) throw DaqError("vme-write packet-id mismatch");
    // FIFO-empty status bit is OK on a write; only check grant/protocol bits
    if (r[2] & (1 << 4)) throw DaqError("vme-write: lost grant");
    if (r[2] & (1 << 6)) throw DaqError("vme-write: protocol error");
}

// --------------------------------------------------------------------------
//  Dispatch (mirrors Python read()/write())
// --------------------------------------------------------------------------
uint32_t Sis3316Daq::read(uint32_t addr) {
    if (addr < 0x20) return readLink(addr);
    if (addr < 0x100000) return readVme(addr);
    throw DaqError("read: address out of range");
}

void Sis3316Daq::write(uint32_t addr, uint32_t data) {
    if (addr < 0x20) writeLink(addr, data);
    else if (addr < 0x100000) writeVme(addr, data);
    else throw DaqError("write: address out of range");
}

double Sis3316Daq::temperatureC() {
    int val = int(read(REG_INTERNAL_TEMPERATURE) & 0x3FF);
    if (val & 0x200) val -= 0x400;   // 10-bit signed
    return val / 4.0;
}

void Sis3316Daq::memToggle() {
    int cur = currentBank();
    if (cur == 0) armBank2(); else armBank1();
}

// --------------------------------------------------------------------------
//  Per-channel readout
// --------------------------------------------------------------------------
EventFormat Sis3316Daq::channelEventFormat(int channel) {
    int gid = channel / 4, cid = channel % 4;
    uint32_t df = readVme(grpReg(DATAFORMAT_CONFIG_REG, gid));
    uint32_t flags = (df >> (8 * cid)) & 0x3F;

    EventFormat f;
    f.acc1      = flags & 0x01;
    f.acc2      = flags & 0x02;
    f.maw       = flags & 0x04;
    f.mawEnergy = flags & 0x08;
    f.mawEna    = flags & 0x10;

    uint32_t rawCfg = readVme(grpReg(RAW_DATA_BUFFER_CONFIG_REG, gid));
    f.nraw16 = int((rawCfg >> 16) & 0xFFFE);                 // raw_window
    uint32_t mawCfg = readVme(grpReg(MAW_TEST_BUFFER_CONFIG_REG, gid));
    f.nmaw = int(mawCfg & 0x3FE);

    // event length in 16-bit words (event_stats), then to 32-bit words
    int elen16 = 6 + f.nraw16
               + (f.acc1 ? 14 : 0)
               + (f.acc2 ? 4 : 0)
               + (f.maw ? 6 : 0)
               + (f.mawEnergy ? 4 : 0)
               + (f.mawEna ? f.nmaw * 2 : 0);
    f.eventLen32 = elen16 / 2;
    return f;
}

uint32_t Sis3316Daq::previousBankWords(int channel) {
    int gid = channel / 4, cid = channel % 4;
    uint32_t reg = grpReg(PREVIOUS_BANK_SAMPLE_ADDRESS_REG, gid) + 4u * uint32_t(cid);
    return readVme(reg) & 0x00FFFFFF;
}

void Sis3316Daq::fifoTransferReset(int gid) {
    writeVme(REG_DATA_TRANSFER_GRP_CTRL + 4u * uint32_t(gid), 0);
}

void Sis3316Daq::fifoTransferReadSetup(int gid, int memNo, uint32_t woffset, uint32_t spaceBits) {
    uint32_t reg = REG_DATA_TRANSFER_GRP_CTRL + 4u * uint32_t(gid);
    if (readVme(reg) & BIT_TRANSFER_BUSY)
        throw DaqError("data-transfer logic busy for group " + std::to_string(gid));
    // command word: read (bit31) | space selector | mem2 select | word offset.
    // spaceBits = 0 for sample/histogram memory, 0x30000000 for statistic
    // counters (manual page 24).
    uint32_t cmd = (0b10u << 30) | spaceBits | woffset;
    if (memNo == 1) cmd |= (1u << 28);
    writeVme(reg, cmd);
}

uint32_t Sis3316Daq::fifoRead(int gid, int memNo, uint32_t nwords, uint32_t woffset,
                              std::vector<uint32_t>& out, uint32_t spaceBits) {
    if (nwords == 0) return 0;
    const uint32_t fifoAddr = FPGA_GRP_MEM_BASE + uint32_t(gid) * FPGA_GRP_MEM_OFFSET;

    // simple, robust congestion window (in 32-bit words)
    const uint32_t wcwndLimit = FIFO_READ_LIMIT;
    uint32_t wcwnd = wcwndLimit / 2;
    uint32_t wcwndMax = wcwnd;
    const uint32_t wmtu = 1440 / 4;

    uint32_t finished = 0;
    int outerRetries = 0;
    while (finished < nwords) {
        try {
            fifoTransferReset(gid);
            fifoTransferReadSetup(gid, memNo, woffset + finished, spaceBits);
        } catch (const DaqError&) {
            if (++outerRetries > 100) throw;
            cleanupSocket();
            continue;
        }

        // Stream successive 0x30 requests without re-setup; break to the outer
        // loop (re-setup at the new offset) on any drop / timeout.
        bool resetup = false;
        while (finished < nwords && !resetup) {
            uint32_t wnum = std::min({nwords - finished, (uint32_t)FIFO_READ_LIMIT, wcwnd});
            if (wnum == 0) wnum = wmtu;
            uint8_t msg[8];
            msg[0] = 0x30; msg[1] = nextId();
            uint16_t wm1 = uint16_t(wnum - 1);
            std::memcpy(msg + 2, &wm1, 2);
            std::memcpy(msg + 4, &fifoAddr, 4);
            reqSend(msg, sizeof(msg));

            uint32_t got = 0;
            int pktIdx = 0;
            try {
                while (got < wnum) {
                    uint8_t buf[9000];
                    int n = respRecv(buf, sizeof(buf), fDefaultTimeout);
                    if (n < 3) throw DaqError("short fifo packet");
                    if (buf[0] != 0x30) throw DaqError("fifo header != 0x30");
                    if (buf[1] != fReqId) throw DaqError("fifo packet-id mismatch");
                    uint8_t stat = buf[2];
                    if (stat & (1 << 4)) throw DaqError("fifo: lost grant");
                    if (stat & (1 << 6)) throw DaqError("fifo: protocol error");
                    if ((stat & 0xF) != (pktIdx & 0xF)) { ++fDroppedPackets; resetup = true; break; } // dropped packet
                    ++pktIdx;
                    int dbytes = n - 3;
                    if (dbytes % 4 != 0) throw DaqError("fifo payload not word-aligned");
                    int dwords = dbytes / 4;
                    for (int k = 0; k < dwords && got < wnum; ++k) {
                        uint32_t v;
                        std::memcpy(&v, buf + 3 + 4 * k, 4);
                        out.push_back(v);
                        ++got;
                    }
                }
                if (!resetup) {                       // full request satisfied: grow window
                    if (wcwndMax > wcwnd) wcwnd += (wcwndMax - wcwnd) / 2;
                    else wcwnd = std::min(wcwndLimit, wcwnd + wmtu);
                }
            } catch (const DaqError&) {                // timeout/congestion: shrink window
                wcwndMax = wcwnd;
                wcwnd = wcwnd / 2;
                resetup = true;
            }
            finished += got;                          // count actually received words once
        }
        if (wcwnd == 0) throw DaqError("fifo read stalled (congestion window collapsed)");
    }
    fifoTransferReset(gid);
    return finished;
}

uint32_t Sis3316Daq::readChannelPreviousBank(int channel, int prevBank,
                                             std::vector<uint32_t>& out) {
    uint32_t nwords = previousBankWords(channel);
    if (nwords == 0) return 0;
    int gid = channel / 4, cid = channel % 4;
    uint32_t woffset = 0;
    if (prevBank == 1) woffset += (1u << 24);   // bank select
    if (cid % 2 == 1)  woffset += (1u << 25);   // odd channel location
    int memNo = (cid < 2) ? 0 : 1;
    return fifoRead(gid, memNo, nwords, woffset, out);
}

// --------------------------------------------------------------------------
//  Hardware info / diagnostics
// --------------------------------------------------------------------------
FirmwareInfo Sis3316Daq::firmwareInfo(int group) {
    FirmwareInfo f;
    f.raw      = readVme(reg::groupReg(reg::ADC_FIRMWARE_VERSION, group));
    f.type     = (f.raw >> 16) & 0xFFFF;
    f.version  = (f.raw >> 8)  & 0xFF;
    f.revision =  f.raw        & 0xFF;
    return f;
}

AdcStatus Sis3316Daq::adcStatus(int group) {
    AdcStatus s;
    s.raw          = readVme(reg::groupReg(reg::ADC_FPGA_STATUS, group));
    s.histClearBusy = s.raw & reg::STAT_HIST_CLEAR_BUSY;
    s.dcmOk         = s.raw & reg::STAT_ADC_DCM_OK;
    s.memory1Ok     = s.raw & reg::STAT_MEMORY1_OK;
    s.memory2Ok     = s.raw & reg::STAT_MEMORY2_OK;
    return s;
}

StatCounters Sis3316Daq::readStatisticCounters(int group) {
    // Manual page 24: select the statistic-counter space via the data-transfer
    // control register, then read 24 (CH_PER_GROUP * STAT_WORDS_PER_CHANNEL=16,
    // but the doc reads 24) 32-bit words from the group memory FIFO. We read the
    // full 4*16 = 64 words to cover all four channels, then keep the documented
    // first 11 per channel.
    StatCounters sc;
    const uint32_t total = reg::CH_PER_GROUP * reg::STAT_WORDS_PER_CHANNEL; // 64
    std::vector<uint32_t> raw;
    raw.reserve(total);
    fifoRead(group, 0, total, 0, raw, reg::XFER_SPACE_STAT_CTR);
    for (int cid = 0; cid < reg::CH_PER_GROUP; ++cid) {
        for (int k = 0; k < reg::SC_COUNT; ++k) {
            size_t idx = size_t(cid) * reg::STAT_WORDS_PER_CHANNEL + k;
            sc.c[cid][k] = (idx < raw.size()) ? raw[idx] : 0;
        }
    }
    return sc;
}

uint32_t Sis3316Daq::readHistogram(int channel, reg::HistType type,
                                   std::vector<uint32_t>& out) {
    const int gid = channel / 4, cid = channel % 4;
    const reg::HistSpec spec = reg::histSpec(type);
    // Odd channels (cid 0,2) live in Memory 1, even channels (cid 1,3) in
    // Memory 2 (manual page 27). The documented 32-bit word offset already
    // differs per parity (byteOffsetOdd / byteOffsetEven).
    const bool even   = (cid % 2 == 1);
    const int  memNo  = even ? 1 : 0;
    const uint32_t woffset = even ? spec.byteOffsetEven : spec.byteOffsetOdd;
    out.clear();
    out.reserve(spec.nbins);
    return fifoRead(gid, memNo, spec.nbins, woffset, out);
}

uint32_t Sis3316Daq::writeVerify(uint32_t addr, uint32_t value) {
    write(addr, value);
    uint32_t rb = read(addr);
    if (rb != value)
        throw DaqError("register 0x" + std::to_string(addr) +
                       " readback mismatch (wrote 0x" + std::to_string(value) +
                       ", read 0x" + std::to_string(rb) + ")");
    return rb;
}

ConfigSnapshot Sis3316Daq::readConfigSnapshot() {
    ConfigSnapshot s;
    for (int g = 0; g < reg::N_GROUPS; ++g) {
        s.eventConfig[g]       = readVme(reg::groupReg(reg::EVENT_CONFIG, g));
        s.channelHeaderId[g]   = readVme(reg::groupReg(reg::CHANNEL_HEADER_ID, g));
        s.dataFormatConfig[g]  = readVme(reg::groupReg(reg::DATAFORMAT_CONFIG, g));
        s.rawDataConfig[g]     = readVme(reg::groupReg(reg::RAW_DATA_BUFFER_CONFIG, g));
        s.activeTrigGateLen[g] = readVme(reg::groupReg(reg::ACTIVE_TRIGGER_GATE_LEN, g));
        s.preTrigDelay[g]      = readVme(reg::groupReg(reg::PRE_TRIGGER_DELAY, g));
        s.pileupConfig[g]      = readVme(reg::groupReg(reg::PILEUP_CONFIG, g));
        for (int gate = 1; gate <= 4; ++gate)
            s.accGateConfig[g][gate-1] = readVme(reg::groupReg(reg::accumGateConfig(gate), g));
        s.tofActiveWindow[g]   = readVme(reg::groupReg(reg::TOF_ACTIVE_WINDOW_CONFIG, g));
        s.generalHistConfig[g] = readVme(reg::groupReg(reg::GENERAL_HISTOGRAM_CONFIG, g));
        s.tofHistConfig[g]     = readVme(reg::groupReg(reg::TOF_HISTOGRAM_CONFIG, g));
        s.shapeHistXConfig[g]  = readVme(reg::groupReg(reg::SHAPE_HIST_X_CONFIG, g));
        s.shapeHistYConfig[g]  = readVme(reg::groupReg(reg::SHAPE_HIST_Y_CONFIG, g));
        s.peakSumHistConfig[g] = readVme(reg::groupReg(reg::PEAK_SUM_HIST_CONFIG, g));
        FirmwareInfo fw = firmwareInfo(g);
        s.firmwareType[g] = fw.type; s.firmwareVersion[g] = fw.version;
        s.firmwareRevision[g] = fw.revision;
        for (int cid = 0; cid < reg::CH_PER_GROUP; ++cid) {
            int ch = g * reg::CH_PER_GROUP + cid;
            s.firTrigSetup[ch]   = readVme(reg::groupReg(reg::firTriggerSetup(cid), g));
            s.trigThreshold[ch]  = readVme(reg::groupReg(reg::triggerThreshold(cid), g));
            s.heTrigThreshold[ch] = readVme(reg::groupReg(reg::heTriggerThreshold(cid), g));
        }
    }
    return s;
}

} // namespace sis
