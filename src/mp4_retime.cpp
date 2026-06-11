#include "mp4_retime.h"
#include "ffmpeg_process.h"
#include "win32_utils.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using sdr2hdr::win32::toWide;

namespace {

// ---------------------------------------------------------------------------
// Big-endian field access into a byte buffer
// ---------------------------------------------------------------------------

uint32_t rd32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}

uint64_t rd64(const uint8_t* p)
{
    return (uint64_t(rd32(p)) << 32) | rd32(p + 4);
}

void wr32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >>  8); p[3] = uint8_t(v);
}

void wr64(uint8_t* p, uint64_t v)
{
    wr32(p, uint32_t(v >> 32));
    wr32(p + 4, uint32_t(v));
}

// ---------------------------------------------------------------------------
// Box tree (moov only -- a few MB, safe to hold in memory)
// ---------------------------------------------------------------------------

struct Box
{
    uint32_t             type = 0;          // 4cc as big-endian uint
    bool                 container = false;
    std::vector<uint8_t> payload;           // leaf body (after 8-byte header)
    std::vector<Box>     children;          // container body
};

constexpr uint32_t fourcc(const char (&s)[5])
{
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) <<  8) |  uint32_t(uint8_t(s[3]));
}

bool isContainerType(uint32_t t)
{
    return t == fourcc("moov") || t == fourcc("trak") || t == fourcc("mdia") ||
           t == fourcc("minf") || t == fourcc("stbl") || t == fourcc("edts");
}

// Parse a run of sibling boxes from buf[0..len). Returns false on any
// malformed size field -- caller then leaves the file untouched.
bool parseBoxes(const uint8_t* buf, size_t len, std::vector<Box>& out)
{
    size_t off = 0;
    while (off < len)
    {
        if (len - off < 8) return false;
        uint64_t size = rd32(buf + off);
        const uint32_t type = rd32(buf + off + 4);
        size_t header = 8;
        if (size == 1)
        {
            if (len - off < 16) return false;
            size = rd64(buf + off + 8);
            header = 16;
        }
        else if (size == 0)
        {
            size = len - off;   // extends to end of parent
        }
        if (size < header || size > len - off) return false;

        Box b;
        b.type = type;
        b.container = isContainerType(type);
        const uint8_t* body = buf + off + header;
        const size_t bodyLen = static_cast<size_t>(size) - header;
        if (b.container)
        {
            if (!parseBoxes(body, bodyLen, b.children)) return false;
        }
        else
        {
            b.payload.assign(body, body + bodyLen);
        }
        out.push_back(std::move(b));
        off += static_cast<size_t>(size);
    }
    return true;
}

size_t serializedSize(const Box& b)
{
    size_t s = 8;
    if (b.container)
        for (const Box& c : b.children) s += serializedSize(c);
    else
        s += b.payload.size();
    return s;
}

void serialize(const Box& b, std::vector<uint8_t>& out)
{
    const size_t size = serializedSize(b);
    uint8_t hdr[8];
    wr32(hdr, static_cast<uint32_t>(size));
    wr32(hdr + 4, b.type);
    out.insert(out.end(), hdr, hdr + 8);
    if (b.container)
        for (const Box& c : b.children) serialize(c, out);
    else
        out.insert(out.end(), b.payload.begin(), b.payload.end());
}

Box* findChild(Box& parent, uint32_t type)
{
    for (Box& c : parent.children)
        if (c.type == type) return &c;
    return nullptr;
}

bool fail(const char* why)
{
    fprintf(stderr, "applyMp4FramePts: %s -- keeping CFR timing.\n", why);
    return false;
}

} // namespace

