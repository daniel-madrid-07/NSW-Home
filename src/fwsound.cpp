// HOME Menu sound effects from the installed firmware. See fwsound.h.
//
// Path through the data (verified on firmware 19.0.0):
//   registered\<id>.nca\00      NCA; the one with program ID 0100000000001000 and content type Program
//   -> RomFS section            sound\qlaunch.bfsar (found by name, wherever it sits)
//   -> BFSAR ("FSAR")           STRG names, INFO tables, FILE data
//      sound info               153 wave sounds (+ 7 empty sequence stubs, ignored)
//      -> BFWSD ("FWSD")        wave sound -> (wave archive, wave index)
//      -> BFWAR ("FWAR")        table of embedded BFWAV
//      -> BFWAV ("FWAV")        PCM16 or DSP-ADPCM, mono or stereo
// The BFWSD/BFWAR files are not stored directly in the archive's FILE block but inside
// group files (BFGRP, "FGRP") that are.
//
// All of it is untrusted input: every offset goes through Span, which fails on anything out of range.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "fwsound.h"
#include "nsw_core.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

namespace fwsound {
namespace {

using namespace nsw::core;

const uint64_t kMenuTitle = 0x0100000000001000ull;     // qlaunch
const uint64_t kVersionTitle = 0x0100000000000809ull;  // SystemVersion

// The firmware lookup (Content, findContent, contentRomfs) lives in nsw.cpp, shared with fwfont.cpp.

// ---------------------------------------------------------------- bounds-checked reader

struct Span {
    const uint8_t* p = nullptr;
    size_t n = 0;

