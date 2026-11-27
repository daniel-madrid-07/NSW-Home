// See emudata.h.
#include "emudata.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace emudata {

namespace fs = std::filesystem;

namespace {

// The rest of the program writes Windows paths on every platform.
fs::path P(const std::wstring& s) {
#ifdef _WIN32
    return fs::path(s);
#else
    std::wstring t = s;
    std::replace(t.begin(), t.end(), L'\\', L'/');
    return fs::path(t);
#endif
}

std::wstring W(const fs::path& p) {
    std::wstring s = p.wstring();
#ifndef _WIN32
    std::replace(s.begin(), s.end(), L'/', L'\\');
#endif
    return s;
}

bool IsTitleId(const std::wstring& s) {
    if (s.size() != 16) return false;
    for (wchar_t c : s)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
    return true;
}

std::string LowerAscii(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out += static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c);
    return out;
}

// A real directory: not a link, not a junction.
bool PlainDir(const fs::path& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() == fs::file_type::directory && !ec;
}

bool PlainFile(const fs::path& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() == fs::file_type::regular && !ec;
}

uint64_t SizeOfTree(const fs::path& dir, int* files = nullptr) {
    uint64_t total = 0;
    std::error_code ec;
    if (!PlainDir(dir)) return 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (PlainDir(p)) {
            total += SizeOfTree(p, files);
        } else if (PlainFile(p)) {
            std::error_code se;
            const uintmax_t n = fs::file_size(p, se);
            if (!se) total += n;
            if (files) ++*files;
        }
    }
    return total;
}

// The components of `dir` below `root`, or empty when it is not inside it.
// Both are resolved first, so "..", a different spelling or a link on the way
// cannot make an outside path look like an inside one.
std::vector<std::wstring> Below(const fs::path& root, const fs::path& dir) {
    std::error_code ec;
    const fs::path r = fs::canonical(root, ec);
    if (ec) return {};
    const fs::path d = fs::canonical(dir, ec);
    if (ec) return {};
    auto ri = r.begin(), di = d.begin();
    for (; ri != r.end(); ++ri, ++di)
        if (di == d.end() || LowerAscii(ri->wstring()) != LowerAscii(di->wstring())) return {};
    std::vector<std::wstring> rest;
    for (; di != d.end(); ++di) rest.push_back(di->wstring());
    return rest;
}

bool SameName(const std::wstring& a, const wchar_t* b) {
    return LowerAscii(a) == LowerAscii(b);
}

// Removes a directory tree without ever following a link: anything that is
// not a plain directory is removed as the single entry it is.
bool RemoveTree(const fs::path& dir, uint64_t& bytes) {
    std::error_code ec;
    bool ok = true;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    if (ec) return false;
    for (const fs::path& p : entries) {
        if (PlainDir(p)) {
            ok = RemoveTree(p, bytes) && ok;
            continue;
        }
        std::error_code se;
        const uintmax_t n = PlainFile(p) ? fs::file_size(p, se) : 0;
        // Read-only files are still the emulator's cache: clear the flag and try once more.
        if (!fs::remove(p, ec) || ec) {
            std::error_code pe;
            fs::permissions(p, fs::perms::owner_write, fs::perm_options::add, pe);
            ec.clear();
            if (!fs::remove(p, ec) || ec) {
                ok = false;
                continue;
            }
        }
        if (!se) bytes += n;
    }
    if (!ok) return false;
    fs::remove(dir, ec);
    return !ec;
}

// Copies a tree of plain files and directories; links are left out.
bool CopyTree(const fs::path& from, const fs::path& to, Outcome& out) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return false;
    bool ok = true;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path(), q = to / p.filename();
        if (PlainDir(p)) {
            ok = CopyTree(p, q, out) && ok;
        } else if (PlainFile(p)) {
            std::error_code ce, se;
            if (!fs::copy_file(p, q, fs::copy_options::none, ce) || ce) {
                ok = false;
                continue;
            }
            const uintmax_t a = fs::file_size(p, se), b = fs::file_size(q, se);
            if (se || a != b) {
                ok = false;
                continue;
            }
            out.bytes += b;
            out.items++;
        }
    }
    return ok && !ec;
}

const wchar_t* const kRyuSaveParts[] = {L"bis\\user\\save", L"bis\\user\\saveMeta", L"bis\\system\\save\\8000000000000000"};
const wchar_t* const kYuzuSaveParts[] = {L"nand\\user\\save"};

