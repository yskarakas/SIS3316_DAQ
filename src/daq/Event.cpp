#include "Event.hpp"

namespace sis {

std::vector<Sis3316Event> parseBuffer(const uint32_t* w, size_t nwords,
                                      const EventFormat& fmt, int channel,
                                      uint64_t& ridCounter) {
    std::vector<Sis3316Event> out;
    if (!fmt.valid() || nwords == 0) return out;

    const size_t evLen = size_t(fmt.eventLen32);
    const size_t nEvents = nwords / evLen;
    out.reserve(nEvents);

    for (size_t e = 0; e < nEvents; ++e) {
        const uint32_t* ev = w + e * evLen;
        Sis3316Event s;
        s.rid = ridCounter++;
        s.channel = channel;

        const uint32_t chFmt = ev[0] & 0xFFFF;
        s.det = int(chFmt >> 4);
        s.timestamp = (uint64_t(ev[0] & 0xFFFF0000) << 16) | uint64_t(ev[1]);

        size_t pos = 2;
        if (fmt.acc1) {
            s.hasAcc1 = true;
            s.adcMax    = ev[pos] & 0xFFFF;
            s.adcArgmax = ev[pos] >> 16;
            s.gate[0]   = ev[pos + 1] & 0x00FFFFFF;          // gate1 (24-bit)
            uint32_t info = ev[pos + 1] >> 28;               // pileup/over/underflow
            s.pileup = int((info & 0b1) | (info & 0b10));
            s.gate[1] = ev[pos + 2];                         // gate2
            s.gate[2] = ev[pos + 3];                         // gate3
            s.gate[3] = ev[pos + 4];                         // gate4
            s.gate[4] = ev[pos + 5];                         // gate5
            s.gate[5] = ev[pos + 6];                         // gate6
            pos += 7;
        }
        if (fmt.acc2) {
            s.hasAcc2 = true;
            s.gate[6] = ev[pos];                             // gate7
            s.gate[7] = ev[pos + 1];                         // gate8
            pos += 2;
        }
        if (fmt.maw)        pos += 3;
        if (fmt.mawEnergy)  pos += 2;

        // raw-sample 32-bit-word count is self-describing in the format word
        uint32_t nraw32 = ev[pos] & 0x03FFFFFF;
        pos += 1;

        // clamp to the event boundary (numpy-slice-clamp parity with Python)
        if (pos + nraw32 > evLen) nraw32 = (pos < evLen) ? uint32_t(evLen - pos) : 0;

        if (nraw32) {
            s.raw.reserve(size_t(nraw32) * 2);
            for (uint32_t k = 0; k < nraw32; ++k) {
                uint32_t word = ev[pos + k];
                s.raw.push_back(uint16_t(word & 0xFFFF));
                s.raw.push_back(uint16_t(word >> 16));
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace sis
