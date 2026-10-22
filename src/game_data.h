/*
 * Tokyo Jungle -- the player's copy of the game, and the data folder.
 *
 * The player installs and decrypts their own copy with RPCS3, by hand
 * (docs/SETUP_GUIDE.md): this repository holds no keys, and nothing here
 * decrypts the game. The launcher (launcher/) finds that copy -- in RPCS3's
 * own folders, or wherever the player points it -- checks it, and puts it in
 * the data folder, which the game runs from when started without arguments:
 *
 *   <data>/EBOOT.ELF                 the decrypted 1.01 executable
 *   <data>/game/                     PARAM.SFO + USRDIR/ (hard links to the
 *                                    copy found, where the drive allows)
 *   <data>/dev_flash/data/font/      the PS3's system fonts
 *
 * <data> is $TJ_DATA, else ~/TokyoJungle-data. The game uses only
 * tj_data_root() and tj_data_ready(); the rest is the launcher's.
 */
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct TjGamePaths {
    std::string eboot;       /* <data>/EBOOT.ELF */
    std::string game;        /* <data>/game (holds USRDIR/) */
    std::string dev_flash;   /* <data>/dev_flash */
};

/* $TJ_DATA, else ~/TokyoJungle-data. */
std::string tj_data_root(void);

/* True when the data folder has everything the game needs; *why says what
 * is missing otherwise. */
bool tj_data_ready(TjGamePaths* out, std::string* why);

namespace tjgd {

namespace fs = std::filesystem;

fs::path home_dir();
std::string lower(std::string s);
bool present(const fs::path& p);
bool is_dir(const fs::path& p);
std::string sha256_file(const fs::path& p);

/* What a long job is doing, for whoever shows it. say() also prints. */
struct Progress {
    std::mutex mu;
    std::vector<std::string> log;     /* everything said */
    std::string step;                 /* what is happening now */
    std::string detail;               /* a second line: counts, times */
    double frac = -1;                 /* 0..1, or < 0 when there is no telling */
    std::string error;                /* why the last job stopped */
};
Progress& progress();
void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void set_step(const std::string& step, double frac = -1);
void set_frac(double frac, const std::string& detail = "");
bool fail(const std::string& why);   /* records it; returns false */

/* A copy of the game on disk, and how complete it is. */
struct Source {
    std::string label;      /* where it was found, for the player */
    fs::path game;          /* the folder holding PARAM.SFO and USRDIR/; empty = none */
    std::string app_ver;    /* PARAM.SFO's APP_VER */
    fs::path elf;           /* the decrypted 1.01 EBOOT; empty = none */
    std::string elf_why;    /* an EBOOT.elf that is there but not usable: why */
    fs::path flash;         /* a dev_flash with the PS3's fonts; empty = none */
    int dlc = 0;            /* DLC .edat files */

    bool complete() const;
    int score() const;
};

/* What the player pointed at (a folder dropped or chosen). */
struct Pointed { fs::path game, elf, flash; };

/* Turn `p` into a game folder (or a dev_flash) in *out. Files RPCS3 must
 * take first (.pkg, .rap, .PUP, an encrypted EBOOT) are explained in *msg. */
bool locate(const fs::path& p, Pointed* out, std::string* msg);

/* The best copy: what the player pointed at, else RPCS3's own. */
Source find_source(const Pointed& pt);

/* A complete copy into the data folder. True when the folder is ready. */
bool import(const Source& s);

/* What is missing from `s`, for the player: empty when it is complete. */
std::string what_is_missing(const Source& s);

}  // namespace tjgd
