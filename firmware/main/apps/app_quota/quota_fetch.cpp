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
// extra headers are sent verbatim (auth cookies / bearer / CF service token).
static std::string _httpGet(const std::string& url,
                            const std::vector<std::pair<const char*, const char*>>& headers,
                            std::string& out_err, int timeout_ms = 12000)
{
    std::string body;
    if (!_httpFetch(url, headers, out_err, timeout_ms,
                    [&body](const char* d, size_t n) { body.append(d, n); return true; })) {
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
//
// The response can be ~37KB — parsing it as a cJSON DOM peaks at ~100KB heap,
// which collides with the xiaozhi agent's audio/TLS allocations. So the body is
// consumed incrementally: a slicer hands us each top-level "files" element as
// its text completes, and only that one object is DOM-parsed (~2KB).

/**
 * Incremental scanner for the "files":[ {...}, ... ] array. Feed body chunks;
 * on_file fires per complete top-level object. Also captures the "error"
 * value (string or object) for API error reporting.
 */
class CpaJsonSlicer {
public:
    std::function<void(const std::string&)> on_file;
    std::string error_text;

    void feed(const char* data, size_t len)
    {
        for (size_t i = 0; i < len; i++) {
            char c = data[i];
            if (_cap_depth > 0) {
                _feedCapture(c);
            } else if (_cap_str) {
                _feedErrStr(c);
            } else if (_in_files) {
                if (_skip_ws(c)) continue;
                if (c == '{') {
                    _cap_depth = 1;
                    _cap_str   = false;
                    _cap_esc   = false;
                    _cap.clear();
                    _cap += c;
                } else if (c == '"') {
                    // stray string element in the array — skip its text
                    _cap_str = true;
                    _cap_esc = false;
                    _cap.clear();
                } else if (c == ']') {
                    _in_files   = false;
                    _array_done = true;
                }
            } else {
                _pushTail(c);
                if (_tailEndsWith("\"files\":")) {
                    _saw_files = true;
                    // wait for '['
                } else if (_saw_files && !_in_files && !_array_done && c == '[') {
                    _in_files = true;
                } else if (_tailEndsWith("\"error\":")) {
                    _saw_error_key = true;
                } else if (_saw_error_key) {
                    if (_skip_ws(c)) continue;
                    if (c == '{') {
                        _cap_depth = 1;
                        _cap_str   = false;
                        _cap_esc   = false;
                        _cap.clear();
                        _cap += c;
                        _cap_err   = true;
                    } else if (c == '"') {
                        _cap_str = true;
                        _cap_esc = false;
                        _cap.clear();
                        _cap_err = true;
                    }
                    _saw_error_key = false;
                }
            }
        }
    }

    bool sawFiles() const { return _saw_files; }

private:
    static bool _skip_ws(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    void _pushTail(char c)
    {
        if (_skip_ws(c)) return;
        _tail += c;
        if (_tail.size() > 16) _tail.erase(0, _tail.size() - 16);
    }

    bool _tailEndsWith(const char* suffix) const
    {
        size_t n = strlen(suffix);
        return _tail.size() >= n && _tail.compare(_tail.size() - n, n, suffix) == 0;
    }

    void _feedCapture(char c)
    {
        _cap += c;
        if (_cap_str) {
            if (_cap_esc) {
                _cap_esc = false;
            } else if (c == '\\') {
                _cap_esc = true;
            } else if (c == '"') {
                _cap_str = false;
            }
        } else if (c == '"') {
            _cap_str = true;
        } else if (c == '{') {
            _cap_depth++;
        } else if (c == '}') {
            if (--_cap_depth == 0) {
                if (_cap_err) {
                    error_text = std::move(_cap);
                    _cap_err   = false;
                } else if (on_file) {
                    on_file(_cap);
                }
                _cap.clear();
            }
        }
    }

    void _feedErrStr(char c)
    {
        _cap += c;
        if (_cap_esc) {
            _cap_esc = false;
        } else if (c == '\\') {
            _cap_esc = true;
        } else if (c == '"') {
            if (_cap_err) {
                error_text = std::move(_cap);
                _cap_err   = false;
            }
            _cap_str = false;
            _cap.clear();
        }
    }

    std::string _tail;
    std::string _cap;
    int _cap_depth        = 0;
    bool _cap_str         = false;
    bool _cap_esc         = false;
    bool _cap_err         = false;
    bool _in_files        = false;
    bool _saw_files       = false;
    bool _array_done      = false;
    bool _saw_error_key   = false;
};

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

// Claude (Anthropic unified rate limit) window: Utilization is a 0..1 used
// fraction, Reset is epoch seconds. Status "rejected" = window exhausted.
static void _claudeWindow(cJSON* signals, const char* span, quota::Card& card)
{
    const std::string prefix = std::string("Anthropic-Ratelimit-Unified-") + span;
    std::string util = _sig(signals, (prefix + "-Utilization").c_str());
    if (util.empty()) return;
    quota::Window win;
    win.name    = span;
    win.percent = (float)(100.0 - strtod(util.c_str(), nullptr) * 100.0);
    if (_sig(signals, (prefix + "-Status").c_str()) == "rejected") win.percent = 0;
    if (win.percent < 0) win.percent = 0;
    if (win.percent > 100) win.percent = 100;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f%%", win.percent);
    win.text     = buf;
    win.resetsAt = _epochShort(_sig(signals, (prefix + "-Reset").c_str()));
    card.windows.push_back(std::move(win));
}

// "90-grade.trapper@icloud.com" -> "90-g***@icloud.com"
static std::string _maskEmail(const std::string& s)
{
    size_t at = s.find('@');
    if (at == std::string::npos || at == 0) return s;
    size_t keep = at > 4 ? 4 : (at > 2 ? at - 1 : 1);
    return s.substr(0, keep) + "***" + s.substr(at);
}

// Extract a Card from one parsed "files" element. Returns false when the
// entry carries no quota signals (skipped silently).
// Account tag for display: explicit note ("Go"), else the auth file name's
// trailing "-<tag>.json" ("...@icloud.com-team.json" -> "Team").
static std::string _cpamgrTag(cJSON* f)
{
    std::string tag = _jstr(f, "note");
    if (tag.empty()) {
        std::string name = _jstr(f, "name");
        const std::string ext = ".json";
        if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            name.resize(name.size() - ext.size());
            size_t dash = name.rfind('-');
            // Only short alphabetic suffixes — not email/uuid pieces.
            if (dash != std::string::npos && name.size() - dash - 1 <= 8) {
                std::string t = name.substr(dash + 1);
                if (!t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) { return isalpha(c); })) {
                    tag = t;
                }
            }
        }
    }
    if (!tag.empty()) tag[0] = (char)toupper((unsigned char)tag[0]);
    return tag;
}