    void need(uint64_t off, uint64_t len) const {
        if (off > n || len > n - off) fail("BAD_ARCHIVE");
    }
    uint8_t u8(uint64_t off) const {
        need(off, 1);
        return p[off];
    }
    uint16_t u16(uint64_t off) const {
        need(off, 2);
        return rd16(p + off);
    }
    uint32_t u32(uint64_t off) const {
        need(off, 4);
        return rd32(p + off);
    }
    int16_t s16(uint64_t off) const { return static_cast<int16_t>(u16(off)); }
    float f32(uint64_t off) const {
        uint32_t bits = u32(off);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    Span sub(uint64_t off, uint64_t len) const {
        need(off, len);
        return Span{p + off, static_cast<size_t>(len)};
    }
    Span from(uint64_t off) const {
        need(off, 0);
        return Span{p + off, n - static_cast<size_t>(off)};
    }
    bool empty() const { return p == nullptr; }
};

// A reference: 16-bit type ID, padding, signed 32-bit offset relative to some base that
// depends on where the reference lives. Offset -1 means "nothing". The type is not reliable
// enough to test for that: valid references with type 0 exist (the group files in the file table).
struct Ref {
    uint16_t id = 0;
    int64_t off = -1;
    bool null() const { return off < 0; }
};

Ref readRef(const Span& s, uint64_t at) {
    Ref r;
    r.id = s.u16(at);
    r.off = static_cast<int32_t>(s.u32(at + 4));
    return r;
}

Span follow(const Span& base, const Ref& r) {
    if (r.null()) fail("BAD_ARCHIVE");
    return base.from(static_cast<uint64_t>(r.off));
}

// "Reference table": u32 count, then that many references relative to the table itself.
std::vector<Ref> refTable(const Span& table) {
    uint32_t count = table.u32(0);
    table.need(4, static_cast<uint64_t>(count) * 8);
    std::vector<Ref> refs(count);
    for (uint32_t i = 0; i < count; ++i) refs[i] = readRef(table, 4 + static_cast<uint64_t>(i) * 8);
    return refs;
}

// Common header of every NintendoWare sound file: magic, byte-order mark, header size, version,
// file size, block count, then {u16 id, u16 pad, u32 offset, u32 size} per block.
// Returns the body of each block (past its own 8-byte magic + size header).
std::map<uint16_t, Span> openBlocks(const Span& file, const char* magic) {
    file.need(0, 0x14);
    if (std::memcmp(file.p, magic, 4) != 0) fail("BAD_ARCHIVE");
    if (file.u16(4) != 0xfeff) fail("BAD_ARCHIVE");  // little-endian only (Switch)
    unsigned count = file.u16(0x10);
    if (count > 16) fail("BAD_ARCHIVE");
    std::map<uint16_t, Span> blocks;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t e = 0x14 + static_cast<uint64_t>(i) * 12;
        uint64_t offset = file.u32(e + 4);
        uint64_t size = file.u32(e + 8);
        // A block may claim alignment padding past the end of its file (the INFX block of an
        // embedded group does), so the size is clamped; a block that does not fit at all is dropped.
        if (offset > file.n || file.n - offset < 8) continue;
        size = std::min<uint64_t>(size, file.n - offset);
        if (size < 8) continue;
        blocks[file.u16(e)] = file.sub(offset, size).from(8);
    }
    return blocks;
}

const Span& block(const std::map<uint16_t, Span>& blocks, uint16_t id) {
    auto it = blocks.find(id);
    if (it == blocks.end()) fail("BAD_ARCHIVE");
    return it->second;
}

// ---------------------------------------------------------------- BFWAV -> PCM

struct Pcm {
    uint32_t rate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> samples;  // interleaved
};

const uint32_t kMaxFrames = 48000u * 120;

int16_t clamp16(int32_t v) { return static_cast<int16_t>(std::min(32767, std::max(-32768, v))); }

Pcm decodeWave(const Span& file) {
    auto blocks = openBlocks(file, "FWAV");
    const Span& info = block(blocks, 0x7000);
    const Span& data = block(blocks, 0x7001);

    uint8_t encoding = info.u8(0);  // 0 PCM8, 1 PCM16, 2 DSP-ADPCM
    Pcm pcm;
    pcm.rate = info.u32(4);
    uint32_t frames = info.u32(0x0c);  // total frames; for a looped wave this is the loop end
    Span channelTable = info.from(0x14);
    std::vector<Ref> channels = refTable(channelTable);
    if (channels.empty() || channels.size() > 2 || frames == 0 || frames > kMaxFrames) fail("BAD_ARCHIVE");
    if (pcm.rate < 1000 || pcm.rate > 192000) fail("BAD_ARCHIVE");
    pcm.channels = static_cast<uint16_t>(channels.size());
    pcm.samples.resize(static_cast<size_t>(frames) * pcm.channels);

    for (size_t ch = 0; ch < channels.size(); ++ch) {
        Span chInfo = follow(channelTable, channels[ch]);
        Span src = follow(data, readRef(chInfo, 0));
        int16_t* dst = pcm.samples.data() + ch;
        size_t step = pcm.channels;

        if (encoding == 1) {
            src.need(0, static_cast<uint64_t>(frames) * 2);
            for (uint32_t i = 0; i < frames; ++i) dst[i * step] = static_cast<int16_t>(rd16(src.p + i * 2));
        } else if (encoding == 0) {
            src.need(0, frames);
            for (uint32_t i = 0; i < frames; ++i) dst[i * step] = static_cast<int16_t>(static_cast<int8_t>(src.p[i]) * 256);
        } else if (encoding == 2) {
            // DSP-ADPCM: 8-byte frames = 1 header byte (predictor << 4 | scale) + 14 nibbles.
            Span adpcm = follow(chInfo, readRef(chInfo, 8));
            int32_t coef[16];
            for (int i = 0; i < 16; ++i) coef[i] = adpcm.s16(static_cast<uint64_t>(i) * 2);
            int32_t h1 = adpcm.s16(0x22), h2 = adpcm.s16(0x24);

            uint32_t whole = frames / 14, rest = frames % 14;
            src.need(0, static_cast<uint64_t>(whole) * 8 + (rest ? 1 + (rest + 1) / 2 : 0));
            const uint8_t* in = src.p;
            uint32_t done = 0;
            while (done < frames) {
                uint8_t header = *in++;
                int scale = header & 0x0f;
                int predictor = (header >> 4) & 7;
                int32_t c1 = coef[predictor * 2], c2 = coef[predictor * 2 + 1];
                uint32_t count = std::min<uint32_t>(14, frames - done);
                for (uint32_t i = 0; i < count; ++i) {
                    int nibble = (i & 1) ? (in[i / 2] & 0x0f) : (in[i / 2] >> 4);
                    if (nibble >= 8) nibble -= 16;
                    // 64-bit: a hostile scale/coefficient pair must not overflow
                    int64_t v = (static_cast<int64_t>(nibble) * (int64_t{1} << scale)) * 2048 + 1024 +
                                static_cast<int64_t>(c1) * h1 + static_cast<int64_t>(c2) * h2;
                    v >>= 11;
                    int16_t s = static_cast<int16_t>(std::min<int64_t>(32767, std::max<int64_t>(-32768, v)));
                    h2 = h1;
                    h1 = s;
                    dst[(done + i) * step] = s;
                }
                in += 7;
                done += count;
            }
        } else {
            fail("BAD_ARCHIVE");
        }
    }
    return pcm;
}

void put16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = static_cast<uint8_t>(v & 0xff);
    b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    put16(b, at, v & 0xffff);
    put16(b, at + 2, v >> 16);
}

// gain is a linear amplitude factor; the result saturates.
std::vector<uint8_t> makeWav(const Pcm& pcm, uint32_t rate, float gain) {
    uint32_t dataSize = static_cast<uint32_t>(pcm.samples.size() * 2);
    std::vector<uint8_t> w(44 + dataSize);
    std::memcpy(&w[0], "RIFF", 4);
    put32(w, 4, 36 + dataSize);
    std::memcpy(&w[8], "WAVEfmt ", 8);
    put32(w, 16, 16);
    put16(w, 20, 1);  // PCM
    put16(w, 22, pcm.channels);
    put32(w, 24, rate);
    put32(w, 28, rate * pcm.channels * 2);
    put16(w, 32, static_cast<uint32_t>(pcm.channels) * 2);
    put16(w, 34, 16);
    std::memcpy(&w[36], "data", 4);
    put32(w, 40, dataSize);
    for (size_t i = 0; i < pcm.samples.size(); ++i) {
        int16_t s = pcm.samples[i];
        if (gain != 1.0f) {
            float v = static_cast<float>(s) * gain;
            s = clamp16(static_cast<int32_t>(v < 0 ? v - 0.5f : v + 0.5f));
        }
        put16(w, 44 + i * 2, static_cast<uint16_t>(s));
    }
    return w;
}

// ---------------------------------------------------------------- BFSAR

struct WaveSoundFile {
    Span waveIds;           // u32 count, then {u32 wave archive item ID, u32 wave index}
    Span dataTable;         // reference table of wave sound data
    std::vector<Ref> data;
};

struct WaveArchive {
    Span info;  // u32 count, then {Ref, u32 size} relative to `files`
    Span files;
    uint32_t count = 0;
};

struct Archive {
    std::vector<std::string> names;
    std::vector<Span> files;         // by file ID; empty() if not stored in this archive
    std::vector<uint32_t> waveArchiveFile;  // wave archive index -> file ID
    Span soundTable;
    std::vector<Ref> sounds;