bool HexBytes(const std::string& hex, unsigned char out[16]) {
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            const char c = hex[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        out[i] = static_cast<unsigned char>(v);
    }
    return true;
}

uint64_t Le64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

std::wstring Hex16(uint64_t v) {
    wchar_t b[20];
    swprintf(b, 20, L"%016llx", static_cast<unsigned long long>(v));
    return b;
}

// Ryujinx's save index (the console's own format, "imkvdb.arc"): "IMKV", a
// reserved word and the number of entries; then per entry "IMEN", the sizes
// of key and value (0x40 each) and the two. The key holds the program id
// (0x00), the user's id as two 64-bit halves, each low byte first (0x08) and
// the kind of save (0x20; 1 = a user's save); the value starts with the
// number of the folder under bis\user\save and says where it lives (0x18;
// 1 = the user area). Checked against the owner's index, read only: his
// user "0000000000000001 0000000000000000" is stored as 01 00 .. 00.
// The emulator commits to folder "1" and works in "0".
struct IndexEntry {
    uint64_t program = 0, save = 0;
    unsigned char user[16] = {};
    int type = 0, space = 0;
};

bool ReadRyuIndex(const fs::path& root, std::vector<IndexEntry>& out) {
    const fs::path base = root / P(kRyuSaveParts[2]);
    for (const wchar_t* sub : {L"1", L"0"}) {
        std::ifstream f(base / sub / L"imkvdb.arc", std::ios::binary);
        if (!f) continue;
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const unsigned char* b = reinterpret_cast<const unsigned char*>(data.data());
        if (data.size() < 12 || data.compare(0, 4, "IMKV") != 0) continue;
        const uint32_t count = static_cast<uint32_t>(Le64(b + 8) & 0xFFFFFFFFu);
        size_t at = 12;
        std::vector<IndexEntry> list;
        bool good = true;
        for (uint32_t i = 0; i < count && good; i++) {
            if (at + 12 > data.size() || data.compare(at, 4, "IMEN") != 0) {
                good = false;
                break;
            }
            const uint32_t ks = static_cast<uint32_t>(b[at + 4] | (b[at + 5] << 8) | (b[at + 6] << 16)) | (static_cast<uint32_t>(b[at + 7]) << 24);
            const uint32_t vs = static_cast<uint32_t>(b[at + 8] | (b[at + 9] << 8) | (b[at + 10] << 16)) | (static_cast<uint32_t>(b[at + 11]) << 24);
            at += 12;
            if (ks != 0x40 || vs != 0x40 || at + ks + vs > data.size()) {
                good = false;
                break;
            }
            IndexEntry e;
            e.program = Le64(b + at);
            std::copy(b + at + 8, b + at + 24, e.user);
            e.type = b[at + 0x20];
            e.save = Le64(b + at + ks);
            e.space = b[at + ks + 0x18];
            list.push_back(e);
            at += ks + vs;
        }
        if (!good) continue;
        out = std::move(list);
        return true;
    }
    return false;
}

} // namespace

uint64_t TreeSize(const std::wstring& dir) {
    return SizeOfTree(P(dir));
}

bool IsCacheDir(const Root& r, const std::wstring& dir) {
    if (r.dir.empty() || dir.empty()) return false;
    const fs::path d = P(dir);
    if (!PlainDir(d)) return false;
    const std::vector<std::wstring> rest = Below(P(r.dir), d);
    if (r.family == 'r')
        return rest.size() == 4 && SameName(rest[0], L"games") && IsTitleId(rest[1]) && SameName(rest[2], L"cache") &&
               (SameName(rest[3], L"cpu") || SameName(rest[3], L"shader"));
    if (r.family == 'y') return rest.size() == 2 && SameName(rest[0], L"shader") && IsTitleId(rest[1]);
    return false;
}

