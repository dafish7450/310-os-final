/*
 * memview.cpp — SDL2 Memory Viewer
 *
 * Displays per-process memory usage from /proc, with live updates,
 * sortable columns, and a scrollable list. Also shows a system-wide
 * RAM bar and swap bar at the top.
 *
 * Build:
 *   g++ -std=c++17 memview.cpp -o memview \
 *       $(sdl2-config --cflags --libs) -lSDL2_ttf -lSDL2_image
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

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// ─── Window dimensions ────────────────────────────────────────────────────────
static constexpr int WIN_W = 900;
static constexpr int WIN_H = 680;

// ─── Layout constants ─────────────────────────────────────────────────────────
static constexpr int HEADER_H      = 160;   // top panel (RAM/SWAP bars + title)
static constexpr int COL_HDR_H     = 36;    // column header row
static constexpr int ROW_H         = 28;    // per-process row height
static constexpr int SCROLLBAR_W   = 12;
static constexpr int LIST_Y        = HEADER_H + COL_HDR_H;
static constexpr int LIST_H        = WIN_H - LIST_Y;
static constexpr int LIST_W        = WIN_W - SCROLLBAR_W;

// Column X positions
static constexpr int COL_PID_X     = 10;
static constexpr int COL_NAME_X    = 90;
static constexpr int COL_RSS_X     = 520;
static constexpr int COL_VSZ_X     = 650;
static constexpr int COL_PCT_X     = 780;

// ─── Color palette ────────────────────────────────────────────────────────────
struct Color { uint8_t r, g, b, a; };
static constexpr Color C_BG        = {  15,  17,  22, 255 };
static constexpr Color C_PANEL     = {  22,  26,  35, 255 };
static constexpr Color C_HEADER_BG = {  18,  21,  30, 255 };
static constexpr Color C_ROW_ODD   = {  20,  24,  33, 255 };
static constexpr Color C_ROW_EVEN  = {  25,  30,  42, 255 };
static constexpr Color C_ROW_HOV   = {  35,  45,  68, 255 };
static constexpr Color C_ACCENT    = {  64, 196, 255, 255 };   // cyan
static constexpr Color C_GREEN     = {  80, 220, 140, 255 };
static constexpr Color C_ORANGE    = { 255, 165,  60, 255 };
static constexpr Color C_RED       = { 255,  80,  80, 255 };
static constexpr Color C_TEXT      = { 210, 220, 235, 255 };
static constexpr Color C_TEXT_DIM  = { 120, 135, 160, 255 };
static constexpr Color C_SORT_IND  = { 255, 210,  60, 255 };
static constexpr Color C_SCROLLBAR = {  50,  60,  85, 255 };
static constexpr Color C_SCROLLTHM = {  90, 110, 160, 255 };
static constexpr Color C_DIVIDER   = {  40,  48,  68, 255 };
static constexpr Color C_RAM_BAR   = {  64, 196, 255, 255 };
static constexpr Color C_SWAP_BAR  = { 255, 160,  60, 255 };
static constexpr Color C_BAR_BG    = {  35,  42,  60, 255 };

// ─── Data structures ──────────────────────────────────────────────────────────
struct ProcessEntry {
    pid_t       pid;
    std::string name;
    uint64_t    rss_kb;   // Resident Set Size
    uint64_t    vsz_kb;   // Virtual memory size
    double      pct;      // rss as % of total RAM
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
    TTF_Font *font_sm  = nullptr;   // 14 pt
    TTF_Font *font_med = nullptr;   // 18 pt
    TTF_Font *font_lg  = nullptr;   // 26 pt

    SysMemInfo          sysinfo{};
    std::vector<ProcessEntry> processes;

    SortCol sort_col      = SortCol::RSS;
    bool    sort_asc      = false;
    int     scroll_offset = 0;      // in pixels
    int     hover_row     = -1;

    // Update timer
    std::chrono::steady_clock::time_point last_update;
    static constexpr int UPDATE_MS = 1500;
};

// ─── Helpers ──────────────────────────────────────────────────────────────────
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

// Render text at (x,y), returning the width of the texture
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
    SDL_Rect dst{x, y, tw, th};
    // Clip to max_w via source rect
    SDL_Rect src{0, 0, tw, th};
    SDL_RenderCopy(r, tex, &src, &dst);
    SDL_DestroyTexture(tex);
    return tw;
}

// Draw a rounded-ish bar (using filled rects, no rounded corner extension)
static void drawBar(SDL_Renderer *r, int x, int y, int w, int h,
                    double fraction, Color bg, Color fg) {
    fillRect(r, x, y, w, h, bg);
    int filled = static_cast<int>(std::clamp(fraction, 0.0, 1.0) * w);
    if (filled > 0) fillRect(r, x, y, filled, h, fg);
    drawRect(r, x, y, w, h, C_DIVIDER);
}

static std::string humanKB(uint64_t kb) {
    char buf[32];
    if (kb >= 1024*1024)
        snprintf(buf, sizeof(buf), "%.1f GB", kb / (1024.0*1024.0));
    else if (kb >= 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", kb / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)kb);
    return buf;
}

// ─── /proc parsing ────────────────────────────────────────────────────────────
static bool readSysMemInfo(SysMemInfo &info) {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        uint64_t val;
        if (sscanf(line.c_str(), "MemTotal: %llu kB", (unsigned long long*)&val) == 1)
            info.total_kb = val;
        else if (sscanf(line.c_str(), "MemFree: %llu kB", (unsigned long long*)&val) == 1)
            info.free_kb = val;
        else if (sscanf(line.c_str(), "MemAvailable: %llu kB", (unsigned long long*)&val) == 1)
            info.available_kb = val;
        else if (sscanf(line.c_str(), "SwapTotal: %llu kB", (unsigned long long*)&val) == 1)
            info.swap_total_kb = val;
        else if (sscanf(line.c_str(), "SwapFree: %llu kB", (unsigned long long*)&val) == 1)
            info.swap_free_kb = val;
    }
    return true;
}

static std::string readProcessName(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    std::ifstream f(path);
    std::string name;
    if (f.is_open()) std::getline(f, name);
    if (name.empty()) name = "?";
    return name;
}

static bool readProcessMem(pid_t pid, uint64_t &rss_kb, uint64_t &vsz_kb) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    rss_kb = vsz_kb = 0;
    while (std::getline(f, line)) {
        uint64_t v;
        if (sscanf(line.c_str(), "VmRSS: %llu kB", (unsigned long long*)&v) == 1)
            rss_kb = v;
        else if (sscanf(line.c_str(), "VmSize: %llu kB", (unsigned long long*)&v) == 1)
            vsz_kb = v;
    }
    return true;
}

static void gatherProcesses(AppData *app) {
    app->processes.clear();
    for (auto &entry : std::filesystem::directory_iterator("/proc")) {
        const std::string &fname = entry.path().filename().string();
        // Only numeric directories (PIDs)
        if (!std::all_of(fname.begin(), fname.end(), ::isdigit)) continue;
        pid_t pid = std::stoi(fname);
        uint64_t rss = 0, vsz = 0;
        if (!readProcessMem(pid, rss, vsz)) continue;
        ProcessEntry pe;
        pe.pid    = pid;
        pe.name   = readProcessName(pid);
        pe.rss_kb = rss;
        pe.vsz_kb = vsz;
        pe.pct    = (app->sysinfo.total_kb > 0)
                    ? 100.0 * rss / app->sysinfo.total_kb
                    : 0.0;
        app->processes.push_back(pe);
    }
}

static void sortProcesses(AppData *app) {
    auto &v = app->processes;
    auto asc = app->sort_asc;
    switch (app->sort_col) {
        case SortCol::PID:
            std::sort(v.begin(), v.end(), [asc](auto &a, auto &b){
                return asc ? a.pid < b.pid : a.pid > b.pid; });
            break;
        case SortCol::NAME:
            std::sort(v.begin(), v.end(), [asc](auto &a, auto &b){
                return asc ? a.name < b.name : a.name > b.name; });
            break;
        case SortCol::RSS:
            std::sort(v.begin(), v.end(), [asc](auto &a, auto &b){
                return asc ? a.rss_kb < b.rss_kb : a.rss_kb > b.rss_kb; });
            break;
        case SortCol::VSZ:
            std::sort(v.begin(), v.end(), [asc](auto &a, auto &b){
                return asc ? a.vsz_kb < b.vsz_kb : a.vsz_kb > b.vsz_kb; });
            break;
        case SortCol::PCT:
            std::sort(v.begin(), v.end(), [asc](auto &a, auto &b){
                return asc ? a.pct < b.pct : a.pct > b.pct; });
            break;
    }
}

static void refreshData(AppData *app) {
    readSysMemInfo(app->sysinfo);
    gatherProcesses(app);
    sortProcesses(app);
}

// ─── Rendering ────────────────────────────────────────────────────────────────
static void renderHeader(SDL_Renderer *r, AppData *app) {
    // Background panel
    fillRect(r, 0, 0, WIN_W, HEADER_H, C_HEADER_BG);

    // Title
    renderText(r, app->font_lg, "Memory Viewer", C_ACCENT, 18, 14);

    // Subtitle / live stats
    uint64_t used = app->sysinfo.total_kb - app->sysinfo.available_kb;
    char stats[128];
    snprintf(stats, sizeof(stats), "RAM: %s used / %s total   |   Processes: %d",
             humanKB(used).c_str(),
             humanKB(app->sysinfo.total_kb).c_str(),
             (int)app->processes.size());
    renderText(r, app->font_sm, stats, C_TEXT_DIM, 18, 50);

    // ── RAM bar ──
    renderText(r, app->font_sm, "RAM", C_TEXT, 18, 80);
    double ram_frac = app->sysinfo.total_kb > 0
        ? (double)used / app->sysinfo.total_kb : 0.0;
    // Color shifts from green → orange → red based on usage
    Color ram_color = ram_frac < 0.6 ? C_GREEN
                    : ram_frac < 0.85 ? C_ORANGE
                    : C_RED;
    drawBar(r, 70, 82, WIN_W - 200, 18, ram_frac, C_BAR_BG, ram_color);
    char ram_pct[16];
    snprintf(ram_pct, sizeof(ram_pct), "%.1f%%", ram_frac * 100.0);
    renderText(r, app->font_sm, ram_pct, C_TEXT, WIN_W - 120, 80);
    renderText(r, app->font_sm, humanKB(used).c_str(), C_TEXT_DIM, WIN_W - 190, 80);

    // ── SWAP bar ──
    renderText(r, app->font_sm, "SWAP", C_TEXT, 18, 112);
    uint64_t swap_used = app->sysinfo.swap_total_kb - app->sysinfo.swap_free_kb;
    double swap_frac = app->sysinfo.swap_total_kb > 0
        ? (double)swap_used / app->sysinfo.swap_total_kb : 0.0;
    drawBar(r, 70, 114, WIN_W - 200, 18, swap_frac, C_BAR_BG, C_SWAP_BAR);
    char swap_pct[16];
    if (app->sysinfo.swap_total_kb > 0)
        snprintf(swap_pct, sizeof(swap_pct), "%.1f%%", swap_frac * 100.0);
    else
        snprintf(swap_pct, sizeof(swap_pct), "N/A");
    renderText(r, app->font_sm, swap_pct, C_TEXT, WIN_W - 120, 112);
    renderText(r, app->font_sm, humanKB(swap_used).c_str(), C_TEXT_DIM, WIN_W - 190, 112);

    // Bottom divider
    drawHLine(r, 0, WIN_W, HEADER_H - 1, C_ACCENT);
}

// Sort indicator arrow
static void renderArrow(SDL_Renderer *r, int cx, int cy, bool ascending) {
    setColor(r, C_SORT_IND);
    if (ascending) {
        // Up arrow
        SDL_RenderDrawLine(r, cx, cy-5, cx-4, cy+3);
        SDL_RenderDrawLine(r, cx, cy-5, cx+4, cy+3);
        SDL_RenderDrawLine(r, cx-4, cy+3, cx+4, cy+3);
    } else {
        // Down arrow
        SDL_RenderDrawLine(r, cx, cy+5, cx-4, cy-3);
        SDL_RenderDrawLine(r, cx, cy+5, cx+4, cy-3);
        SDL_RenderDrawLine(r, cx-4, cy-3, cx+4, cy-3);
    }
}

static void renderColumnHeaders(SDL_Renderer *r, AppData *app) {
    fillRect(r, 0, HEADER_H, WIN_W, COL_HDR_H, C_PANEL);

    struct ColDef { const char *label; int x; SortCol col; };
    ColDef cols[] = {
        {"PID",     COL_PID_X,  SortCol::PID},
        {"NAME",    COL_NAME_X, SortCol::NAME},
        {"RSS",     COL_RSS_X,  SortCol::RSS},
        {"VIRT",    COL_VSZ_X,  SortCol::VSZ},
        {"%MEM",    COL_PCT_X,  SortCol::PCT},
    };
    int cy = HEADER_H + COL_HDR_H / 2;
    for (auto &col : cols) {
        Color tc = (app->sort_col == col.col) ? C_SORT_IND : C_TEXT_DIM;
        int tw = renderText(r, app->font_sm, col.label, tc, col.x, HEADER_H + 10);
        if (app->sort_col == col.col)
            renderArrow(r, col.x + tw + 10, cy, app->sort_asc);
    }
    drawHLine(r, 0, WIN_W, HEADER_H + COL_HDR_H - 1, C_DIVIDER);
}

// Scrollable clip region for the process list
static void renderProcessList(SDL_Renderer *r, AppData *app) {
    SDL_Rect clip{0, LIST_Y, LIST_W, LIST_H};
    SDL_RenderSetClipRect(r, &clip);

    int n = (int)app->processes.size();
    int total_content_h = n * ROW_H;
    int max_scroll = std::max(0, total_content_h - LIST_H);
    app->scroll_offset = std::clamp(app->scroll_offset, 0, max_scroll);

    // Determine which rows are visible
    int first_row = app->scroll_offset / ROW_H;
    int last_row  = std::min(n - 1, (app->scroll_offset + LIST_H) / ROW_H + 1);

    for (int i = first_row; i <= last_row; i++) {
        int row_y = LIST_Y + (i * ROW_H) - app->scroll_offset;
        Color bg = (i == app->hover_row) ? C_ROW_HOV
                 : (i % 2 == 0)          ? C_ROW_EVEN
                                         : C_ROW_ODD;
        fillRect(r, 0, row_y, LIST_W, ROW_H, bg);

        const ProcessEntry &pe = app->processes[i];
        int ty = row_y + (ROW_H - 14) / 2;   // vertically centered

        // PID
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", pe.pid);
        renderText(r, app->font_sm, buf, C_TEXT_DIM, COL_PID_X, ty);

        // Name (truncated)
        renderText(r, app->font_sm, pe.name.c_str(), C_TEXT,
                   COL_NAME_X, ty, COL_RSS_X - COL_NAME_X - 8);

        // RSS
        renderText(r, app->font_sm, humanKB(pe.rss_kb).c_str(), C_GREEN,
                   COL_RSS_X, ty);

        // VSZ
        renderText(r, app->font_sm, humanKB(pe.vsz_kb).c_str(), C_TEXT_DIM,
                   COL_VSZ_X, ty);

        // %MEM — color coded
        snprintf(buf, sizeof(buf), "%.2f%%", pe.pct);
        Color pct_color = pe.pct > 5.0 ? C_RED
                        : pe.pct > 1.0 ? C_ORANGE
                        : C_TEXT_DIM;
        renderText(r, app->font_sm, buf, pct_color, COL_PCT_X, ty);

        // Row divider
        drawHLine(r, 0, LIST_W, row_y + ROW_H - 1, C_DIVIDER);
    }

    SDL_RenderSetClipRect(r, nullptr);

    // ─── Scrollbar ───────────────────────────────────────────────────────────
    fillRect(r, LIST_W, LIST_Y, SCROLLBAR_W, LIST_H, C_SCROLLBAR);
    if (total_content_h > LIST_H) {
        double thumb_frac   = (double)LIST_H / total_content_h;
        int    thumb_h      = std::max(20, (int)(thumb_frac * LIST_H));
        double scroll_frac  = (double)app->scroll_offset / max_scroll;
        int    thumb_y      = LIST_Y + (int)(scroll_frac * (LIST_H - thumb_h));
        fillRect(r, LIST_W, thumb_y, SCROLLBAR_W, thumb_h, C_SCROLLTHM);
    }
}

static void renderFooter(SDL_Renderer *r, AppData *app) {
    // Subtle hint text at the very bottom
    const char *hint = "Scroll: mouse wheel   |   Sort: click column header   |   Updates every 1.5s";
    renderText(r, app->font_sm, hint, C_TEXT_DIM, 10, WIN_H - 18);
}

static void render(SDL_Renderer *r, AppData *app) {
    fillRect(r, 0, 0, WIN_W, WIN_H, C_BG);
    renderHeader(r, app);
    renderColumnHeaders(r, app);
    renderProcessList(r, app);
    renderFooter(r, app);
    SDL_RenderPresent(r);
}

// ─── Event handling ───────────────────────────────────────────────────────────
// Returns the column clicked for the given x coordinate in the header
static SortCol colAtX(int x) {
    if (x < COL_NAME_X) return SortCol::PID;
    if (x < COL_RSS_X)  return SortCol::NAME;
    if (x < COL_VSZ_X)  return SortCol::RSS;
    if (x < COL_PCT_X)  return SortCol::VSZ;
    return SortCol::PCT;
}

static void handleEvent(SDL_Event *ev, AppData *app, bool &running) {
    if (ev->type == SDL_QUIT) { running = false; return; }

    if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_q)
        { running = false; return; }

    if (ev->type == SDL_MOUSEMOTION) {
        int mx = ev->motion.x, my = ev->motion.y;
        if (my >= LIST_Y && my < WIN_H && mx < LIST_W) {
            int row = (my - LIST_Y + app->scroll_offset) / ROW_H;
            app->hover_row = (row < (int)app->processes.size()) ? row : -1;
        } else {
            app->hover_row = -1;
        }
    }

    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        int mx = ev->button.x, my = ev->button.y;
        // Click on column header?
        if (my >= HEADER_H && my < HEADER_H + COL_HDR_H) {
            SortCol clicked = colAtX(mx);
            if (clicked == app->sort_col)
                app->sort_asc = !app->sort_asc;
            else {
                app->sort_col = clicked;
                app->sort_asc = (clicked == SortCol::NAME);
            }
            sortProcesses(app);
        }
    }

    if (ev->type == SDL_MOUSEWHEEL) {
        // Only scroll when mouse is over the list area
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        if (my >= LIST_Y) {
            app->scroll_offset -= ev->wheel.y * ROW_H * 3;
            int n = (int)app->processes.size();
            int max_scroll = std::max(0, n * ROW_H - LIST_H);
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

    // Try to load a monospace/clean font; fall back to whatever is available
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
        // Clean up failed opens
        if (app.font_sm)  { TTF_CloseFont(app.font_sm);  app.font_sm  = nullptr; }
        if (app.font_med) { TTF_CloseFont(app.font_med); app.font_med = nullptr; }
        if (app.font_lg)  { TTF_CloseFont(app.font_lg);  app.font_lg  = nullptr; }
    }
    if (!app.font_sm) {
        std::cerr << "ERROR: Could not load any font. Install ttf-dejavu.\n";
        return 1;
    }

    app.last_update = std::chrono::steady_clock::now() -
                      std::chrono::milliseconds(AppData::UPDATE_MS + 1);

    bool running = true;
    SDL_Event ev;

    while (running) {
        // Timed refresh
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - app.last_update).count();
        if (elapsed >= AppData::UPDATE_MS) {
            refreshData(&app);
            app.last_update = now;
        }

        render(renderer, &app);

        // Poll with a short timeout so we refresh on schedule
        if (SDL_WaitEventTimeout(&ev, 100)) {
            handleEvent(&ev, &app, running);
            // Drain any remaining events
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