    std::map<uint32_t, WaveSoundFile> wsdCache;
    std::map<uint32_t, WaveArchive> warCache;
    std::map<uint64_t, std::shared_ptr<Pcm>> pcmCache;

    const Span& file(uint32_t id) const {
        if (id >= files.size() || files[id].empty()) fail("BAD_ARCHIVE");
        return files[id];
    }
};

void openArchive(const Span& sar, Archive& a) {
    auto blocks = openBlocks(sar, "FSAR");
    const Span& strg = block(blocks, 0x2000);
    const Span& info = block(blocks, 0x2001);
    const Span& fileData = block(blocks, 0x2002);

    // STRG: reference to the string table, then {Ref, u32 size incl. terminator} per name.
    Span stringTable = follow(strg, readRef(strg, 0));
    uint32_t stringCount = stringTable.u32(0);
    stringTable.need(4, static_cast<uint64_t>(stringCount) * 12);
    a.names.resize(stringCount);
    for (uint32_t i = 0; i < stringCount; ++i) {
        uint64_t e = 4 + static_cast<uint64_t>(i) * 12;
        Ref r = readRef(stringTable, e);
        uint32_t size = stringTable.u32(e + 8);
        if (r.null() || size == 0 || size > 0x200) continue;
        Span s = stringTable.sub(static_cast<uint64_t>(r.off), size);
        size_t len = 0;
        while (len < s.n && s.p[len] != 0) ++len;
        a.names[i].assign(reinterpret_cast<const char*>(s.p), len);
    }

    // INFO: eight references to the tables, identified by type.
    Span fileTable, groupTable, waveArchiveTable;
    for (int i = 0; i < 8; ++i) {
        Ref r = readRef(info, static_cast<uint64_t>(i) * 8);
        if (r.null()) continue;
        Span t = follow(info, r);
        switch (r.id) {
            case 0x2100: a.soundTable = t; break;
            case 0x2103: waveArchiveTable = t; break;
            case 0x2105: groupTable = t; break;
            case 0x2106: fileTable = t; break;
            default: break;
        }
    }
    if (a.soundTable.empty() || fileTable.empty() || waveArchiveTable.empty()) fail("BAD_ARCHIVE");
    a.sounds = refTable(a.soundTable);

    // Files stored directly in the FILE block.
    std::vector<Ref> fileRefs = refTable(fileTable);
    a.files.resize(fileRefs.size());
    for (size_t i = 0; i < fileRefs.size(); ++i) {
        if (fileRefs[i].null()) continue;
        Span fi = follow(fileTable, fileRefs[i]);
        Ref location = readRef(fi, 0);
        if (location.id != 0x220c || location.null()) continue;  // 0x220d = external file
        Span internal = follow(fi, location);
        Ref where = readRef(internal, 0);
        if (where.null()) continue;  // lives inside a group only
        a.files[i] = fileData.sub(static_cast<uint64_t>(where.off), internal.u32(8));
    }

    // Files embedded in group files.
    if (!groupTable.empty()) {
        for (const Ref& g : refTable(groupTable)) {
            if (g.null()) continue;
            uint32_t groupFile = follow(groupTable, g).u32(0);
            if (groupFile >= a.files.size() || a.files[groupFile].empty()) continue;
            auto gb = openBlocks(a.files[groupFile], "FGRP");
            const Span& gInfo = block(gb, 0x7800);
            const Span& gFiles = block(gb, 0x7801);
            for (const Ref& item : refTable(gInfo)) {
                if (item.null()) continue;
                Span it = follow(gInfo, item);
                uint32_t id = it.u32(0);
                Ref where = readRef(it, 4);
                if (where.null() || id >= a.files.size() || !a.files[id].empty()) continue;
                a.files[id] = gFiles.sub(static_cast<uint64_t>(where.off), it.u32(0x0c));
            }
        }
    }

    for (const Ref& w : refTable(waveArchiveTable))
        a.waveArchiveFile.push_back(w.null() ? UINT32_MAX : follow(waveArchiveTable, w).u32(0));
}

const WaveSoundFile& waveSoundFile(Archive& a, uint32_t fileId) {
    auto it = a.wsdCache.find(fileId);
    if (it != a.wsdCache.end()) return it->second;
    auto blocks = openBlocks(a.file(fileId), "FWSD");
    const Span& info = block(blocks, 0x6800);
    WaveSoundFile w;
    w.waveIds = follow(info, readRef(info, 0));
    w.dataTable = follow(info, readRef(info, 8));
    w.data = refTable(w.dataTable);
    return a.wsdCache.emplace(fileId, std::move(w)).first->second;
}

const WaveArchive& waveArchive(Archive& a, uint32_t index) {
    auto it = a.warCache.find(index);
    if (it != a.warCache.end()) return it->second;
    if (index >= a.waveArchiveFile.size()) fail("BAD_ARCHIVE");
    auto blocks = openBlocks(a.file(a.waveArchiveFile[index]), "FWAR");
    WaveArchive w;
    w.info = block(blocks, 0x6800);
    w.files = block(blocks, 0x6801);
    w.count = w.info.u32(0);
    w.info.need(4, static_cast<uint64_t>(w.count) * 12);
    return a.warCache.emplace(index, w).first->second;
}

std::shared_ptr<Pcm> wavePcm(Archive& a, uint32_t archiveIndex, uint32_t waveIndex) {
    uint64_t key = (static_cast<uint64_t>(archiveIndex) << 32) | waveIndex;
    auto it = a.pcmCache.find(key);
    if (it != a.pcmCache.end()) return it->second;
    const WaveArchive& war = waveArchive(a, archiveIndex);
    if (waveIndex >= war.count) fail("BAD_ARCHIVE");
    uint64_t e = 4 + static_cast<uint64_t>(waveIndex) * 12;
    Ref where = readRef(war.info, e);
    if (where.null()) fail("BAD_ARCHIVE");
    auto pcm = std::make_shared<Pcm>(decodeWave(war.files.sub(static_cast<uint64_t>(where.off), war.info.u32(e + 8))));
    a.pcmCache[key] = pcm;
    return pcm;
}

// One entry of the sound table. Returns false for anything that is not a wave sound.
bool loadSound(Archive& a, size_t index, Sound& out) {
    Span si = follow(a.soundTable, a.sounds[index]);
    uint32_t fileId = si.u32(0);
    uint8_t volume = si.u8(8);  // 127 = unity; the menu's mix lives here
    Ref detail = readRef(si, 0x0c);
    uint32_t flags = si.u32(0x14);
    if (detail.id != 0x2202) return false;  // 0x2201 stream, 0x2203 sequence
    uint32_t wsdIndex = follow(si, detail).u32(0);

    const WaveSoundFile& wsd = waveSoundFile(a, fileId);
    if (wsdIndex >= wsd.data.size()) fail("BAD_ARCHIVE");
    Span item = follow(wsd.dataTable, wsd.data[wsdIndex]);
    Span wsInfo = follow(item, readRef(item, 0));
    Span noteTable = follow(item, readRef(item, 0x10));
    std::vector<Ref> notes = refTable(noteTable);
    if (notes.empty()) fail("BAD_ARCHIVE");
    uint32_t waveIdIndex = follow(noteTable, notes[0]).u32(0);
    if (waveIdIndex >= wsd.waveIds.u32(0)) fail("BAD_ARCHIVE");
    uint32_t archiveItem = wsd.waveIds.u32(4 + static_cast<uint64_t>(waveIdIndex) * 8);
    uint32_t waveIndex = wsd.waveIds.u32(8 + static_cast<uint64_t>(waveIdIndex) * 8);
    if ((archiveItem >> 24) != 5) fail("BAD_ARCHIVE");  // item type 5 = wave archive

    // optional wave sound parameters, in flag order: bit 0 pan (u32), bit 1 pitch (float)
    uint32_t wsFlags = wsInfo.u32(0);
    float pitch = (wsFlags & 2) ? wsInfo.f32(4 + ((wsFlags & 1) ? 4 : 0)) : 1.0f;
    if (!(pitch >= 0.25f && pitch <= 4.0f)) pitch = 1.0f;

    std::shared_ptr<Pcm> pcm = wavePcm(a, archiveItem & 0xffffff, waveIndex);
    if (std::all_of(pcm->samples.begin(), pcm->samples.end(), [](int16_t s) { return s == 0; })) return false;

    // A pitch other than 1 is expressed through the sample rate: exact, no resampling.
    uint32_t rate = pitch == 1.0f ? pcm->rate : static_cast<uint32_t>(static_cast<float>(pcm->rate) * pitch + 0.5f);
    // Volume 0 marks sounds the menu levels at run time; those keep the level of the wave.
    float gain = volume == 0 ? 1.0f : static_cast<float>(volume) / 127.0f;
    out.wav = makeWav(*pcm, rate, gain);

    uint32_t stringId = (flags & 1) ? si.u32(0x18) : UINT32_MAX;
    if (stringId < a.names.size() && !a.names[stringId].empty()) {
        out.name = a.names[stringId];
    } else {
        out.name = "Sound_" + std::to_string(index);
    }
    return true;
}

// ---------------------------------------------------------------- top level

void loadMenuSoundsImpl(const std::wstring& registeredDir, const std::wstring& keysDir, std::vector<Sound>& out) {
    Content menu;
    findContent(registeredDir, keysDir, kMenuTitle, kContentProgram, "NO_MENU_NCA", menu);
    View romfs = contentRomfs(menu);

    const RomFile* archiveFile = nullptr;
    std::map<std::string, RomFile> files = listRomfs(romfs);
    for (const auto& f : files) {
        const std::string& n = f.first;
        if (n.size() < 6 || n.compare(n.size() - 6, 6, ".bfsar") != 0) continue;
        if (!archiveFile || n == "qlaunch.bfsar") archiveFile = &f.second;
    }
    if (!archiveFile) fail("NO_ARCHIVE");
    if (archiveFile->size < 0x40 || archiveFile->size > 0x8000000) fail("BAD_ARCHIVE");
    Bytes raw = romfs.read(archiveFile->offset, static_cast<size_t>(archiveFile->size));

    Archive archive;
    openArchive(Span{raw.data(), raw.size()}, archive);
    for (size_t i = 0; i < archive.sounds.size(); ++i) {
        if (archive.sounds[i].null()) continue;
        try {
            Sound s;
            if (loadSound(archive, i, s)) out.push_back(std::move(s));
        } catch (const Fail&) {
            // one broken entry does not take the rest down
        }
    }
    if (out.empty()) fail("NO_SOUNDS");
}

void firmwareVersionImpl(const std::wstring& registeredDir, const std::wstring& keysDir, std::string& version) {
    Content c;
    findContent(registeredDir, keysDir, kVersionTitle, kContentData, "NO_VERSION", c);
    View romfs = contentRomfs(c);
    std::map<std::string, RomFile> files = listRomfs(romfs);
    auto it = files.find("file");
    if (it == files.end() || it->second.size < 0x80) fail("NO_VERSION");
    Bytes b = romfs.read(it->second.offset + 0x68, 0x18);  // display version, NUL-terminated
    size_t len = 0;
    while (len < b.size() && b[len] >= 0x20 && b[len] < 0x7f) ++len;
    if (len == 0) fail("NO_VERSION");
    version.assign(reinterpret_cast<const char*>(b.data()), len);
}

template <typename Fn>
bool guarded(std::string& err, Fn&& fn) {
    try {
        err.clear();
        fn();
        return true;
    } catch (const Fail& f) {
        try {
            err = f.code;
        } catch (...) {
        }
    } catch (...) {
        try {
            err = "INTERNAL";
        } catch (...) {
        }
    }
    return false;
}

}  // namespace

bool LoadMenuSounds(const std::wstring& registeredDir, const std::wstring& keysDir, std::vector<Sound>& out,
                    std::string& err) {
    return guarded(err, [&] {
        std::vector<Sound> sounds;
        loadMenuSoundsImpl(registeredDir, keysDir, sounds);
        out = std::move(sounds);
    });
}

bool FirmwareVersion(const std::wstring& registeredDir, const std::wstring& keysDir, std::string& version,
                     std::string& err) {
    return guarded(err, [&] {
        std::string v;
        firmwareVersionImpl(registeredDir, keysDir, v);
        version = std::move(v);
    });
}

}  // namespace fwsound
