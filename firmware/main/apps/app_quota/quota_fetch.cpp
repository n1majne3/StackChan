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
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <ctime>
#include <functional>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#ifndef CONFIG_QUOTA_PLANS_URL
#define CONFIG_QUOTA_PLANS_URL ""
#endif
#ifndef CONFIG_QUOTA_PLANS_CF_ID
#define CONFIG_QUOTA_PLANS_CF_ID ""
#endif
#ifndef CONFIG_QUOTA_PLANS_CF_SECRET
#define CONFIG_QUOTA_PLANS_CF_SECRET ""
#endif
#ifndef CONFIG_QUOTA_PLANS_TOKEN
#define CONFIG_QUOTA_PLANS_TOKEN ""
#endif
#ifndef CONFIG_QUOTA_HIDE
#define CONFIG_QUOTA_HIDE ""
#endif

static const std::string_view _tag = "QuotaFetch";

namespace {

static std::string _jstr(cJSON* obj, const char* key)
{
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

// Streaming HTTP GET — every received chunk goes to on_chunk; returning false
// from it aborts the read early (sets out_aborted, no error).
static bool _httpFetch(const std::string& url,
                       const std::vector<std::pair<const char*, const char*>>& headers,
                       std::string& out_err, int timeout_ms,
                       const std::function<bool(const char*, size_t)>& on_chunk,
                       bool* out_aborted = nullptr)
{
    if (out_aborted) *out_aborted = false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        out_err = "no http client";
        return false;
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
        return false;
    }
    int status = http->GetStatusCode();
    // Stream-drain with Read(): ml307 HttpClient applies 8KB backpressure in its
    // TCP callback (MAX_BODY_CHUNKS_SIZE) while holding mutex_, and ReadAll()
    // blocks on that same mutex before its timed wait -> permanent deadlock on
    // bodies >8KB. Read() releases chunks as it goes, so flow control clears.
    char buf[2048];
    bool ok = true;
    while (true) {
        int n = http->Read(buf, sizeof(buf));
        if (n < 0) { out_err = "read error"; ok = false; break; }
        if (n == 0) break;
        if (!on_chunk(buf, static_cast<size_t>(n))) {
            if (out_aborted) *out_aborted = true;
            break;
        }
    }
    http->Close();
    if (out_aborted && *out_aborted) return false;
    if (!ok) return false;
    if (status != 200) {
        out_err = "http " + std::to_string(status);
        return false;
    }
    return true;
}

// HTTP GET. Returns body, or empty string + out_err set on failure.
// Bodies are capped at 256KB — anything larger is rejected as "body too large".
// should_abort is checked per received chunk.
static std::string _httpGet(const std::string& url,
                            const std::vector<std::pair<const char*, const char*>>& headers,
                            std::string& out_err, int timeout_ms = 12000,
                            const std::function<bool()>& should_abort = nullptr)
{
    std::string body;
    bool too_large = false;
    bool aborted   = false;
    if (!_httpFetch(url, headers, out_err, timeout_ms,
                    [&](const char* d, size_t n) {
                        if (should_abort && should_abort()) {
                            aborted = true;
                            return false;
                        }
                        if (body.size() + n > 256 * 1024) {
                            too_large = true;
                            return false;
                        }
                        body.append(d, n);
                        return true;
                    })) {
        if (too_large) out_err = "body too large";
        if (aborted) out_err = "aborted";
        return "";
    }
    if (body.empty()) {
        out_err = "empty body";
        return "";
    }
    return body;
}

// case-insensitive substring match
static bool _icontains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != hay.end();
}

