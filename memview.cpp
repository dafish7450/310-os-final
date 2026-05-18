/*
 * memview.cpp — SDL2 Memory Viewer
 *
 * Features:
 *   - Live system RAM / SWAP bars (updates every 1 second)
 *   - Scrollable, sortable process list
 *   - Click a process row to open a detail panel showing extra /proc info
 *   - Detail panel has a "Kill Process" button (sends SIGTERM)
 *   - Hover highlight, color-coded memory usage
 *
 * Build:
 *   g++ -std=c++17 memview.cpp -o memview \
 *       $(sdl2-config --cflags --libs) -lSDL2_ttf
 *
 * Run:
 *   ./memview
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <chrono>
#include <signal.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// ─── Window / layout ──────────────────────────────────────────────────────────
static constexpr int WIN_W       = 900;
static constexpr int WIN_H       = 680;
static constexpr int HEADER_H    = 160;
static constexpr int COL_HDR_H   = 36;
static constexpr int ROW_H       = 28;
static constexpr int SCROLLBAR_W = 12;
static constexpr int LIST_Y      = HEADER_H + COL_HDR_H;
static constexpr int LIST_H      = WIN_H - LIST_Y;
static constexpr int LIST_W      = WIN_W - SCROLLBAR_W;

// Column X positions
static constexpr int COL_PID_X  = 10;
static constexpr int COL_NAME_X = 90;
static constexpr int COL_RSS_X  = 520;
static constexpr int COL_VSZ_X  = 650;
static constexpr int COL_PCT_X  = 780;

// Detail panel dimensions (centered modal)
static constexpr int DETAIL_W   = 560;
static constexpr int DETAIL_H   = 380;
static constexpr int DETAIL_X   = (WIN_W - DETAIL_W) / 2;
static constexpr int DETAIL_Y   = (WIN_H - DETAIL_H) / 2;

// Buttons inside the detail panel
static constexpr int BTN_W      = 160;
static constexpr int BTN_H      = 38;

// ─── Colors ───────────────────────────────────────────────────────────────────
struct Color { uint8_t r, g, b, a; };
static constexpr Color C_BG        = {  15,  17,  22, 255 };
static constexpr Color C_PANEL     = {  22,  26,  35, 255 };
static constexpr Color C_HEADER_BG = {  18,  21,  30, 255 };
static constexpr Color C_ROW_ODD   = {  20,  24,  33, 255 };
static constexpr Color C_ROW_EVEN  = {  25,  30,  42, 255 };
static constexpr Color C_ROW_HOV   = {  35,  45,  68, 255 };
static constexpr Color C_ROW_SEL   = {  30,  60,  90, 255 };
static constexpr Color C_ACCENT    = {  64, 196, 255, 255 };
static constexpr Color C_GREEN     = {  80, 220, 140, 255 };
static constexpr Color C_ORANGE    = { 255, 165,  60, 255 };
static constexpr Color C_RED       = { 255,  80,  80, 255 };
static constexpr Color C_TEXT      = { 210, 220, 235, 255 };
static constexpr Color C_TEXT_DIM  = { 120, 135, 160, 255 };
static constexpr Color C_SORT_IND  = { 255, 210,  60, 255 };
static constexpr Color C_SCROLLBAR = {  50,  60,  85, 255 };
static constexpr Color C_SCROLLTHM = {  90, 110, 160, 255 };
static constexpr Color C_DIVIDER   = {  40,  48,  68, 255 };
static constexpr Color C_BAR_BG    = {  35,  42,  60, 255 };
static constexpr Color C_MODAL_BG  = {  18,  22,  34, 245 };
static constexpr Color C_MODAL_BDR = {  64, 196, 255, 200 };
static constexpr Color C_BTN_KILL  = { 180,  40,  40, 255 };
static constexpr Color C_BTN_CLOSE = {  45,  55,  80, 255 };
static constexpr Color C_BTN_HOV   = { 220,  60,  60, 255 };
static constexpr Color C_OVERLAY   = {   0,   0,   0, 160 };

// ─── Data structures ──────────────────────────────────────────────────────────
struct ProcessEntry {
    pid_t       pid;
    std::string name;
    std::string cmdline;
    uint64_t    rss_kb;
    uint64_t    vsz_kb;
    uint64_t    shared_kb;
    uint64_t    stack_kb;
    uint64_t    data_kb;
    uint64_t    swap_kb;
    int         threads;
    double      pct;
};

struct SysMemInfo {
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
};

enum class SortCol { PID, NAME, RSS, VSZ, PCT };

struct AppData {
    TTF_Font *font_sm  = nullptr;
    TTF_Font *font_med = nullptr;
    TTF_Font *font_lg  = nullptr;

    SysMemInfo                sysinfo{};
    std::vector<ProcessEntry> processes;

    SortCol sort_col      = SortCol::RSS;
    bool    sort_asc      = false;
    int     scroll_offset = 0;
    int     hover_row     = -1;

    // Detail panel state
    bool        detail_open  = false;
    int         detail_idx   = -1;
    bool        kill_hover   = false;
    bool        close_hover  = false;
    std::string kill_msg;

    std::chrono::steady_clock::time_point last_update;
    static constexpr int UPDATE_MS = 1000;   // 1-second refresh
};

// ─── Draw helpers ─────────────────────────────────────────────────────────────
static void setColor(SDL_Renderer *r, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
static void fillRect(SDL_Renderer *r, int x, int y, int w, int h, Color c) {
    setColor(r, c);
    SDL_Rect rc{x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}
static void drawRect(SDL_Renderer *r, int x, int y, int w, int h, Color c) {
    setColor(r, c);
    SDL_Rect rc{x, y, w, h};
    SDL_RenderDrawRect(r, &rc);
}
static void drawHLine(SDL_Renderer *r, int x1, int x2, int y, Color c) {
    setColor(r, c);
    SDL_RenderDrawLine(r, x1, y, x2, y);
}

static int renderText(SDL_Renderer *r, TTF_Font *f,
                      const char *txt, Color c,
                      int x, int y, int max_w = 0) {
    if (!txt || txt[0] == '\0') return 0;
    SDL_Color sc{c.r, c.g, c.b, c.a};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, txt, sc);
    if (!surf) return 0;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    int tw = surf->w, th = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return 0;
    if (max_w > 0 && tw > max_w) tw = max_w;
    SDL_Rect src{0, 0, tw, th};
    SDL_Rect dst{x, y, tw, th};
    SDL_RenderCopy(r, tex, &src, &dst);
    SDL_DestroyTexture(tex);
    return tw;
}

static void drawBar(SDL_Renderer *r, int x, int y, int w, int h,
                    double fraction, Color bg, Color fg) {
    fillRect(r, x, y, w, h, bg);
    int filled = (int)(std::clamp(fraction, 0.0, 1.0) * w);
    if (filled > 0) fillRect(r, x, y, filled, h, fg);
    drawRect(r, x, y, w, h, C_DIVIDER);
}

static std::string humanKB(uint64_t kb) {
    char buf[32];
    if      (kb >= 1024*1024) snprintf(buf, sizeof(buf), "%.1f GB", kb/(1024.0*1024.0));
    else if (kb >= 1024)      snprintf(buf, sizeof(buf), "%.1f MB", kb/1024.0);
    else                      snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)kb);
    return buf;
}

static bool pointInRect(int x, int y, SDL_Rect rc) {
    return x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h;
}

// ─── /proc parsing ────────────────────────────────────────────────────────────
static bool readSysMemInfo(SysMemInfo &info) {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        uint64_t v;
        if      (sscanf(line.c_str(), "MemTotal: %llu kB",    (unsigned long long*)&v)==1) info.total_kb      = v;
        else if (sscanf(line.c_str(), "MemFree: %llu kB",     (unsigned long long*)&v)==1) info.free_kb       = v;
        else if (sscanf(line.c_str(), "MemAvailable: %llu kB",(unsigned long long*)&v)==1) info.available_kb  = v;
        else if (sscanf(line.c_str(), "SwapTotal: %llu kB",   (unsigned long long*)&v)==1) info.swap_total_kb = v;
        else if (sscanf(line.c_str(), "SwapFree: %llu kB",    (unsigned long long*)&v)==1) info.swap_free_kb  = v;
    }
    return true;
}

static std::string readProcessName(pid_t pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    std::ifstream f(path); std::string name;
    if (f.is_open()) std::getline(f, name);
    return name.empty() ? "?" : name;
}

static std::string readCmdline(pid_t pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string s, tok;
    while (std::getline(f, tok, '\0')) { if (!s.empty()) s += ' '; s += tok; }
    if (s.size() > 78) { s = s.substr(0, 75); s += "..."; }
    return s;
}

static bool readProcessStatus(pid_t pid, ProcessEntry &pe) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/status", pid);
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    pe.rss_kb = pe.vsz_kb = pe.shared_kb = pe.stack_kb = pe.data_kb = pe.swap_kb = 0;
    pe.threads = 0;
    while (std::getline(f, line)) {
        uint64_t v; int iv;
        if      (sscanf(line.c_str(), "VmRSS: %llu kB",   (unsigned long long*)&v)==1) pe.rss_kb    = v;
        else if (sscanf(line.c_str(), "VmSize: %llu kB",  (unsigned long long*)&v)==1) pe.vsz_kb    = v;
        else if (sscanf(line.c_str(), "RssFile: %llu kB", (unsigned long long*)&v)==1) pe.shared_kb = v;
        else if (sscanf(line.c_str(), "VmStk: %llu kB",   (unsigned long long*)&v)==1) pe.stack_kb  = v;
        else if (sscanf(line.c_str(), "VmData: %llu kB",  (unsigned long long*)&v)==1) pe.data_kb   = v;
        else if (sscanf(line.c_str(), "VmSwap: %llu kB",  (unsigned long long*)&v)==1) pe.swap_kb   = v;
        else if (sscanf(line.c_str(), "Threads: %d",      &iv)==1)                      pe.threads   = iv;
    }
    return true;
}

static void gatherProcesses(AppData *app) {
    app->processes.clear();
    for (auto &entry : std::filesystem::directory_iterator("/proc")) {
        const std::string &fname = entry.path().filename().string();
        if (!std::all_of(fname.begin(), fname.end(), ::isdigit)) continue;
        pid_t pid = std::stoi(fname);
        ProcessEntry pe{};
        pe.pid = pid;
        if (!readProcessStatus(pid, pe)) continue;
        pe.name    = readProcessName(pid);
        pe.cmdline = readCmdline(pid);
        pe.pct     = app->sysinfo.total_kb > 0
                     ? 100.0 * pe.rss_kb / app->sysinfo.total_kb : 0.0;
        app->processes.push_back(pe);
    }
}

static void sortProcesses(AppData *app) {
    auto &v = app->processes;
    bool asc = app->sort_asc;
    switch (app->sort_col) {
        case SortCol::PID:  std::sort(v.begin(),v.end(),[asc](auto&a,auto&b){return asc?a.pid<b.pid:a.pid>b.pid;}); break;
        case SortCol::NAME: std::sort(v.begin(),v.end(),[asc](auto&a,auto&b){return asc?a.name<b.name:a.name>b.name;}); break;
        case SortCol::RSS:  std::sort(v.begin(),v.end(),[asc](auto&a,auto&b){return asc?a.rss_kb<b.rss_kb:a.rss_kb>b.rss_kb;}); break;
        case SortCol::VSZ:  std::sort(v.begin(),v.end(),[asc](auto&a,auto&b){return asc?a.vsz_kb<b.vsz_kb:a.vsz_kb>b.vsz_kb;}); break;
        case SortCol::PCT:  std::sort(v.begin(),v.end(),[asc](auto&a,auto&b){return asc?a.pct<b.pct:a.pct>b.pct;}); break;
    }
}

static void refreshData(AppData *app) {
    readSysMemInfo(app->sysinfo);

    // Remember selected PID so we can re-locate it after refresh
    pid_t selected_pid = -1;
    if (app->detail_open && app->detail_idx >= 0 &&
        app->detail_idx < (int)app->processes.size())
        selected_pid = app->processes[app->detail_idx].pid;

    gatherProcesses(app);
    sortProcesses(app);

    if (selected_pid != -1) {
        app->detail_idx = -1;
        for (int i = 0; i < (int)app->processes.size(); i++) {
            if (app->processes[i].pid == selected_pid) {
                app->detail_idx = i;
                break;
            }
        }
        // Process vanished (was killed or exited)
        if (app->detail_idx == -1) {
            app->detail_open = false;
            app->kill_msg    = "";
        }
    }
}

// ─── Rendering ────────────────────────────────────────────────────────────────
static void renderHeader(SDL_Renderer *r, AppData *app) {
    fillRect(r, 0, 0, WIN_W, HEADER_H, C_HEADER_BG);
    renderText(r, app->font_lg, "Memory Viewer", C_ACCENT, 18, 14);

    uint64_t used = app->sysinfo.total_kb - app->sysinfo.available_kb;
    char stats[160];
    snprintf(stats, sizeof(stats),
             "RAM: %s used / %s total   |   Processes: %d   |   Updates every 1s",
             humanKB(used).c_str(), humanKB(app->sysinfo.total_kb).c_str(),
             (int)app->processes.size());
    renderText(r, app->font_sm, stats, C_TEXT_DIM, 18, 50);

    // RAM bar
    renderText(r, app->font_sm, "RAM", C_TEXT, 18, 80);
    double ram_frac = app->sysinfo.total_kb > 0 ? (double)used / app->sysinfo.total_kb : 0.0;
    Color ram_c = ram_frac < 0.6 ? C_GREEN : ram_frac < 0.85 ? C_ORANGE : C_RED;
    drawBar(r, 70, 82, WIN_W - 200, 18, ram_frac, C_BAR_BG, ram_c);
    char pct[16]; snprintf(pct, sizeof(pct), "%.1f%%", ram_frac * 100.0);
    renderText(r, app->font_sm, pct, C_TEXT, WIN_W - 120, 80);
    renderText(r, app->font_sm, humanKB(used).c_str(), C_TEXT_DIM, WIN_W - 190, 80);

    // SWAP bar
    renderText(r, app->font_sm, "SWAP", C_TEXT, 18, 112);
    uint64_t swap_used = app->sysinfo.swap_total_kb - app->sysinfo.swap_free_kb;
    double swap_frac = app->sysinfo.swap_total_kb > 0
        ? (double)swap_used / app->sysinfo.swap_total_kb : 0.0;
    drawBar(r, 70, 114, WIN_W - 200, 18, swap_frac, C_BAR_BG, C_ORANGE);
    if (app->sysinfo.swap_total_kb > 0)
        snprintf(pct, sizeof(pct), "%.1f%%", swap_frac * 100.0);
    else
        snprintf(pct, sizeof(pct), "N/A");
    renderText(r, app->font_sm, pct, C_TEXT, WIN_W - 120, 112);
    renderText(r, app->font_sm, humanKB(swap_used).c_str(), C_TEXT_DIM, WIN_W - 190, 112);

    drawHLine(r, 0, WIN_W, HEADER_H - 1, C_ACCENT);
}

static void renderArrow(SDL_Renderer *r, int cx, int cy, bool asc) {
    setColor(r, C_SORT_IND);
    if (asc) {
        SDL_RenderDrawLine(r, cx, cy-5, cx-4, cy+3);
        SDL_RenderDrawLine(r, cx, cy-5, cx+4, cy+3);
        SDL_RenderDrawLine(r, cx-4, cy+3, cx+4, cy+3);
    } else {
        SDL_RenderDrawLine(r, cx, cy+5, cx-4, cy-3);
        SDL_RenderDrawLine(r, cx, cy+5, cx+4, cy-3);
        SDL_RenderDrawLine(r, cx-4, cy-3, cx+4, cy-3);
    }
}

static void renderColumnHeaders(SDL_Renderer *r, AppData *app) {
    fillRect(r, 0, HEADER_H, WIN_W, COL_HDR_H, C_PANEL);
    struct ColDef { const char *label; int x; SortCol col; };
    ColDef cols[] = {
        {"PID",  COL_PID_X,  SortCol::PID},
        {"NAME", COL_NAME_X, SortCol::NAME},
        {"RSS",  COL_RSS_X,  SortCol::RSS},
        {"VIRT", COL_VSZ_X,  SortCol::VSZ},
        {"%MEM", COL_PCT_X,  SortCol::PCT},
    };
    int cy = HEADER_H + COL_HDR_H / 2;
    for (auto &col : cols) {
        Color tc = (app->sort_col == col.col) ? C_SORT_IND : C_TEXT_DIM;
        int tw = renderText(r, app->font_sm, col.label, tc, col.x, HEADER_H + 10);
        if (app->sort_col == col.col)
            renderArrow(r, col.x + tw + 10, cy, app->sort_asc);
    }
    renderText(r, app->font_sm, "(click row for details)", C_TEXT_DIM,
               COL_NAME_X + 160, HEADER_H + 10);
    drawHLine(r, 0, WIN_W, HEADER_H + COL_HDR_H - 1, C_DIVIDER);
}

static void renderProcessList(SDL_Renderer *r, AppData *app) {
    SDL_Rect clip{0, LIST_Y, LIST_W, LIST_H};
    SDL_RenderSetClipRect(r, &clip);

    int n = (int)app->processes.size();
    int total_h = n * ROW_H;
    int max_scroll = std::max(0, total_h - LIST_H);
    app->scroll_offset = std::clamp(app->scroll_offset, 0, max_scroll);

    int first_row = app->scroll_offset / ROW_H;
    int last_row  = std::min(n - 1, (app->scroll_offset + LIST_H) / ROW_H + 1);

    for (int i = first_row; i <= last_row; i++) {
        int row_y = LIST_Y + (i * ROW_H) - app->scroll_offset;
        bool selected = (app->detail_open && i == app->detail_idx);
        Color bg = selected            ? C_ROW_SEL
                 : (i == app->hover_row) ? C_ROW_HOV
                 : (i % 2 == 0)          ? C_ROW_EVEN
                                         : C_ROW_ODD;
        fillRect(r, 0, row_y, LIST_W, ROW_H, bg);

        const ProcessEntry &pe = app->processes[i];
        int ty = row_y + (ROW_H - 13) / 2;

        char buf[32];
        snprintf(buf, sizeof(buf), "%d", pe.pid);
        renderText(r, app->font_sm, buf, C_TEXT_DIM, COL_PID_X, ty);
        renderText(r, app->font_sm, pe.name.c_str(), C_TEXT,
                   COL_NAME_X, ty, COL_RSS_X - COL_NAME_X - 8);
        renderText(r, app->font_sm, humanKB(pe.rss_kb).c_str(), C_GREEN, COL_RSS_X, ty);
        renderText(r, app->font_sm, humanKB(pe.vsz_kb).c_str(), C_TEXT_DIM, COL_VSZ_X, ty);
        snprintf(buf, sizeof(buf), "%.2f%%", pe.pct);
        Color pc = pe.pct > 5.0 ? C_RED : pe.pct > 1.0 ? C_ORANGE : C_TEXT_DIM;
        renderText(r, app->font_sm, buf, pc, COL_PCT_X, ty);
        drawHLine(r, 0, LIST_W, row_y + ROW_H - 1, C_DIVIDER);
    }

    SDL_RenderSetClipRect(r, nullptr);

    // Scrollbar
    fillRect(r, LIST_W, LIST_Y, SCROLLBAR_W, LIST_H, C_SCROLLBAR);
    if (total_h > LIST_H) {
        double tf = (double)LIST_H / total_h;
        int    th = std::max(20, (int)(tf * LIST_H));
        double sf = (double)app->scroll_offset / max_scroll;
        int    ty = LIST_Y + (int)(sf * (LIST_H - th));
        fillRect(r, LIST_W, ty, SCROLLBAR_W, th, C_SCROLLTHM);
    }
}

// ── Button rects ──────────────────────────────────────────────────────────────
static SDL_Rect killBtnRect() {
    return {DETAIL_X + 30, DETAIL_Y + DETAIL_H - BTN_H - 20, BTN_W, BTN_H};
}
static SDL_Rect closeBtnRect() {
    return {DETAIL_X + DETAIL_W - BTN_W - 30, DETAIL_Y + DETAIL_H - BTN_H - 20, BTN_W, BTN_H};
}

static void renderDetailPanel(SDL_Renderer *r, AppData *app) {
    if (!app->detail_open || app->detail_idx < 0 ||
        app->detail_idx >= (int)app->processes.size()) return;

    // Semi-transparent overlay
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    fillRect(r, 0, 0, WIN_W, WIN_H, C_OVERLAY);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    // Modal box
    fillRect(r, DETAIL_X, DETAIL_Y, DETAIL_W, DETAIL_H, C_MODAL_BG);
    drawRect(r, DETAIL_X,   DETAIL_Y,   DETAIL_W,   DETAIL_H,   C_MODAL_BDR);
    drawRect(r, DETAIL_X+2, DETAIL_Y+2, DETAIL_W-4, DETAIL_H-4, C_DIVIDER);

    const ProcessEntry &pe = app->processes[app->detail_idx];

    // Title bar
    fillRect(r, DETAIL_X, DETAIL_Y, DETAIL_W, 40, C_PANEL);
    drawHLine(r, DETAIL_X, DETAIL_X + DETAIL_W, DETAIL_Y + 40, C_MODAL_BDR);
    char title[128];
    snprintf(title, sizeof(title), "Details  —  %s  (PID %d)", pe.name.c_str(), pe.pid);
    renderText(r, app->font_med, title, C_ACCENT, DETAIL_X + 14, DETAIL_Y + 10);

    // Detail rows
    char b1[32], b2[32], b3[32], b4[32], b5[32], b6[32], b7[16];
    snprintf(b1, sizeof(b1), "%s", humanKB(pe.rss_kb).c_str());
    snprintf(b2, sizeof(b2), "%s", humanKB(pe.vsz_kb).c_str());
    snprintf(b3, sizeof(b3), "%s", humanKB(pe.shared_kb).c_str());
    snprintf(b4, sizeof(b4), "%s", humanKB(pe.data_kb).c_str());
    snprintf(b5, sizeof(b5), "%s", humanKB(pe.stack_kb).c_str());
    snprintf(b6, sizeof(b6), "%s", humanKB(pe.swap_kb).c_str());
    snprintf(b7, sizeof(b7), "%d", pe.threads);

    struct Row { const char *label; const char *value; Color vc; };
    Row rows[] = {
        {"RSS (Physical RAM):", b1, C_GREEN},
        {"Virtual Size:",       b2, C_TEXT},
        {"Shared (RssFile):",   b3, C_TEXT_DIM},
        {"Data Segment:",       b4, C_TEXT_DIM},
        {"Stack Size:",         b5, C_TEXT_DIM},
        {"Swap Usage:",         b6, pe.swap_kb > 0 ? C_ORANGE : C_TEXT_DIM},
        {"Threads:",            b7, C_TEXT},
    };

    int ry = DETAIL_Y + 56;
    for (auto &row : rows) {
        renderText(r, app->font_sm, row.label, C_TEXT_DIM, DETAIL_X + 20, ry);
        renderText(r, app->font_sm, row.value,  row.vc,    DETAIL_X + 220, ry);
        ry += 24;
    }

    // % RAM mini-bar
    ry += 4;
    renderText(r, app->font_sm, "% of total RAM:", C_TEXT_DIM, DETAIL_X + 20, ry);
    char pct_buf[32]; snprintf(pct_buf, sizeof(pct_buf), "%.3f%%", pe.pct);
    // Scale bar so even tiny values are visible (cap display at 20%)
    double bar_frac = std::min(pe.pct / 20.0, 1.0);
    Color bar_c = pe.pct > 5.0 ? C_RED : pe.pct > 1.0 ? C_ORANGE : C_GREEN;
    drawBar(r, DETAIL_X + 220, ry + 2, 240, 14, bar_frac, C_BAR_BG, bar_c);
    renderText(r, app->font_sm, pct_buf, bar_c, DETAIL_X + 468, ry);
    ry += 26;

    // Command line
    if (!pe.cmdline.empty()) {
        renderText(r, app->font_sm, "Command:", C_TEXT_DIM, DETAIL_X + 20, ry);
        renderText(r, app->font_sm, pe.cmdline.c_str(), C_TEXT_DIM,
                   DETAIL_X + 20, ry + 18, DETAIL_W - 40);
    }

    // Buttons
    SDL_Rect kb = killBtnRect();
    SDL_Rect cb = closeBtnRect();

    Color kill_c = app->kill_hover ? C_BTN_HOV : C_BTN_KILL;
    fillRect(r, kb.x, kb.y, kb.w, kb.h, kill_c);
    drawRect(r, kb.x, kb.y, kb.w, kb.h, C_RED);
    renderText(r, app->font_sm, "Kill (SIGTERM)",
               C_TEXT, kb.x + 18, kb.y + (BTN_H - 13) / 2);

    Color close_c = app->close_hover ? C_ROW_HOV : C_BTN_CLOSE;
    fillRect(r, cb.x, cb.y, cb.w, cb.h, close_c);
    drawRect(r, cb.x, cb.y, cb.w, cb.h, C_DIVIDER);
    renderText(r, app->font_sm, "Close",
               C_TEXT, cb.x + (BTN_W - 36) / 2, cb.y + (BTN_H - 13) / 2);

    // Kill status message (shown between buttons)
    if (!app->kill_msg.empty()) {
        renderText(r, app->font_sm, app->kill_msg.c_str(), C_ORANGE,
                   DETAIL_X + 20, kb.y + (BTN_H - 13) / 2);
    }
}

static void renderFooter(SDL_Renderer *r, AppData *app) {
    const char *hint =
        "Scroll: mouse wheel   |   Sort: click column   |   Click row: details   |   ESC: close panel";
    renderText(r, app->font_sm, hint, C_TEXT_DIM, 10, WIN_H - 18);
}

static void render(SDL_Renderer *r, AppData *app) {
    fillRect(r, 0, 0, WIN_W, WIN_H, C_BG);
    renderHeader(r, app);
    renderColumnHeaders(r, app);
    renderProcessList(r, app);
    renderFooter(r, app);
    renderDetailPanel(r, app);   // drawn last, on top of everything
    SDL_RenderPresent(r);
}

// ─── Event handling ───────────────────────────────────────────────────────────
static SortCol colAtX(int x) {
    if (x < COL_NAME_X) return SortCol::PID;
    if (x < COL_RSS_X)  return SortCol::NAME;
    if (x < COL_VSZ_X)  return SortCol::RSS;
    if (x < COL_PCT_X)  return SortCol::VSZ;
    return SortCol::PCT;
}

static void handleEvent(SDL_Event *ev, AppData *app, bool &running) {
    if (ev->type == SDL_QUIT) { running = false; return; }

    if (ev->type == SDL_KEYDOWN) {
        if (ev->key.keysym.sym == SDLK_q && !app->detail_open)
            { running = false; return; }
        if (ev->key.keysym.sym == SDLK_ESCAPE)
            { app->detail_open = false; app->kill_msg = ""; return; }
    }

    if (ev->type == SDL_MOUSEMOTION) {
        int mx = ev->motion.x, my = ev->motion.y;
        if (app->detail_open) {
            app->kill_hover  = pointInRect(mx, my, killBtnRect());
            app->close_hover = pointInRect(mx, my, closeBtnRect());
        } else {
            app->hover_row = -1;
            if (my >= LIST_Y && my < WIN_H && mx < LIST_W) {
                int row = (my - LIST_Y + app->scroll_offset) / ROW_H;
                if (row >= 0 && row < (int)app->processes.size())
                    app->hover_row = row;
            }
        }
    }

    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        int mx = ev->button.x, my = ev->button.y;

        if (app->detail_open) {
            if (pointInRect(mx, my, killBtnRect())) {
                // Send SIGTERM to the selected process
                if (app->detail_idx >= 0 && app->detail_idx < (int)app->processes.size()) {
                    pid_t target = app->processes[app->detail_idx].pid;
                    if (kill(target, SIGTERM) == 0)
                        app->kill_msg = "SIGTERM sent. Process stopping...";
                    else
                        app->kill_msg = "Failed (permission denied?).";
                }
                return;
            }
            // Close button or click outside modal closes the panel
            if (pointInRect(mx, my, closeBtnRect()) ||
                mx < DETAIL_X || mx > DETAIL_X + DETAIL_W ||
                my < DETAIL_Y || my > DETAIL_Y + DETAIL_H) {
                app->detail_open = false;
                app->kill_msg    = "";
            }
            return;
        }

        // Column header → sort
        if (my >= HEADER_H && my < HEADER_H + COL_HDR_H) {
            SortCol clicked = colAtX(mx);
            if (clicked == app->sort_col)
                app->sort_asc = !app->sort_asc;
            else {
                app->sort_col = clicked;
                app->sort_asc = (clicked == SortCol::NAME);
            }
            sortProcesses(app);
            return;
        }

        // Process row → open detail panel
        if (my >= LIST_Y && my < WIN_H && mx < LIST_W) {
            int row = (my - LIST_Y + app->scroll_offset) / ROW_H;
            if (row >= 0 && row < (int)app->processes.size()) {
                app->detail_idx  = row;
                app->detail_open = true;
                app->kill_msg    = "";
            }
        }
    }

    if (ev->type == SDL_MOUSEWHEEL && !app->detail_open) {
        int mx, my; SDL_GetMouseState(&mx, &my);
        if (my >= LIST_Y) {
            app->scroll_offset -= ev->wheel.y * ROW_H * 3;
            int max_scroll = std::max(0, (int)app->processes.size() * ROW_H - LIST_H);
            app->scroll_offset = std::clamp(app->scroll_offset, 0, max_scroll);
        }
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int /*argc*/, char * /*argv*/[]) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    TTF_Init();

    SDL_Window *window = SDL_CreateWindow(
        "Memory Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    const char *font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        nullptr
    };

    AppData app;
    for (int i = 0; font_paths[i]; i++) {
        app.font_sm  = TTF_OpenFont(font_paths[i], 13);
        app.font_med = TTF_OpenFont(font_paths[i], 17);
        app.font_lg  = TTF_OpenFont(font_paths[i], 24);
        if (app.font_sm && app.font_med && app.font_lg) break;
        if (app.font_sm)  { TTF_CloseFont(app.font_sm);  app.font_sm  = nullptr; }
        if (app.font_med) { TTF_CloseFont(app.font_med); app.font_med = nullptr; }
        if (app.font_lg)  { TTF_CloseFont(app.font_lg);  app.font_lg  = nullptr; }
    }
    if (!app.font_sm) {
        std::cerr << "ERROR: No font found. Try: sudo apt install fonts-dejavu-core\n";
        return 1;
    }

    // Force immediate first data load
    app.last_update = std::chrono::steady_clock::now() -
                      std::chrono::milliseconds(AppData::UPDATE_MS + 1);

    bool running = true;
    SDL_Event ev;

    while (running) {
        // Timed data refresh (every 1 second)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - app.last_update).count();
        if (elapsed >= AppData::UPDATE_MS) {
            refreshData(&app);
            app.last_update = now;
        }

        render(renderer, &app);

        // Wait up to 100ms for an event, then loop (ensures timely refresh)
        if (SDL_WaitEventTimeout(&ev, 100)) {
            handleEvent(&ev, &app, running);
            while (SDL_PollEvent(&ev))
                handleEvent(&ev, &app, running);
        }
    }

    TTF_CloseFont(app.font_sm);
    TTF_CloseFont(app.font_med);
    TTF_CloseFont(app.font_lg);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
