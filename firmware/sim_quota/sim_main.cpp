// Host-side LVGL simulator for the Quota app view.
// Renders the real quota_view.cpp against a mock Snapshot so layout can be
// iterated without flashing the device.
//
// Build:  cmake -B build && cmake --build build
// Run:    ./build/quota_sim            (opens 320x240 window, writes
//                                       /tmp/quota_sim.bmp after ~2s)

#include <lvgl.h>
#include "../main/apps/app_quota/quota_view.h"
#include "../main/apps/app_quota/quota_model.h"

// Pull in the real fetch/parse layer — static functions become visible in
// this TU. board.h/mooncake_log.h resolve to stubs/; cJSON is the real ESP-IDF
// source. Network stubs make _httpGet return "no http client" if called.
#include "../main/apps/app_quota/quota_fetch.cpp"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

static std::string slurp(const char* path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Real-data mode: parse actual API bodies captured in testdata/ through the
// real parsers, so labels/exclusion logic is verified against live payloads.
static quota::Snapshot makeRealSnapshot()
{
    quota::Snapshot snap;
    std::string err;
    std::string body;

    body = slurp("testdata/magpie.json");
    if (!body.empty()) _parseMagpie(body, snap.cards, err);

    body = slurp("testdata/cpa_main.json");
    if (!body.empty()) {
        size_t before = snap.cards.size();
        _parseCpamgr(body, snap.cards, err);
        for (size_t i = before; i < snap.cards.size(); i++) snap.cards[i].source = "cpa";
    }
    body = slurp("testdata/cpa_bf.json");
    if (!body.empty()) {
        size_t before = snap.cards.size();
        _parseCpamgr(body, snap.cards, err);
        for (size_t i = before; i < snap.cards.size(); i++) snap.cards[i].source = "cpa-bf";
    }
    return snap;
}

static quota::Snapshot makeMockSnapshot()
{
    quota::Snapshot snap;

    {
        quota::Card c;
        c.source = "magpie";
        c.name   = "OpenCode Go";
        c.user   = "pro";
        quota::Window w1; w1.name = "5 hours"; w1.percent = 62; w1.text = "62%"; w1.resetsAt = "10-29 19:00";
        quota::Window w2; w2.name = "7 days";  w2.percent = 81; w2.text = "81%"; w2.resetsAt = "11-03 08:00";
        c.windows = {w1, w2};
        snap.cards.push_back(c);
    }
    {
        quota::Card c;
        c.source = "magpie";
        c.name   = "Kimi Code (China)";
        c.user   = "standard";
        quota::Window w; w.name = "month"; w.percent = 18; w.text = "18%"; w.resetsAt = "11-01 00:00";
        c.windows = {w};
        snap.cards.push_back(c);
    }
    {
        quota::Card c;
        c.source = "cpa";
        c.name   = "90-grade.trapper@icloud.com";  // long email: must not overflow card
        c.user   = "plus";
        quota::Window w1; w1.name = "7d";    w1.percent = 79; w1.text = "79%";  w1.resetsAt = "11-05 14:30";
        quota::Window w2; w2.name = "5h";    w2.percent = 12; w2.text = "12%";  w2.resetsAt = "19:30";
        quota::Window w3; w3.name = "credits"; w3.percent = -1; w3.text = "62.5k";
        c.windows = {w1, w2, w3};
        snap.cards.push_back(c);
    }
    {
        quota::Card c;
        c.source = "magpie";
        c.name   = "MiniMax";
        c.error  = "http 401";
        snap.cards.push_back(c);
    }

    snap.fetchedAtMs = 0;
    return snap;
}

static void dumpSnapshot()
{
    lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!snap) { fprintf(stderr, "snapshot failed\n"); return; }
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        snap->data, snap->header.w, snap->header.h, 16,
        (int)snap->header.stride, SDL_PIXELFORMAT_RGB565);
    if (surf) {
        SDL_SaveBMP(surf, "/tmp/quota_sim.bmp");
        SDL_FreeSurface(surf);
        printf("[sim] wrote /tmp/quota_sim.bmp (%ux%u)\n",
               (unsigned)snap->header.w, (unsigned)snap->header.h);
    }
    lv_draw_buf_destroy(snap);
}

int main(int argc, char** argv)
{
    lv_init();

    lv_display_t* disp = lv_sdl_window_create(320, 240);
    if (!disp) { fprintf(stderr, "sdl window failed\n"); return 1; }
    lv_sdl_mouse_create();
    lv_sdl_window_set_title(disp, "Quota Sim");

    quota::QuotaView view;
    view.showLoading("loading quotas…");
    view.setStatusText("connecting to ASUS-88U");

    // --real: parse testdata/*.json through the real parsers; else mock data
    bool real_mode = argc > 1 && strcmp(argv[1], "--real") == 0;
    quota::Snapshot snap = real_mode ? makeRealSnapshot() : makeMockSnapshot();
    printf("[sim] %s: %zu cards\n", real_mode ? "real data" : "mock data", snap.cards.size());
    for (const auto& c : snap.cards) {
        printf("  [%s] %s (%s) wins=%zu err=%s\n", c.source.c_str(), c.name.c_str(),
               c.user.c_str(), c.windows.size(), c.error.c_str());
        for (const auto& w : c.windows)
            printf("      %-10s %-8s reset=%s\n", w.name.c_str(), w.text.c_str(), w.resetsAt.c_str());
    }
    int frames = 0;
    bool applied = false, dumped = false;

    // any arg = headless: dump the bitmap and exit
    bool shot_mode = argc > 1;

    uint32_t last = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
        }
        uint32_t now = SDL_GetTicks();
        lv_tick_inc(now - last);
        last = now;
        lv_timer_handler();
        frames++;

        if (!applied && frames == 30) {
            view.showSnapshot(snap);
            view.setStatusText("updated 14:40");
            // --detail N: open card N's detail overlay right before the shot
            for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--detail") == 0 && i + 1 < argc) {
                    int n = atoi(argv[i + 1]);
                    if (n >= 0 && (size_t)n < snap.cards.size())
                        view.showCardDetail(snap.cards[n]);
                }
            }
            applied = true;
        }
        if (!dumped && frames == 120) {
            dumpSnapshot();
            dumped = true;
            if (shot_mode) running = false;
        }
        SDL_Delay(5);
    }
    return 0;
}
