/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "quota_fetch.h"
#include <board.h>
#include <cJSON.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include <ctime>

#ifndef CONFIG_QUOTA_MAGPIE_URL
#define CONFIG_QUOTA_MAGPIE_URL "http://192.168.6.3:3430"
#endif
#ifndef CONFIG_QUOTA_MAGPIE_KEY
#define CONFIG_QUOTA_MAGPIE_KEY ""
#endif
#ifndef CONFIG_QUOTA_CPAMGR_URL
#define CONFIG_QUOTA_CPAMGR_URL ""
#endif
#ifndef CONFIG_QUOTA_CPAMGR_KEY
#define CONFIG_QUOTA_CPAMGR_KEY ""
#endif
#ifndef CONFIG_QUOTA_CPAMGR_BF_PATH
#define CONFIG_QUOTA_CPAMGR_BF_PATH ""
#endif
#ifndef CONFIG_QUOTA_MAGPIE_EXCLUDE
#define CONFIG_QUOTA_MAGPIE_EXCLUDE ""
#endif

static const std::string_view _tag = "QuotaFetch";

namespace {

static std::string _jstr(cJSON* obj, const char* key)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static double _jnum(cJSON* obj, const char* key, double fallback = -1)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

// "2026-11-02T08:00:05Z" -> "11-02 08:00"
static std::string _shortTime(const std::string& iso)
{
    if (iso.size() >= 16 && iso[4] == '-' && iso[7] == '-') {
        return iso.substr(5, 5) + " " + iso.substr(11, 5);
    }
    return iso;
}

// epoch seconds -> "MM-DD HH:MM" local time; empty on failure
static std::string _epochShort(const std::string& epoch_str)
{
    if (epoch_str.empty()) return "";
    time_t t = (time_t)strtoll(epoch_str.c_str(), nullptr, 10);
    if (t <= 0) return "";
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    strftime(buf, sizeof(buf), "%m-%d %H:%M", &tmv);
    return buf;
}

// HTTP GET. Returns body, or empty string + out_err set on failure.
// extra headers are sent verbatim (auth cookies / bearer / CF service token).
static std::string _httpGet(const std::string& url,
                            const std::vector<std::pair<const char*, const char*>>& headers,
                            std::string& out_err, int timeout_ms = 12000)
{
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        out_err = "no http client";
        return "";
    }
    http->SetHeader("User-Agent",
                    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 "
                    "Safari/537.36 Edg/154.0.0.0");
    http->SetHeader("Accept", "application/json");
    for (auto& h : headers) {
        if (h.first && h.first[0] != '\0' && h.second && h.second[0] != '\0') {
            http->SetHeader(h.first, h.second);
        }
    }
    // Bound connect+read: a stalled CF stream must not hold the UI hostage
    http->SetTimeout(timeout_ms);
    if (!http->Open("GET", url)) {
        out_err = "connect failed";
        http->Close();
        return "";
    }
    int status = http->GetStatusCode();
    // Stream-drain with Read(): ml307 HttpClient applies 8KB backpressure in its
    // TCP callback (MAX_BODY_CHUNKS_SIZE) while holding mutex_, and ReadAll()
    // blocks on that same mutex before its timed wait -> permanent deadlock on
    // bodies >8KB. Read() releases chunks as it goes, so flow control clears.
    std::string body;
    char buf[2048];
    bool read_err = false;
    while (true) {
        int n = http->Read(buf, sizeof(buf));
        if (n < 0) { read_err = true; break; }
        if (n == 0) break;
        body.append(buf, static_cast<size_t>(n));
    }
    http->Close();
    if (read_err) {
        out_err = "read error";
        return "";
    }
    if (status != 200) {
        out_err = "http " + std::to_string(status);
        return "";
    }
    if (body.empty()) {
        out_err = "empty body";
        return "";
    }
    return body;
}

// Find the first JSON array anywhere directly under root or one level deep
// under common container keys.
static cJSON* _findArray(cJSON* root)
{
    if (cJSON_IsArray(root)) return root;
    static const char* keys[] = {"quotas", "cards", "data", "items", "snapshots", "list", "results"};
    for (auto* k : keys) {
        cJSON* v = cJSON_GetObjectItemCaseSensitive(root, k);
        if (cJSON_IsArray(v)) return v;
        if (cJSON_IsObject(v)) {
            for (auto* k2 : keys) {
                cJSON* v2 = cJSON_GetObjectItemCaseSensitive(v, k2);
                if (cJSON_IsArray(v2)) return v2;
            }
        }
    }
    return nullptr;
}

/* ----------------------- magpie panel /api/usage/quotas ----------------------- */
// [{"provider","name","icon","plan","until","error","seeAt",
//   "windows":[{"name":"5 hours","used":<percent>,"resetsAt","startsAt"}]}]

// case-insensitive substring match
static bool _icontains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != hay.end();
}