// CONFIG_QUOTA_HIDE: comma-separated substrings matched (case-insensitive)
// against card name, provider and id, e.g. "OpenCode Zen, MiniMax"
static bool _excluded(const std::string& name, const std::string& provider, const std::string& id)
{
    std::string cfg = CONFIG_QUOTA_HIDE;
    size_t pos = 0;
    while (pos <= cfg.size()) {
        size_t comma = cfg.find(',', pos);
        std::string tok = cfg.substr(pos, comma == std::string::npos ? comma : comma - pos);
        size_t b = tok.find_first_not_of(" \t"), e = tok.find_last_not_of(" \t");
        if (b != std::string::npos) {
            std::string needle = tok.substr(b, e - b + 1);
            if (_icontains(name, needle) || _icontains(provider, needle) || _icontains(id, needle)) {
                return true;
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

// Howard Hinnant's days_from_civil — newlib has no timegm, so epoch math is
// done by hand for the ISO-8601 UTC timestamps in the summary payload.
static long long _daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era      = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

// "2026-11-02T08:00:05.000Z" (millis optional) -> local strftime(fmt);
// "" when unparseable.
static std::string _isoToLocal(const std::string& iso, const char* fmt)
{
    int y, mo, d, h, mi, s;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return "";
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
        return "";
    }
    time_t t = (time_t)(_daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    strftime(buf, sizeof(buf), fmt, &tmv);
    return buf;
}

// Window label compaction: "5 小时"/"5h"/"5 hours" -> "5h",
// "7 天"/"7 days" -> "7d", "每周" -> "7d". A suffix after " · " is kept,
// e.g. "7 天 · Opus" -> "7d Opus". Anything else passes through.
static std::string _compactLabel(const std::string& name)
{
    std::string base = name, suffix;
    const std::string sep = " · ";  // '·' is a 2-byte UTF-8 char
    size_t dot = name.find(sep);
    if (dot != std::string::npos) {
        base   = name.substr(0, dot);
        suffix = name.substr(dot + sep.size());
    }

    std::string out;
    if (base == "每周") {
        out = "7d";
    } else {
        size_t i = 0;
        while (i < base.size() && (isdigit((unsigned char)base[i]) || base[i] == '.')) i++;
        if (i > 0) {
            std::string num  = base.substr(0, i);
            size_t u         = base.find_first_not_of(' ', i);
            std::string unit = u == std::string::npos ? "" : base.substr(u);
            if (unit == "小时" || unit == "h" || unit == "hours" || unit == "hour") {
                out = num + "h";
            } else if (unit == "天" || unit == "d" || unit == "days" || unit == "day") {
                out = num + "d";
            }
        }
    }
    if (out.empty()) out = base;
    if (!suffix.empty()) out += " " + suffix;
    return out;
}

static void _parseWindow(cJSON* w, quota::Card& card)
{
    const bool unlimited = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "unlimited"));
    cJSON* used          = cJSON_GetObjectItemCaseSensitive(w, "usedPct");
    // A null usedPct means the window doesn't apply — skip unless unlimited.
    if (!unlimited && !cJSON_IsNumber(used)) return;

    quota::Window win;
    win.name = _compactLabel(_jstr(w, "name"));
    if (unlimited) {
        win.unlimited = true;
        win.text      = "unlimited";
    } else {
        double pct = 100.0 - used->valuedouble;  // API reports used; show remaining
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        win.percent = (float)pct;
        char buf[24];
        snprintf(buf, sizeof(buf), "%.0f%%", pct);
        win.text = buf;
    }
    win.resetsAt = _isoToLocal(_jstr(w, "resetsAt"), "%m-%d %H:%M");
    card.windows.push_back(std::move(win));
}

static void _parseWindows(cJSON* item, quota::Card& card)
{
    cJSON* wins = cJSON_GetObjectItemCaseSensitive(item, "windows");
    if (!cJSON_IsArray(wins)) return;
    cJSON* w = nullptr;
    cJSON_ArrayForEach(w, wins) { _parseWindow(w, card); }
}

// codex only: {"available": int|null, "earliestExpiry": ISO|null}
static void _parseResetCredits(cJSON* item, quota::Card& card)
{
    cJSON* rc = cJSON_GetObjectItemCaseSensitive(item, "resetCredits");
    if (!cJSON_IsObject(rc)) return;
    quota::Window win;
    win.name    = "resets";
    cJSON* avail = cJSON_GetObjectItemCaseSensitive(rc, "available");
    win.text    = cJSON_IsNumber(avail) ? std::to_string((int)avail->valuedouble) : "-";
    std::string expiry = _isoToLocal(_jstr(rc, "earliestExpiry"), "%m-%d");
    if (!expiry.empty()) win.text += " · " + expiry;
    card.windows.push_back(std::move(win));
}

static void _parseCpaEntry(cJSON* item, std::vector<quota::Card>& out)
{
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "disabled"))) return;

    std::string provider = _jstr(item, "provider");
    std::string plan     = _jstr(item, "plan");
    std::string name;
    if (!plan.empty()) {
        // "ChatGPT Pro" -> "GPT Pro"
        name = plan;
        if (name.compare(0, 8, "ChatGPT ") == 0) name = "GPT " + name.substr(8);
    } else if (!provider.empty()) {
        name    = provider;
        name[0] = (char)toupper((unsigned char)name[0]);
    } else {
        name = "Account";
    }

    if (_excluded(name, provider, _jstr(item, "id"))) return;

    quota::Card card;
    card.source = "cpa";
    card.name   = name;
    card.user   = _jstr(item, "email");
    _parseWindows(item, card);
    _parseResetCredits(item, card);
    if (card.windows.empty()) return;
    out.push_back(std::move(card));
}

static void _parseMagpieEntry(cJSON* item, std::vector<quota::Card>& out)
{
    std::string provider = _jstr(item, "provider");
    std::string name     = _jstr(item, "name");
    if (name.empty()) name = provider;

    if (_excluded(name, provider, _jstr(item, "id"))) return;

    quota::Card card;
    card.source = "magpie";
    card.name   = name;
    card.user   = _jstr(item, "plan");
    _parseWindows(item, card);
    if (card.windows.empty()) return;
    out.push_back(std::move(card));
}

// First 3 alphanumeric chars of the user's local part, lowercased —
// "90-g***@icloud.com" -> "90g". Appended as "(90g)" to disambiguate
// same-name cards (montserrat_14 has no '·' glyph).
static std::string _dupTag(const std::string& user)
{
    std::string tag;
    for (char c : user.substr(0, user.find('@'))) {
        if (isalnum((unsigned char)c)) {
            tag += (char)tolower((unsigned char)c);
            if (tag.size() == 3) break;
        }
    }
    return tag;
}