static bool _cpamgrFileCard(cJSON* f, quota::Card& card)
{
    cJSON* quota   = cJSON_GetObjectItemCaseSensitive(f, "quota");
    cJSON* signals = cJSON_GetObjectItemCaseSensitive(quota, "signals");
    const bool disabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "disabled")) ||
                          _jstr(f, "status") == "disabled";
    const bool has_signals = cJSON_IsObject(signals) && signals->child != nullptr;
    const std::string provider = _jstr(f, "provider");
    // Without quota info, only active accounts of providers we can parse are
    // worth a card — disabled ones (bf pool has many) and others are noise.
    if (!has_signals && (disabled || (provider != "codex" && provider != "claude"))) return false;

    card.source = "cpamgr";
    std::string tag = _cpamgrTag(f);
    if (!tag.empty()) {
        card.name = (provider == "codex" ? std::string("GPT ") : provider + " ") + tag;
    } else {
        card.name = _maskEmail(_jstr(f, "account"));
        if (card.name.empty()) card.name = _jstr(f, "label");
    }

    // The relay learns quota passively from response headers — an active
    // account with no recent traffic has no signals yet. Show it anyway.
    if (!has_signals) {
        card.user = provider;
        quota::Window win;
        win.name = "quota";
        win.text = "no data yet";
        card.windows.push_back(std::move(win));
        return true;
    }
    std::string plan = _sig(signals, "X-Codex-Plan-Type");
    if (plan.empty()) plan = _jstr(f, "provider");
    card.user = plan;
    if (disabled) card.error = "disabled";

    // codex windows: primary = weekly (7d), secondary = 5h session
    _cpamgrWindow(signals, "X-Codex-Primary", "1w", card);
    _cpamgrWindow(signals, "X-Codex-Secondary", "5h", card);

    // claude windows
    _claudeWindow(signals, "5h", card);
    _claudeWindow(signals, "7d", card);

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
    return true;
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
        quota::Card card;
        if (_cpamgrFileCard(f, card)) out.push_back(std::move(card));
    }
    if (out.empty() && err.empty()) err = "no accounts";
    cJSON_Delete(root);
}