bool applyMp4FramePts(const std::string& mp4Path,
                      const FramePtsInfo& framePts,
                      bool verbose)
{
    const size_t n = framePts.pts.size();
    if (n < 2 || framePts.tbNum == 0 || framePts.tbDen == 0)
        return fail("no usable source timestamps");

    // ------------------------------------------------------------------
    // 1. Convert source pts -> 90 kHz ticks, normalised to start at 0.
    //
    // 90 kHz: fine enough for any frame rate (11 us steps), and a 32-bit
    // mdhd duration still covers 13 h. Deltas are differences of *rounded
    // absolute* ticks so rounding never accumulates across the file.
    // ------------------------------------------------------------------
    constexpr int64_t kTimescale = 90000;
    const int64_t tbNum = framePts.tbNum;
    const int64_t tbDen = framePts.tbDen;
    const int64_t base  = framePts.pts.front();

    std::vector<int64_t> ticks(n);
    for (size_t i = 0; i < n; ++i)
    {
        const int64_t rel = framePts.pts[i] - base;     // >= 0 (sorted)
        const int64_t q = rel / tbDen;
        const int64_t r = rel % tbDen;
        ticks[i] = q * tbNum * kTimescale +
                   (r * tbNum * kTimescale + tbDen / 2) / tbDen;
    }

    std::vector<uint32_t> deltas(n);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const int64_t d = ticks[i + 1] - ticks[i];
        if (d <= 0 || d > 0xFFFFFFFFLL)
            return fail("non-increasing or absurd source timestamps");
        deltas[i] = static_cast<uint32_t>(d);
    }
    deltas[n - 1] = deltas[n - 2];          // last frame: repeat previous
    const uint64_t durTicks =
        static_cast<uint64_t>(ticks[n - 1]) + deltas[n - 1];

    // ------------------------------------------------------------------
    // 2. Locate moov; it must be the last top-level box (ffmpeg writes it
    //    after mdat unless +faststart) so we can rewrite it by truncating.
    // ------------------------------------------------------------------
    HANDLE h = CreateFileW(toWide(mp4Path).c_str(),
                           GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return fail("cannot open mp4 for read/write");

    LARGE_INTEGER fileSizeLi{};
    GetFileSizeEx(h, &fileSizeLi);
    const uint64_t fileSize = static_cast<uint64_t>(fileSizeLi.QuadPart);

    uint64_t moovOffset = 0, moovSize = 0, scan = 0;
    bool found = false;
    while (scan + 8 <= fileSize)
    {
        uint8_t hdr[16];
        LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(scan);
        DWORD got = 0;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) ||
            !ReadFile(h, hdr, 16, &got, nullptr) || got < 8)
            break;
        uint64_t size = rd32(hdr);
        const uint32_t type = rd32(hdr + 4);
        if (size == 1)
        {
            if (got < 16) break;
            size = rd64(hdr + 8);
        }
        else if (size == 0)
        {
            size = fileSize - scan;
        }
        if (size < 8 || scan + size > fileSize) break;
        if (type == fourcc("moov"))
        {
            moovOffset = scan;
            moovSize   = size;
            found = (scan + size == fileSize);   // must be last
            break;
        }
        scan += size;
    }
    if (!found)
    {
        CloseHandle(h);
        return fail("moov box missing or not at end of file");
    }

    std::vector<uint8_t> moovRaw(static_cast<size_t>(moovSize));
    {
        LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(moovOffset);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        size_t done = 0;
        while (done < moovRaw.size())
        {
            const DWORD want = static_cast<DWORD>(
                std::min<size_t>(moovRaw.size() - done, 1u << 24));
            DWORD got = 0;
            if (!ReadFile(h, moovRaw.data() + done, want, &got, nullptr) || !got)
            {
                CloseHandle(h);
                return fail("short read on moov");
            }
            done += got;
        }
    }

    // moovRaw includes the 8-byte header; parse the body.
    Box moov;
    moov.type = fourcc("moov");
    moov.container = true;
    if (rd32(moovRaw.data()) == 1 ||
        !parseBoxes(moovRaw.data() + 8, moovRaw.size() - 8, moov.children))
    {
        CloseHandle(h);
        return fail("unexpected moov structure");
    }

    // ------------------------------------------------------------------
    // 3. Find the boxes we patch. The temp file is produced by our own
    //    ffmpeg invocation, so be strict: exactly one trak, video handler.
    // ------------------------------------------------------------------
    Box* mvhd = findChild(moov, fourcc("mvhd"));
    Box* trak = nullptr;
    for (Box& c : moov.children)
    {
        if (c.type != fourcc("trak")) continue;
        if (trak) { CloseHandle(h); return fail("more than one trak"); }
        trak = &c;
    }
    Box* mdia = trak ? findChild(*trak, fourcc("mdia")) : nullptr;
    Box* hdlr = mdia ? findChild(*mdia, fourcc("hdlr")) : nullptr;
    Box* mdhd = mdia ? findChild(*mdia, fourcc("mdhd")) : nullptr;
    Box* tkhd = trak ? findChild(*trak, fourcc("tkhd")) : nullptr;
    Box* minf = mdia ? findChild(*mdia, fourcc("minf")) : nullptr;
    Box* stbl = minf ? findChild(*minf, fourcc("stbl")) : nullptr;
    Box* stts = stbl ? findChild(*stbl, fourcc("stts")) : nullptr;
    if (!mvhd || !trak || !mdia || !hdlr || !mdhd || !tkhd || !stbl || !stts ||
        mvhd->payload.size() < 20 || hdlr->payload.size() < 12 ||
        mdhd->payload.size() < 20 || stts->payload.size() < 8)
    {
        CloseHandle(h);
        return fail("required boxes missing");
    }
    if (rd32(hdlr->payload.data() + 8) != fourcc("vide"))
    {
        CloseHandle(h);
        return fail("track is not video");
    }
    if (findChild(*stbl, fourcc("ctts")))
    {
        // IPPP output never has composition offsets; refuse anything else.
        CloseHandle(h);
        return fail("ctts present (unexpected B-frame reordering)");
    }

    // Sample count must match the timestamps we captured.
    {
        const uint32_t entries = rd32(stts->payload.data() + 4);
        if (stts->payload.size() < 8 + size_t(entries) * 8)
        {
            CloseHandle(h);
            return fail("truncated stts");
        }
        uint64_t total = 0;
        for (uint32_t i = 0; i < entries; ++i)
            total += rd32(stts->payload.data() + 8 + i * 8);
        if (total != n)
        {
            fprintf(stderr,
                    "applyMp4FramePts: sample count %llu != %zu source "
                    "timestamps -- keeping CFR timing.\n",
                    static_cast<unsigned long long>(total), n);
            CloseHandle(h);
            return false;
        }
    }

    // ------------------------------------------------------------------
    // 4. Rebuild stts (run-length encoded per-frame deltas) and update the
    //    duration fields that depend on the media timescale.
    // ------------------------------------------------------------------
    {
        std::vector<uint8_t> p(8);
        wr32(p.data(), 0);                       // version 0, flags 0
        uint32_t entryCount = 0;
        size_t i = 0;
        while (i < n)
        {
            size_t j = i;
            while (j < n && deltas[j] == deltas[i]) ++j;
            uint8_t e[8];
            wr32(e,     static_cast<uint32_t>(j - i));
            wr32(e + 4, deltas[i]);
            p.insert(p.end(), e, e + 8);
            ++entryCount;
            i = j;
        }
        wr32(p.data() + 4, entryCount);
        stts->payload = std::move(p);
    }

    // mdhd: media timescale + duration.
    {
        uint8_t* d = mdhd->payload.data();
        if (d[0] == 0)
        {
            if (durTicks > 0xFFFFFFFFULL)
            {
                CloseHandle(h);
                return fail("duration overflows 32-bit mdhd (>13 h)");
            }
            wr32(d + 12, static_cast<uint32_t>(kTimescale));
            wr32(d + 16, static_cast<uint32_t>(durTicks));
        }
        else if (d[0] == 1 && mdhd->payload.size() >= 32)
        {
            wr32(d + 20, static_cast<uint32_t>(kTimescale));
            wr64(d + 24, durTicks);
        }
        else
        {
            CloseHandle(h);
            return fail("unsupported mdhd version");
        }
    }

    // Movie-timescale duration for mvhd / tkhd / elst.
    const uint32_t movieTs = (mvhd->payload[0] == 0)
        ? rd32(mvhd->payload.data() + 12)
        : rd32(mvhd->payload.data() + 20);
    const uint64_t movieDur =
        (durTicks * movieTs + kTimescale / 2) / kTimescale;

    {
        uint8_t* d = mvhd->payload.data();
        if (d[0] == 0)
            wr32(d + 16, static_cast<uint32_t>(movieDur));
        else if (mvhd->payload.size() >= 32)
            wr64(d + 24, movieDur);
    }
    {
        uint8_t* d = tkhd->payload.data();
        if (d[0] == 0 && tkhd->payload.size() >= 24)
            wr32(d + 20, static_cast<uint32_t>(movieDur));
        else if (d[0] == 1 && tkhd->payload.size() >= 36)
            wr64(d + 28, movieDur);
    }
    if (Box* edts = findChild(*trak, fourcc("edts")))
    {
        if (Box* elst = findChild(*edts, fourcc("elst")))
        {
            uint8_t* d = elst->payload.data();
            if (elst->payload.size() >= 8 && rd32(d + 4) == 1)
            {
                if (d[0] == 0 && elst->payload.size() >= 20)
                    wr32(d + 8, static_cast<uint32_t>(movieDur));
                else if (d[0] == 1 && elst->payload.size() >= 28)
                    wr64(d + 8, movieDur);
            }
            else
            {
                CloseHandle(h);
                return fail("multi-entry edit list");
            }
        }
    }

    // ------------------------------------------------------------------
    // 5. Serialize and swap the moov in place (truncate + append).
    // ------------------------------------------------------------------
    std::vector<uint8_t> newMoov;
    newMoov.reserve(serializedSize(moov));
    serialize(moov, newMoov);

    LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(moovOffset);
    bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
    size_t done = 0;
    while (ok && done < newMoov.size())
    {
        const DWORD want = static_cast<DWORD>(
            std::min<size_t>(newMoov.size() - done, 1u << 24));
        DWORD put = 0;
        ok = WriteFile(h, newMoov.data() + done, want, &put, nullptr) && put;
        done += put;
    }
    ok = ok && SetEndOfFile(h);
    CloseHandle(h);
    if (!ok)
        return fail("write-back failed (file may need re-mux)");

    if (verbose)
        fprintf(stderr,
                "applyMp4FramePts: %zu samples re-stamped, duration %.3f s, "
                "stts %zu -> %u bytes.\n",
                n, double(durTicks) / double(kTimescale),
                size_t(moovSize), rd32(newMoov.data()));
    return true;
}