// CONFIG_QUOTA_MAGPIE_EXCLUDE: comma-separated substrings matched against
// the provider display name, e.g. "OpenCode Zen, MiniMax"
static bool _excluded(const std::string& name)
{
    std::string cfg = CONFIG_QUOTA_MAGPIE_EXCLUDE;
    size_t pos = 0;
    while (pos <= cfg.size()) {
        size_t comma = cfg.find(',', pos);
        std::string tok = cfg.substr(pos, comma == std::string::npos ? comma : comma - pos);
        size_t b = tok.find_first_not_of(" \t"), e = tok.find_last_not_of(" \t");
        if (b != std::string::npos && _icontains(name, tok.substr(b, e - b + 1))) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

static void _parseMagpie(const std::string& body, std::vector<quota::Card>& out, std::string& err)
{
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        err = "bad json";
        return;
    }
    cJSON* arr = cJSON_IsArray(root) ? root : _findArray(root);
    if (!cJSON_IsArray(arr)) {
        err = "no quota array";
        cJSON_Delete(root);
        return;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr)
    {
        std::string cname = _jstr(item, "name");
        if (cname.empty()) cname = _jstr(item, "provider");
        if (_excluded(cname)) continue;

        quota::Card card;
        card.source = "magpie";
        card.name   = cname;
        card.error  = _jstr(item, "error");
        std::string plan = _jstr(item, "plan");
        if (!plan.empty()) card.user = plan;

        cJSON* wins = cJSON_GetObjectItemCaseSensitive(item, "windows");
        if (cJSON_IsArray(wins)) {
            cJSON* w = nullptr;
            cJSON_ArrayForEach(w, wins)
            {
                quota::Window win;
                win.name      = _jstr(w, "name");
                double used   = _jnum(w, "used");
                win.unlimited = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "unlimited"));
                std::string disp = _jstr(w, "display");
                std::string rst  = _jstr(w, "resetsAt");

                if (win.unlimited) {
                    win.text = "unlimited";
                } else if (used >= 0) {
                    // panel reports percent USED; show remaining
                    win.percent = (float)(100.0 - used);
                    if (win.percent < 0) win.percent = 0;
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%.0f%%", win.percent);
                    win.text = disp.empty() ? buf : disp;
                }
                win.resetsAt = _shortTime(rst);
                card.windows.push_back(std::move(win));
            }
        }
        out.push_back(std::move(card));
    }
    cJSON_Delete(root);
}

/* ------------------- cpamgr /v0/management/auth-files ------------------- */
// {"files":[{"provider":"codex","account","label","status","quota":{
//   "observed_at","signals":{"X-Codex-Primary-Used-Percent":"21", ...}}}]}

static std::string _sig(cJSON* signals, const char* key)
{
    if (!cJSON_IsObject(signals)) return "";
    return _jstr(signals, key);
}

// Derive a window label from minutes: 10080->"1w", 1440->"1d", 300->"5h"
static std::string _winName(const std::string& fallback, const std::string& minutes_str)
{
    long m = strtol(minutes_str.c_str(), nullptr, 10);
    if (m <= 0) return fallback;
    char buf[16];
    if (m % 10080 == 0) snprintf(buf, sizeof(buf), "%ldw", m / 10080);
    else if (m % 1440 == 0) snprintf(buf, sizeof(buf), "%ldd", m / 1440);
    else if (m % 60 == 0) snprintf(buf, sizeof(buf), "%ldh", m / 60);
    else snprintf(buf, sizeof(buf), "%ldm", m);
    return buf;
}

static void _cpamgrWindow(cJSON* signals, const char* prefix, const char* fallback,
                          quota::Card& card)
{
    std::string used = _sig(signals, (std::string(prefix) + "-Used-Percent").c_str());
    if (used.empty()) return;
    // Window-Minutes "0" = the window doesn't apply to this plan
    // (team/pro accounts have no secondary 5h window)
    std::string minutes = _sig(signals, (std::string(prefix) + "-Window-Minutes").c_str());
    if (!minutes.empty() && strtol(minutes.c_str(), nullptr, 10) <= 0) return;
    quota::Window win;
    win.name = _winName(fallback, minutes);
    double u = strtod(used.c_str(), nullptr);
    win.percent = (float)(100.0 - u);
    if (win.percent < 0) win.percent = 0;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f%%", win.percent);
    win.text = buf;
    std::string reset = _sig(signals, (std::string(prefix) + "-Reset-At").c_str());
    win.resetsAt = _epochShort(reset);
    card.windows.push_back(std::move(win));
}