std::vector<Cache> ListCaches(const Root& r) {
    std::vector<Cache> out;
    if (r.dir.empty()) return out;
    const fs::path root = P(r.dir);
    std::error_code ec;
    auto add = [&](const fs::path& dir, const std::wstring& title) {
        const std::wstring w = W(dir);
        if (!IsCacheDir(r, w)) return;
        out.push_back({w, LowerAscii(title), SizeOfTree(dir)});
    };
    if (r.family == 'r') {
        for (fs::directory_iterator it(root / L"games", ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (!IsTitleId(name) || !PlainDir(it->path())) continue;
            add(it->path() / L"cache" / L"cpu", name);
            add(it->path() / L"cache" / L"shader", name);
        }
    } else if (r.family == 'y') {
        for (fs::directory_iterator it(root / L"shader", ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (IsTitleId(name)) add(it->path(), name);
        }
    }
    std::sort(out.begin(), out.end(), [](const Cache& a, const Cache& b) { return a.dir < b.dir; });
    return out;
}

Outcome RemoveCaches(const Root& r, const std::vector<std::wstring>& dirs) {
    Outcome out;
    for (const std::wstring& d : dirs)
        if (!IsCacheDir(r, d)) {
            out.error = "REFUSED";
            out.where = d;
            return out;
        }
    bool ok = true;
    for (const std::wstring& d : dirs) {
        if (RemoveTree(P(d), out.bytes)) out.items++;
        else ok = false;
    }
    out.ok = ok;
    if (!ok) out.error = "IO";
    return out;
}

std::vector<std::wstring> SaveParts(const Root& r) {
    std::vector<std::wstring> out;
    if (r.dir.empty()) return out;
    auto take = [&](const wchar_t* part) {
        if (PlainDir(P(r.dir) / P(part))) out.push_back(part);
    };
    if (r.family == 'r')
        for (const wchar_t* p : kRyuSaveParts) take(p);
    else if (r.family == 'y')
        for (const wchar_t* p : kYuzuSaveParts) take(p);
    return out;
}

uint64_t SavesSize(const Root& r) {
    uint64_t total = 0;
    for (const std::wstring& part : SaveParts(r)) total += SizeOfTree(P(r.dir) / P(part));
    return total;
}

bool IsDir(const std::wstring& dir) {
    return !dir.empty() && PlainDir(P(dir));
}

bool MakeDir(const std::wstring& dir) {
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    return PlainDir(P(dir));
}

std::vector<std::pair<std::wstring, uint64_t>> ListFiles(const std::wstring& dir) {
    std::vector<std::pair<std::wstring, uint64_t>> out;
    std::error_code ec;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!PlainFile(it->path())) continue;
        std::error_code se;
        const uintmax_t n = fs::file_size(it->path(), se);
        out.push_back({it->path().filename().wstring(), se ? 0 : static_cast<uint64_t>(n)});
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool FreeSpace(const std::wstring& dir, uint64_t& bytes) {
    std::error_code ec;
    const fs::space_info si = fs::space(P(dir), ec);
    if (ec) return false;
    bytes = si.available;
    return true;
}

namespace {
// The checks every copy-out shares: somewhere new, on a disk with room.
bool ReadyDest(const fs::path& dest, uint64_t need, Outcome& out) {
    std::error_code ec;
    if (dest.empty() || !dest.has_parent_path() || !PlainDir(dest.parent_path())) {
        out.error = "MISSING";
        return false;
    }
    if (fs::exists(fs::symlink_status(dest, ec))) {
        out.error = "EXISTS";
        return false;
    }
    uint64_t free = 0;
    if (FreeSpace(W(dest.parent_path()), free) && free < need + (16u << 20)) {
        out.error = "NO_SPACE";
        return false;
    }
    return true;
}

bool InsideOrSame(const fs::path& root, const fs::path& dir) {
    std::error_code ec;
    const bool same = fs::equivalent(root, dir, ec);
    return (same && !ec) || !Below(root, dir).empty();
}
} // namespace

Outcome BackupSaves(const Root& r, const std::wstring& destW) {
    Outcome out;
    const fs::path dest = P(destW);
    const std::vector<std::wstring> parts = SaveParts(r);
    if (parts.empty()) {
        out.error = "MISSING";
        return out;
    }
    if (!ReadyDest(dest, SavesSize(r), out)) return out;
    // A copy inside the emulator's own data would be picked up by the next copy.
    if (InsideOrSame(P(r.dir), dest.parent_path())) {
        out.error = "REFUSED";
        return out;
    }
    out.where = destW;
    bool ok = true;
    for (const std::wstring& part : parts) ok = CopyTree(P(r.dir) / P(part), dest / P(part), out) && ok;
    out.ok = ok;
    if (!ok) out.error = "IO";
    return out;
}

std::wstring UserSaveFolder(const Root& r, const std::string& userId) {
    unsigned char id[16];
    if (r.family != 'y' || r.dir.empty() || !HexBytes(userId, id)) return {};
    // The folder is the id read as one 128-bit number: its bytes back to front
    // (Eden, src/core/file_sys/savedata_factory.cpp; the owner's Citron folder
    // CCD40B9A... is his user b92807fc...d4cc).
    wchar_t name[33];
    for (int i = 0; i < 16; i++) swprintf(name + i * 2, 3, L"%02X", id[15 - i]);
    return W(P(r.dir) / P(kYuzuSaveParts[0]) / L"0000000000000000" / name);
}

std::vector<std::pair<std::string, std::wstring>> UserSaves(const Root& r, const std::string& userId, std::string& error) {
    std::vector<std::pair<std::string, std::wstring>> out;
    error.clear();
    unsigned char id[16];
    if (r.dir.empty() || !HexBytes(userId, id)) {
        error = "MISSING";
        return out;
    }
    if (r.family == 'y') {
        const fs::path dir = P(UserSaveFolder(r, userId));
        std::error_code ec;
        if (!PlainDir(dir)) return out;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (IsTitleId(name) && PlainDir(it->path())) out.push_back({LowerAscii(name), W(it->path())});
        }
    } else {
        std::vector<IndexEntry> index;
        if (!ReadRyuIndex(P(r.dir), index)) {
            error = "NO_INDEX";
            return out;
        }
        // NSW-Home writes the id as the emulator prints it: the high half then
        // the low half, most significant digit first.
        unsigned char stored[16];
        for (int i = 0; i < 8; i++) {
            stored[i] = id[7 - i];
            stored[8 + i] = id[15 - i];
        }
        for (const IndexEntry& e : index) {
            if (e.type != 1 || e.space != 1 || !std::equal(stored, stored + 16, e.user)) continue;
            const fs::path dir = P(r.dir) / P(kRyuSaveParts[0]) / Hex16(e.save);
            if (PlainDir(dir)) out.push_back({LowerAscii(Hex16(e.program)), W(dir)});
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

Outcome ExportUserSaves(const Root& r, const std::string& userId, const std::wstring& destW) {
    Outcome out;
    const auto saves = UserSaves(r, userId, out.error);
    if (!out.error.empty()) return out;
    if (saves.empty()) {
        out.error = "MISSING";
        return out;
    }
    const fs::path dest = P(destW);
    uint64_t need = 0;
    for (const auto& s : saves) need += SizeOfTree(P(s.second));
    if (!ReadyDest(dest, need, out)) return out;
    if (InsideOrSame(P(r.dir), dest.parent_path())) {
        out.error = "REFUSED";
        return out;
    }
    out.where = destW;
    bool ok = true;
    for (const auto& s : saves) ok = CopyTree(P(s.second), dest / fs::path(s.first), out) && ok;
    out.ok = ok;
    if (!ok) out.error = "IO";
    return out;
}

Outcome MoveGameFile(const std::wstring& fileW, const std::wstring& toDirW) {
    Outcome out;
    const fs::path file = P(fileW), toDir = P(toDirW);
    std::error_code ec;
    if (!PlainFile(file) || !PlainDir(toDir)) {
        out.error = "MISSING";
        return out;
    }
    if (fs::equivalent(file.parent_path(), toDir, ec) || ec) {
        out.error = "REFUSED"; // already there
        return out;
    }
    const fs::path dest = toDir / file.filename();
    fs::path part = dest;
    part += L".dockside-part";
    if (fs::exists(fs::symlink_status(dest, ec)) || fs::exists(fs::symlink_status(part, ec))) {
        out.error = "EXISTS";
        return out;
    }
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        out.error = "IO";
        return out;
    }
    uint64_t free = 0;
    if (FreeSpace(toDirW, free) && free < size + (16u << 20)) {
        out.error = "NO_SPACE";
        return out;
    }
    if (!fs::copy_file(file, part, fs::copy_options::none, ec) || ec) {
        std::error_code re;
        fs::remove(part, re);
        out.error = "IO";
        return out;
    }
    const uintmax_t copied = fs::file_size(part, ec);
    if (ec || copied != size) {
        std::error_code re;
        fs::remove(part, re);
        out.error = "VERIFY";
        return out;
    }
    fs::rename(part, dest, ec);
    if (ec) {
        std::error_code re;
        fs::remove(part, re);
        out.error = "IO";
        return out;
    }
    out.where = W(dest);
    out.bytes = size;
    out.items = 1;
    // Only now, with the whole file under its name in the new place.
    if (!fs::remove(file, ec) || ec) {
        out.error = "KEPT";
        return out;
    }
    out.ok = true;
    return out;
}

std::wstring Stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    wchar_t b[40];
    swprintf(b, 40, L"%04d-%02d-%02d %02d.%02d.%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             tm.tm_sec);
    return b;
}

} // namespace emudata