// Streaming variant: never holds the whole body or a whole-files DOM. Each
// top-level files element is parsed alone (~2KB) as its text arrives.
static void _fetchCpamgr(const char* url, const char* path,
                         const std::vector<std::pair<const char*, const char*>>& headers,
                         quota::Snapshot& snap, const char* source_tag,
                         int timeout_ms, const std::function<bool()>& should_abort)
{
    CpaJsonSlicer slicer;
    size_t before = snap.cards.size();

    slicer.on_file = [&](const std::string& obj_text) {
        cJSON* f = cJSON_Parse(obj_text.c_str());
        if (!f) return;
        quota::Card card;
        if (_cpamgrFileCard(f, card)) {
            if (source_tag) card.source = source_tag;
            snap.cards.push_back(std::move(card));
        }
        cJSON_Delete(f);
    };

    std::string err;
    bool aborted = false;
    _httpFetch(std::string(url) + path, headers, err, timeout_ms,
               [&](const char* d, size_t n) {
                   slicer.feed(d, n);
                   return !(should_abort && should_abort());
               },
               &aborted);

    // API-level error object/string beats transport errors for display.
    if (!slicer.error_text.empty()) {
        cJSON* e = cJSON_Parse(slicer.error_text.c_str());
        if (e) {
            std::string api_err = cJSON_IsObject(e) ? _jstr(e, "message") : slicer.error_text;
            if (!api_err.empty() && api_err.front() == '"') {
                api_err = api_err.substr(1, api_err.size() - 2);  // strip quotes
            }
            if (!api_err.empty() && snap.error.empty()) snap.error = api_err;
            cJSON_Delete(e);
        } else if (snap.error.empty()) {
            snap.error = slicer.error_text;
        }
    }

    if (aborted) {
        mclog::tagInfo(_tag, "fetch {}{} aborted early", url, path);
        return;  // keep whatever cards arrived — not an error state
    }
    if (!err.empty()) {
        if (snap.error.empty()) snap.error = err;
        mclog::tagWarn(_tag, "fetch {}{} failed: {}", url, path, err);
        return;
    }
    if (!slicer.sawFiles() && snap.error.empty()) {
        snap.error = "no files array";
    }
    if (snap.cards.size() == before && snap.error.empty()) {
        snap.error = "no accounts";
    }
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

quota::Snapshot quota::fetchAll(std::function<void(const Snapshot&)> on_progress,
                                std::function<bool()> should_abort)
{
    Snapshot snap;
    auto aborted = [&]() { return should_abort && should_abort(); };
    if (CONFIG_QUOTA_MAGPIE_URL[0] != '\0') {
        // panel auth is cookie-based: magpie_web_<port>=<key>
        _fetchSource(CONFIG_QUOTA_MAGPIE_URL, "/api/usage/quotas?asked=1",
                     {{"Cookie", "magpie_web_3430=" CONFIG_QUOTA_MAGPIE_KEY}},
                     _parseMagpie, snap);
        if (on_progress) on_progress(snap);
    }
    if (aborted()) return snap;
    if (CONFIG_QUOTA_CPAMGR_URL[0] != '\0') {
        // Cloudflare edge blocks ESP-TLS fingerprints — the cpamgr URL points
        // at the LAN nginx relay which injects auth server-side, so the
        // device just needs a plain HTTP GET. /quota and /quota-bf are two
        // separate cpamgr instances behind the same relay host.
        _fetchCpamgr(CONFIG_QUOTA_CPAMGR_URL, "/quota", {}, snap, "cpa", 30000, should_abort);
        if (on_progress) on_progress(snap);
        if (!aborted() && CONFIG_QUOTA_CPAMGR_BF_PATH[0] != '\0') {
            _fetchCpamgr(CONFIG_QUOTA_CPAMGR_URL, CONFIG_QUOTA_CPAMGR_BF_PATH, {},
                         snap, "cpa-bf", 30000, should_abort);
            if (on_progress) on_progress(snap);
        }
        if (!aborted() && snap.cards.empty() && !snap.error.empty()) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            snap.error.clear();
            _fetchCpamgr(CONFIG_QUOTA_CPAMGR_URL, "/quota", {}, snap, "cpa", 30000, should_abort);
            if (on_progress) on_progress(snap);
        }
    }
    if (snap.cards.empty() && snap.error.empty()) {
        snap.error = "no source configured";
    }
    return snap;
}