static void _parseCpamgr(const std::string& body, std::vector<quota::Card>& out, std::string& err)
{
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        err = "bad json";
        return;
    }
    cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
    if ((cJSON_IsString(e) && e->valuestring && e->valuestring[0]) || cJSON_IsObject(e)) {
        err = cJSON_IsObject(e) ? _jstr(e, "message") : e->valuestring;
        if (err.empty()) err = "api error";
        cJSON_Delete(root);
        return;
    }
    cJSON* files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (!cJSON_IsArray(files)) {
        err = "no files array";
        cJSON_Delete(root);
        return;
    }
    cJSON* f = nullptr;
    cJSON_ArrayForEach(f, files)
    {
        cJSON* quota   = cJSON_GetObjectItemCaseSensitive(f, "quota");
        cJSON* signals = cJSON_GetObjectItemCaseSensitive(quota, "signals");
        if (!cJSON_IsObject(signals) || signals->child == nullptr) continue;  // no quota info

        quota::Card card;
        card.source = "cpamgr";
        card.name   = _jstr(f, "account");
        if (card.name.empty()) card.name = _jstr(f, "label");
        std::string plan = _sig(signals, "X-Codex-Plan-Type");
        if (plan.empty()) plan = _jstr(f, "provider");
        card.user = plan;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "disabled")) ||
            _jstr(f, "status") == "disabled") {
            card.error = "disabled";
        }

        // codex windows: primary = weekly (7d), secondary = 5h session
        _cpamgrWindow(signals, "X-Codex-Primary", "1w", card);
        _cpamgrWindow(signals, "X-Codex-Secondary", "5h", card);

        std::string bal = _sig(signals, "X-Codex-Credits-Balance");
        if (!bal.empty()) {
            quota::Window win;
            win.name = "credits";
            double b = strtod(bal.c_str(), nullptr);
            char buf[32];
            if (b >= 1000) snprintf(buf, sizeof(buf), "%.1fk", b / 1000.0);
            else snprintf(buf, sizeof(buf), "%.0f", b);
            win.text = buf;
            card.windows.push_back(std::move(win));
        }
        if (card.windows.empty() && card.error.empty()) card.error = "no quota";
        out.push_back(std::move(card));
    }
    if (out.empty() && err.empty()) err = "no accounts";
    cJSON_Delete(root);
}

static void _fetchSource(const char* url, const char* path,
                         const std::vector<std::pair<const char*, const char*>>& headers,
                         void (*parse)(const std::string&, std::vector<quota::Card>&, std::string&),
                         quota::Snapshot& snap, const char* source_tag = nullptr,
                         int timeout_ms = 12000)
{
    std::string err;
    std::string body = _httpGet(std::string(url) + path, headers, err, timeout_ms);
    if (!err.empty()) {
        if (snap.error.empty()) snap.error = err;
        mclog::tagWarn(_tag, "fetch {}{} failed: {}", url, path, err);
        return;
    }
    size_t before = snap.cards.size();
    parse(body, snap.cards, err);
    if (source_tag) {
        for (size_t i = before; i < snap.cards.size(); i++) snap.cards[i].source = source_tag;
    }
    if (!err.empty()) {
        if (snap.error.empty()) snap.error = err;
        mclog::tagWarn(_tag, "parse {}{} failed: {}", url, path, err);
    }
}

}  // namespace

quota::Snapshot quota::fetchAll(std::function<void(const Snapshot&)> on_progress)
{
    Snapshot snap;
    if (CONFIG_QUOTA_MAGPIE_URL[0] != '\0') {
        // panel auth is cookie-based: magpie_web_<port>=<key>
        _fetchSource(CONFIG_QUOTA_MAGPIE_URL, "/api/usage/quotas?asked=1",
                     {{"Cookie", "magpie_web_3430=" CONFIG_QUOTA_MAGPIE_KEY}},
                     _parseMagpie, snap);
        if (on_progress) on_progress(snap);
    }
    if (CONFIG_QUOTA_CPAMGR_URL[0] != '\0') {
        // Cloudflare edge blocks ESP-TLS fingerprints — the cpamgr URL points
        // at the LAN nginx relay which injects auth server-side, so the
        // device just needs a plain HTTP GET. /quota and /quota-bf are two
        // separate cpamgr instances behind the same relay host.
        _fetchSource(CONFIG_QUOTA_CPAMGR_URL, "/quota", {}, _parseCpamgr, snap, "cpa", 30000);
        if (on_progress) on_progress(snap);
        if (CONFIG_QUOTA_CPAMGR_BF_PATH[0] != '\0') {
            _fetchSource(CONFIG_QUOTA_CPAMGR_URL, CONFIG_QUOTA_CPAMGR_BF_PATH, {},
                         _parseCpamgr, snap, "cpa-bf", 30000);
            if (on_progress) on_progress(snap);
        }
        if (snap.cards.empty() && !snap.error.empty()) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            snap.error.clear();
            _fetchSource(CONFIG_QUOTA_CPAMGR_URL, "/quota", {}, _parseCpamgr, snap, "cpa", 30000);
            if (on_progress) on_progress(snap);
        }
    }
    if (snap.cards.empty() && snap.error.empty()) {
        snap.error = "no source configured";
    }
    return snap;
}