static void _dedupNames(std::vector<quota::Card>::iterator first, std::vector<quota::Card>::iterator last)
{
    std::map<std::string, int> counts;
    for (auto it = first; it != last; ++it) counts[it->name]++;
    for (auto it = first; it != last; ++it) {
        if (counts[it->name] > 1) {
            std::string tag = _dupTag(it->user);
            if (!tag.empty()) it->name += " (" + tag + ")";
        }
    }
    // Still colliding (same tag): " 2", " 3", ...
    counts.clear();
    for (auto it = first; it != last; ++it) counts[it->name]++;
    std::map<std::string, int> seen;
    for (auto it = first; it != last; ++it) {
        if (counts[it->name] > 1 && ++seen[it->name] > 1) {
            it->name += " " + std::to_string(seen[it->name]);
        }
    }
}

// Lowest remaining percent first; cards with no numeric window go last.
static void _sortCards(std::vector<quota::Card>::iterator first, std::vector<quota::Card>::iterator last)
{
    auto min_remaining = [](const quota::Card& c) {
        float m = 101;  // above any real percent => sorts last
        for (auto& w : c.windows) {
            if (w.percent >= 0 && w.percent < m) m = w.percent;
        }
        return m;
    };
    std::stable_sort(first, last,
                     [&](const quota::Card& a, const quota::Card& b) { return min_remaining(a) < min_remaining(b); });
}

#ifdef ESP_PLATFORM
// cJSON nodes are ~40 bytes, below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, so a
// parsed summary would land entirely in internal RAM — the xiaozhi agent's
// scarcest resource. Point cJSON at PSRAM for the parse. The hooks are
// global, but every allocation stays free()-compatible, so a concurrent
// xiaozhi parse at worst puts its own nodes in PSRAM too.
static void* _psramMalloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

struct CjsonPsramScope {
    CjsonPsramScope()
    {
        cJSON_Hooks hooks = {_psramMalloc, free};
        cJSON_InitHooks(&hooks);
    }
    ~CjsonPsramScope()
    {
        cJSON_InitHooks(nullptr);
    }
};
#else
struct CjsonPsramScope {};
#endif

static void _parseSummary(const std::string& body, std::vector<quota::Card>& out, std::string& err)
{
    CjsonPsramScope psram;
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        err = "bad json";
        return;
    }
    const size_t before = out.size();

    cJSON* cpa = cJSON_GetObjectItemCaseSensitive(root, "cpa");
    if (cJSON_IsArray(cpa)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, cpa) { _parseCpaEntry(item, out); }
    }
    cJSON* magpie = cJSON_GetObjectItemCaseSensitive(root, "magpie");
    if (cJSON_IsArray(magpie)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, magpie) { _parseMagpieEntry(item, out); }
    }
    cJSON_Delete(root);

    _dedupNames(out.begin() + before, out.end());
    _sortCards(out.begin() + before, out.end());
    if (out.size() == before && err.empty()) err = "no accounts";
}

}  // namespace

quota::Snapshot quota::fetchAll(std::function<void(const Snapshot&)> on_progress,
                                std::function<bool()> should_abort)
{
    Snapshot snap;
    if (CONFIG_QUOTA_PLANS_URL[0] == '\0') {
        snap.error = "no source configured";
        return snap;
    }
    if (should_abort && should_abort()) return snap;

    auto fetch_once = [&]() {
        std::vector<std::pair<const char*, const char*>> headers;
        std::string bearer = std::string("Bearer ") + CONFIG_QUOTA_PLANS_TOKEN;
        if (CONFIG_QUOTA_PLANS_CF_ID[0] != '\0') headers.push_back({"CF-Access-Client-Id", CONFIG_QUOTA_PLANS_CF_ID});
        if (CONFIG_QUOTA_PLANS_CF_SECRET[0] != '\0')
            headers.push_back({"CF-Access-Client-Secret", CONFIG_QUOTA_PLANS_CF_SECRET});
        if (CONFIG_QUOTA_PLANS_TOKEN[0] != '\0') headers.push_back({"Authorization", bearer.c_str()});

        std::string err;
        std::string body = _httpGet(std::string(CONFIG_QUOTA_PLANS_URL) + "/api/v1/summary", headers, err, 20000,
                                    should_abort);
        if (err == "http 401") err = "token invalid (401)";
        else if (err == "http 403") err = "access denied (403)";
        if (err.empty()) _parseSummary(body, snap.cards, err);
        if (!err.empty() && snap.error.empty()) {
            snap.error = err;
            mclog::tagWarn(_tag, "summary fetch failed: {}", err);
        }
    };

    fetch_once();
    // One retry after a beat when nothing came back — same as before.
    if (snap.cards.empty() && !snap.error.empty() && !(should_abort && should_abort())) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        snap.error.clear();
        fetch_once();
    }
    if (on_progress) on_progress(snap);
    return snap;
}
