/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "quota_cache.h"
#include <settings.h>
#include <cstdlib>

namespace quota {

static constexpr const char* _ns       = "quota_cache";
static constexpr const char* _key_data = "cards";
static constexpr const char* _key_time = "fetched_at";

// Serialized as one line per card: name \t window \t percent \t error
static std::string _serialize(const CachedSummary& s)
{
    std::string out;
    for (const auto& c : s.cards) {
        out += c.name;
        out += '\t';
        out += c.window;
        out += '\t';
        out += std::to_string(c.percent);
        out += '\t';
        out += c.error ? '1' : '0';
        out += '\n';
    }
    return out;
}

static std::vector<CachedCard> _deserialize(const std::string& data)
{
    std::vector<CachedCard> cards;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        pos              = eol + 1;

        std::string fields[4];
        size_t start = 0;
        int n        = 0;
        for (; n < 4; ++n) {
            size_t tab = line.find('\t', start);
            fields[n]  = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
            if (tab == std::string::npos) {
                ++n;
                break;
            }
            start = tab + 1;
        }
        if (n < 4) continue;  // malformed line

        CachedCard c;
        c.name    = fields[0];
        c.window  = fields[1];
        c.percent = std::atoi(fields[2].c_str());
        c.error   = fields[3] == "1";
        cards.push_back(std::move(c));
    }
    return cards;
}

CachedSummary summarize(const Snapshot& snap, int64_t fetchedAt)
{
    CachedSummary s;
    s.fetchedAt = fetchedAt;
    for (const auto& card : snap.cards) {
        CachedCard c;
        c.name  = card.name;
        c.error = !card.error.empty();
        const Window* worst = nullptr;
        for (const auto& win : card.windows) {
            if (win.percent < 0) continue;
            if (!worst || win.percent < worst->percent) worst = &win;
        }
        if (worst) {
            c.window  = worst->name;
            c.percent = (int)worst->percent;
        }
        s.cards.push_back(std::move(c));
    }
    return s;
}

void saveCache(const CachedSummary& summary)
{
    Settings settings(_ns, true);
    const std::string data = _serialize(summary);
    if (settings.GetString(_key_data) != data) {
        settings.SetString(_key_data, data);
    }
    // Stored as int32 (NVS helper API) — fine until 2038.
    settings.SetInt(_key_time, (int32_t)summary.fetchedAt);
}

CachedSummary loadCache()
{
    Settings settings(_ns, false);
    CachedSummary s;
    s.cards     = _deserialize(settings.GetString(_key_data));
    s.fetchedAt = settings.GetInt(_key_time, 0);
    return s;
}

}  // namespace quota
