/*
 * Tokyo Jungle -- the player's copy of the game, and the data folder. See
 * game_data.h for what it is; the notes here are about how.
 *
 * Where a copy of the game is looked for, in order:
 *   - what the player pointed at: the game's folder (NPUA80523), any folder
 *     above it up to an RPCS3 config folder, its USRDIR, its PARAM.SFO or its
 *     decrypted EBOOT.elf;
 *   - RPCS3's own folders: the flatpak's (~/.var/app/net.rpcs3.RPCS3/config/
 *     rpcs3) and the native build's or AppImage's ($XDG_CONFIG_HOME/rpcs3).
 * A copy is complete when PARAM.SFO says NPUA80523 version 01.01, USRDIR/
 * holds the EBOOT.elf RPCS3's "Decrypt PS3 Binaries" writes (checked by its
 * SHA-256), and a dev_flash with the PS3's fonts is beside it.
 *
 * Files RPCS3 must handle first (.pkg, .rap, .PUP, an encrypted EBOOT.BIN)
 * are recognised by their contents, and the player is told which RPCS3 menu
 * takes them. RPCS3's folders are only ever read.
 */
#include "game_data.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>

namespace tjgd {

namespace {

const char kTitleId[] = "NPUA80523";
const char kAppVer[] = "01.01";
/* SHA-256 of the decrypted EBOOT: 1.01 (what this port is lifted from) and
 * 1.00 (the base game's, to say why it is refused). */
const char kElf101[] = "0929cfa2730b75200553bbb9b352d182e96ff87f2488479c71159bdf3dd6ff16";
const char kElf100[] = "6d253f6429485b43b816fd6884611377626ee95db5dd9200ea58395c93a0812d";
const char kFontProbe[] = "data/font/SCE-PS3-RD-R-LATIN.TTF";

std::string pathstr(const fs::path& p) { return p.string(); }
fs::path data_dir() { return fs::path(tj_data_root()); }

/* Up to n bytes of a file's start. */
std::vector<uint8_t> head(const fs::path& p, size_t n)
{
    std::vector<uint8_t> b(n);
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return {};
    b.resize(fread(b.data(), 1, n, f));
    fclose(f);
    return b;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t total = 0;
    uint8_t buf[64];
    size_t n = 0;

    static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

    void block(const uint8_t* p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* p, size_t len)
    {
        total += len;
        while (len) {
            const size_t k = std::min(len, 64 - n);
            memcpy(buf + n, p, k);
            n += k; p += k; len -= k;
            if (n == 64) { block(buf); n = 0; }
        }
    }

    std::string hex()
    {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (n != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};

/* PARAM.SFO's string and integer entries. */
std::map<std::string, std::string> read_sfo(const fs::path& p)
{
    std::map<std::string, std::string> out;
    const std::vector<uint8_t> b = head(p, 64 * 1024);
    if (b.size() < 20 || memcmp(b.data(), "\0PSF", 4) != 0) return out;
    auto le16 = [&](size_t o) { return (uint32_t)b[o] | b[o + 1] << 8; };
    auto le32 = [&](size_t o) { return le16(o) | le16(o + 2) << 16; };
    const uint32_t keys = le32(8), data = le32(12), count = le32(16);
    for (uint32_t i = 0; i < count && 20 + 16 * (i + 1) <= b.size(); i++) {
        const size_t e = 20 + 16 * i;
        const size_t ko = keys + le16(e), dof = data + le32(e + 12);
        const uint32_t fmt = le16(e + 2), len = le32(e + 4);
        if (ko >= b.size() || dof + len > b.size()) continue;
        const std::string key((const char*)b.data() + ko, strnlen((const char*)b.data() + ko, b.size() - ko));
        if (fmt == 0x0404 && len >= 4) out[key] = std::to_string(le32(dof));
        else out[key] = std::string((const char*)b.data() + dof, strnlen((const char*)b.data() + dof, len));
    }
    return out;
}

/* The flatpak's and the native build's config folders. */
std::vector<std::pair<std::string, fs::path>> rpcs3_configs()
{
    const char* x = getenv("XDG_CONFIG_HOME");
    return {{"RPCS3 (Flatpak)", home_dir() / ".var/app/net.rpcs3.RPCS3/config/rpcs3"},
            {"RPCS3", (x && *x ? fs::path(x) : home_dir() / ".config") / "rpcs3"}};
}

bool has_fonts(const fs::path& flash) { return !flash.empty() && present(flash / kFontProbe); }

/* Look at the game folder `game`. `elf` is a decrypted EBOOT the player
 * dropped (else USRDIR's own is looked for); `flash` a dev_flash they
 * pointed at (else the one in the RPCS3 config folder holding `game`, or
 * any RPCS3's). */
Source probe(const fs::path& game, const std::string& label, const fs::path& elf = {}, const fs::path& flash = {})
{
    Source s;
    s.label = label;
    const auto sfo = read_sfo(game / "PARAM.SFO");
    if (!sfo.count("TITLE_ID") || sfo.at("TITLE_ID") != kTitleId || !is_dir(game / "USRDIR")) return s;
    s.game = game;
    s.app_ver = sfo.count("APP_VER") ? sfo.at("APP_VER") : "?";
    std::vector<fs::path> elfs;
    if (!elf.empty()) elfs.push_back(elf);
    for (const char* n : {"USRDIR/EBOOT.elf", "USRDIR/EBOOT.ELF"}) elfs.push_back(game / n);
    for (const fs::path& e : elfs) {
        if (!present(e)) continue;
        const std::string sha = sha256_file(e);
        if (sha == kElf101) { s.elf = e; s.elf_why.clear(); break; }
        s.elf_why = sha == kElf100 ? "decrypted from version 1.00: install the 1.01 update in RPCS3, then decrypt again"
                                   : "is not Tokyo Jungle 1.01's decrypted executable";
    }
    std::vector<fs::path> flashes;
    if (!flash.empty()) flashes.push_back(flash);
    /* <cfg>/dev_hdd0/game/NPUA80523 -> <cfg>/dev_flash */
    flashes.push_back(game.parent_path().parent_path().parent_path() / "dev_flash");
    for (const auto& c : rpcs3_configs()) flashes.push_back(c.second / "dev_flash");
    flashes.push_back(data_dir() / "dev_flash");
    for (const fs::path& f : flashes)
        if (has_fonts(f)) { s.flash = f; break; }
    std::error_code ec;
    for (fs::directory_iterator it(game / "USRDIR/data/dlc", ec), end; !ec && it != end; it.increment(ec))
        if (lower(it->path().extension().string()) == ".edat") s.dlc++;
    return s;
}

uint64_t tree_bytes(const fs::path& dir)
{
    uint64_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}

/* `from` into `to` (a new folder) as hard links, copying what can't be
 * linked (another drive), leaving out what `skip` says. */
bool link_tree(const fs::path& from, const fs::path& to, const std::function<bool(const fs::path&)>& skip,
               const std::string& what)
{
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    const uint64_t total = tree_bytes(from);
    uint64_t done = 0;
    size_t linked = 0, copied = 0;
    set_step("Putting " + what + " in the data folder", 0);
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(from), dst = to / rel;
        if (skip && skip(rel)) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory()) { fs::create_directories(dst, ec); continue; }
        if (!it->is_regular_file()) continue;
        const uint64_t size = it->file_size();
        std::error_code lk;
        fs::create_hard_link(it->path(), dst, lk);
        if (!lk) {
            linked++;
        } else {
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, lk);
            if (lk) return fail("could not copy " + pathstr(rel) + ": " + lk.message());
            copied++;
        }
        done += size;
        if (total) set_frac((double)done / (double)total);
    }
    if (ec) return fail("could not read " + pathstr(from) + ": " + ec.message());
    say("%s: %zu file(s) linked, %zu copied", what.c_str(), linked, copied);
    return true;
}

/* `tmp` becomes `dst`, replacing what was there. */
bool swap_in(const fs::path& tmp, const fs::path& dst)
{
    std::error_code ec;
    const fs::path old = fs::path(pathstr(dst) + ".old");
    fs::remove_all(old, ec);
    if (present(dst)) fs::rename(dst, old, ec);
    if (!ec) fs::rename(tmp, dst, ec);
    if (ec) return fail("could not move " + pathstr(dst) + " into place: " + ec.message());
    fs::remove_all(old, ec);
    return true;
}

bool same(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

}  // namespace

/* ---------------------------------------------------------------------------
 * Small helpers
 * -----------------------------------------------------------------------*/
fs::path home_dir()
{
    const char* h = getenv("HOME");
    return fs::path(h && *h ? h : ".");
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool present(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

bool is_dir(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec);
}

std::string sha256_file(const fs::path& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "";
    Sha256 s;
    std::vector<uint8_t> b(1 << 20);
    size_t k;
    while ((k = fread(b.data(), 1, b.size(), f)) > 0) s.update(b.data(), k);
    fclose(f);
    return s.hex();
}

/* ---------------------------------------------------------------------------
 * Progress
 * -----------------------------------------------------------------------*/
Progress& progress()
{
    static Progress p;
    return p;
}

void say(const char* fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("[launcher] %s\n", line);
    fflush(stdout);
    std::lock_guard<std::mutex> lk(progress().mu);
    progress().log.emplace_back(line);
}

void set_step(const std::string& step, double frac)
{
    {
        std::lock_guard<std::mutex> lk(progress().mu);
        progress().step = step;
        progress().detail.clear();
        progress().frac = frac;
    }
    say("%s", step.c_str());
}

void set_frac(double frac, const std::string& detail)
{
    std::lock_guard<std::mutex> lk(progress().mu);
    progress().frac = frac;
    if (!detail.empty()) progress().detail = detail;
}

bool fail(const std::string& why)
{
    {
        std::lock_guard<std::mutex> lk(progress().mu);
        progress().error = why;
    }
    say("STOPPED: %s", why.c_str());
    return false;
}

/* ---------------------------------------------------------------------------
 * Finding the game
 * -----------------------------------------------------------------------*/
bool Source::complete() const { return app_ver == kAppVer && !elf.empty() && !flash.empty(); }
int Source::score() const { return (!game.empty()) + (app_ver == kAppVer) + 2 * !elf.empty() + !flash.empty(); }

bool locate(const fs::path& p, Pointed* out, std::string* msg)
{
    const std::string name = p.filename().string(), lname = lower(name);
    if (!is_dir(p)) {
        const std::vector<uint8_t> h = head(p, 16);
        auto starts = [&](const char* m, size_t n) { return h.size() >= n && memcmp(h.data(), m, n) == 0; };
        if (lname == "param.sfo") return locate(p.parent_path(), out, msg);
        if (starts("\x7f" "ELF", 4) && lower(p.parent_path().filename().string()) == "usrdir") {
            out->elf = p;
            return locate(p.parent_path().parent_path(), out, msg);
        }
        if (starts("\x7fPKG", 4))
            *msg = name + ": install it in RPCS3 first (File > Install Packages/Raps/Edats), then drop the game's folder here";
        else if (lname.size() > 4 && lname.compare(lname.size() - 4, 4, ".rap") == 0)
            *msg = name + ": give it to RPCS3 with the game's package (File > Install Packages/Raps/Edats)";
        else if (starts("SCEUF", 5))
            *msg = name + ": install it in RPCS3 (File > Install Firmware)";
        else if (starts("SCE\0", 4))
            *msg = name + " is still encrypted: decrypt it in RPCS3 (Utilities > Decrypt PS3 Binaries) and drop the game's folder";
        else
            *msg = name + " isn't something the launcher uses: drop the game's folder (" + kTitleId + ")";
        return false;
    }
    /* The game's folder itself, or one above it, or its USRDIR. */
    const fs::path tail = fs::path("dev_hdd0/game") / kTitleId;
    const std::vector<fs::path> cands = {p, p / kTitleId, p / "game" / kTitleId, p / tail, p / "rpcs3" / tail,
                                         p / "config/rpcs3" / tail, p / ".config/rpcs3" / tail,
                                         p / ".var/app/net.rpcs3.RPCS3/config/rpcs3" / tail};
    for (const fs::path& c : cands)
        if (present(c / "PARAM.SFO") && is_dir(c / "USRDIR")) { out->game = c; return true; }
    if (lname == "usrdir" && present(p.parent_path() / "PARAM.SFO")) { out->game = p.parent_path(); return true; }
    /* A dev_flash, or a folder holding one. */
    for (const fs::path& c : {p, p / "dev_flash", p.parent_path().parent_path()})
        if (has_fonts(c)) {
            out->flash = c;
            *msg = "PS3 fonts: " + pathstr(c);
            return true;
        }
    *msg = "No copy of Tokyo Jungle (" + std::string(kTitleId) + ") in " + pathstr(p);
    return false;
}

Source find_source(const Pointed& pt)
{
    std::vector<Source> all;
    if (!pt.game.empty()) all.push_back(probe(pt.game, "The folder you chose", pt.elf, pt.flash));
    for (const auto& [label, cfg] : rpcs3_configs())
        all.push_back(probe(cfg / "dev_hdd0/game" / kTitleId, label, {}, pt.flash));
    Source best = all.front();
    for (const Source& s : all)
        if (s.score() > best.score()) best = s;
    if (best.flash.empty() && has_fonts(pt.flash)) best.flash = pt.flash;
    return best;
}

std::string what_is_missing(const Source& s)
{
    if (s.game.empty()) return "Not found. Install it in RPCS3 (see the guide), or drop its folder here";
    if (s.app_ver != kAppVer) return "Version " + s.app_ver + ": install the 1.01 update in RPCS3";
    if (s.elf.empty())
        return !s.elf_why.empty() ? "EBOOT.elf " + s.elf_why
                                  : "Not decrypted yet. In RPCS3: Utilities > Decrypt PS3 Binaries, then pick USRDIR/EBOOT.BIN";
    if (s.flash.empty()) return "No PS3 fonts. In RPCS3: File > Install Firmware";
    return "";
}

bool import(const Source& s)
{
    {
        std::lock_guard<std::mutex> lk(progress().mu);
        progress().error.clear();
    }
    const fs::path root = data_dir();
    if (!s.complete()) return fail("the copy of the game found is incomplete: " + what_is_missing(s));
    std::error_code ec;
    fs::create_directories(root, ec);
    say("Taking the game from %s", pathstr(s.game).c_str());
    /* The game's folder, without its executables: the decrypted one goes to
     * <data>/EBOOT.ELF. */
    auto skip = [](const fs::path& rel) {
        const std::string r = lower(rel.generic_string());
        return r == "usrdir/eboot.bin" || r == "usrdir/eboot.elf";
    };
    if (same(s.game, root / "game")) {
        say("The game is already in the data folder");
    } else if (!link_tree(s.game, root / "game.new", skip, "the game's files") ||
               !swap_in(root / "game.new", root / "game")) {
        return false;
    }
    if (!same(s.elf, root / "EBOOT.ELF")) {
        fs::copy_file(s.elf, root / "EBOOT.ELF.new", fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::rename(root / "EBOOT.ELF.new", root / "EBOOT.ELF", ec);
        if (ec) return fail("could not copy the executable: " + ec.message());
    }
    if (!same(s.flash, root / "dev_flash") &&
        (!link_tree(s.flash / "data/font", root / "dev_flash/data/font.new", nullptr, "the PS3 fonts") ||
         !swap_in(root / "dev_flash/data/font.new", root / "dev_flash/data/font")))
        return false;
    TjGamePaths p;
    std::string why;
    if (!tj_data_ready(&p, &why)) return fail("the data folder is still incomplete: " + why);
    say("The game is in the data folder");
    return true;
}

}  // namespace tjgd

/* ---------------------------------------------------------------------------
 * The data folder
 * -----------------------------------------------------------------------*/
std::string tj_data_root(void)
{
    const char* d = getenv("TJ_DATA");
    return d && *d ? std::string(d) : (tjgd::home_dir() / "TokyoJungle-data").string();
}

bool tj_data_ready(TjGamePaths* out, std::string* why)
{
    using namespace tjgd;
    const fs::path root = data_dir();
    out->eboot = pathstr(root / "EBOOT.ELF");
    out->game = pathstr(root / "game");
    out->dev_flash = pathstr(root / "dev_flash");
    std::string w;
    if (!present(out->eboot)) w = "no decrypted executable";
    else if (sha256_file(out->eboot) != kElf101) w = "EBOOT.ELF is not the decrypted 1.01 executable";
    else {
        const auto sfo = read_sfo(root / "game/PARAM.SFO");
        if (!sfo.count("TITLE_ID") || sfo.at("TITLE_ID") != kTitleId) w = "no game data";
        else if (!sfo.count("APP_VER") || sfo.at("APP_VER") != kAppVer) w = "the game data is not version 1.01";
        else if (!is_dir(root / "game/USRDIR/data")) w = "the game data has no USRDIR/data";
        else if (!has_fonts(root / "dev_flash")) w = "no PS3 system fonts";
    }
    if (why) *why = w;
    return w.empty();
}
