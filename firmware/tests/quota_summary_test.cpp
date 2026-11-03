/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// Host test for _parseSummary in quota_fetch.cpp — the .cpp is included
// directly so the anonymous-namespace functions are visible, same trick as
// sim_quota. board.h/mooncake_log.h resolve to ../sim_quota/stubs.

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "../main/apps/app_quota/quota_fetch.cpp"

namespace {

int _failures = 0;

void fail(const char* label, const std::string& detail)
{
    std::cerr << "FAIL " << label << ": " << detail << '\n';
    ++_failures;
}

void expectEq(const std::string& actual, const std::string& expected, const char* label)
{
    if (actual != expected) fail(label, "expected \"" + expected + "\", got \"" + actual + "\"");
}

void expectTrue(bool cond, const char* label, const std::string& detail = "")
{
    if (!cond) fail(label, detail.empty() ? "expected true" : detail);
}

const quota::Card* findCard(const std::vector<quota::Card>& cards, const std::string& name)
{
    for (auto& c : cards)
        if (c.name == name) return &c;
    return nullptr;
}

// Response covering: dup "ChatGPT Pro" names, Claude Max CN labels + suffix,
// a disabled entry, a null-usedPct window, resetCredits (value and null),
// a hidden magpie provider and an unlimited window.
const char* kBody = R"json({
  "generatedAt": "2026-02-10T14:40:00.000Z",
  "tz": "Asia/Singapore",
  "cached": false,
  "cpa": [
    { "id": "codex-alpha", "instance": "main", "provider": "codex",
      "email": "90-g***@example.com", "plan": "ChatGPT Pro", "disabled": false,
      "windows": [
        { "name": "5 小时", "usedPct": 21, "remainingPct": 79,
          "resetsAt": "2026-02-10T20:00:00.000Z", "forecast": null },
        { "name": "7 天", "usedPct": 65, "remainingPct": 35,
          "resetsAt": "2026-02-16T08:00:05.000Z", "forecast": null }
      ],
      "resetCredits": { "available": 3, "earliestExpiry": "2026-03-01T00:00:00.000Z" } },
    { "id": "codex-beta", "instance": "main", "provider": "codex",
      "email": "zq-t***@example.com", "plan": "ChatGPT Pro", "disabled": false,
      "windows": [
        { "name": "5 hours", "usedPct": 10, "remainingPct": 90,
          "resetsAt": "2026-02-10T18:00:00Z", "forecast": null },
        { "name": "每周", "usedPct": 40, "remainingPct": 60,
          "resetsAt": null, "forecast": null }
      ],
      "resetCredits": { "available": null, "earliestExpiry": null } },
    { "id": "claude-main", "instance": "main", "provider": "claude",
      "email": "cl***@example.com", "plan": "Claude Max", "disabled": false,
      "windows": [
        { "name": "5 小时", "usedPct": 55, "remainingPct": 45,
          "resetsAt": "2026-02-10T19:00:00Z", "forecast": null },
        { "name": "7 天", "usedPct": 30, "remainingPct": 70,
          "resetsAt": "2026-02-17T08:00:00Z", "forecast": null },
        { "name": "7 天 · Opus", "usedPct": 12, "remainingPct": 88,
          "resetsAt": "2026-02-17T08:00:00Z", "forecast": null }
      ] },
    { "id": "xai-off", "instance": "main", "provider": "xai",
      "email": "x***@example.com", "plan": null, "disabled": true,
      "windows": [
        { "name": "Code Review", "usedPct": 5, "remainingPct": 95,
          "resetsAt": null, "forecast": null }
      ] },
    { "id": "kimi-cpa", "instance": "main", "provider": "kimi",
      "email": "km***@example.com", "plan": "Kimi For Coding", "disabled": false,
      "windows": [
        { "name": "Code Review", "usedPct": null, "remainingPct": null,
          "resetsAt": null, "forecast": null },
        { "name": "7 days", "usedPct": 80, "remainingPct": 20,
          "resetsAt": null, "forecast": null }
      ] }
  ],
  "magpie": [
    { "id": "kimi-code", "name": "Kimi Code", "provider": "kimi", "plan": "pro",
      "until": "2026-03-01",
      "windows": [
        { "name": "month", "usedPct": 18, "remainingPct": 82,
          "resetsAt": "2026-03-01T00:00:00Z", "unlimited": false }
      ] },
    { "id": "opencode-zen", "name": "OpenCode Zen", "provider": "opencode",
      "plan": "zen", "until": null,
      "windows": [
        { "name": "5h", "usedPct": 50, "remainingPct": 50,
          "resetsAt": null, "unlimited": false }
      ] },
    { "id": "bigmodel", "name": "BigModel", "provider": "zhipu", "plan": "free",
      "until": null,
      "windows": [
        { "name": "daily", "usedPct": null, "remainingPct": null,
          "resetsAt": null, "unlimited": true }
      ] }
  ]
})json";

std::vector<quota::Card> parse(const std::string& body, std::string& err)
{
    std::vector<quota::Card> cards;
    _parseSummary(body, cards, err);
    return cards;
}

void testMapping()
{
    std::string err;
    auto cards = parse(kBody, err);
    expectTrue(err.empty(), "parse err", err);
    expectEq(std::to_string(cards.size()), "6", "card count");

    // disabled skipped, hidden dropped
    expectTrue(findCard(cards, "Xai") == nullptr, "disabled skipped");
    expectTrue(findCard(cards, "OpenCode Zen") == nullptr, "hidden dropped");

    // duplicate "GPT Pro" names get "(<tag>)" from the email local part
    // (montserrat_14, the card-name font, has no '·' glyph)
    expectTrue(findCard(cards, "GPT Pro (90g)") != nullptr, "dup tag 90g");
    expectTrue(findCard(cards, "GPT Pro (zqt)") != nullptr, "dup tag zqt");

    // sort: lowest remaining percent first, unlimited-only card last
    const char* want[] = {"Kimi For Coding", "GPT Pro (90g)", "Claude Max",
                          "GPT Pro (zqt)",   "Kimi Code",    "BigModel"};
    if (cards.size() == 6) {
        for (int i = 0; i < 6; i++) expectEq(cards[i].name, want[i], "sort order");
    }

    // codex-alpha: labels compacted, remaining %, reset string, resets window
    auto* alpha = findCard(cards, "GPT Pro (90g)");
    expectTrue(alpha != nullptr, "alpha exists");
    if (alpha && alpha->windows.size() == 3) {
        expectEq(alpha->windows[0].name, "5h", "label 5h");
        expectEq(alpha->windows[0].text, "79%", "remaining text");
        expectEq(alpha->windows[0].resetsAt, "02-10 20:00", "reset 5h");
        expectEq(alpha->windows[1].name, "7d", "label 7d");
        expectEq(alpha->windows[1].text, "35%", "remaining 35");
        expectEq(alpha->windows[1].resetsAt, "02-16 08:00", "reset 7d");
        expectEq(alpha->windows[2].name, "resets", "resets name");
        expectEq(alpha->windows[2].text, "3 · 03-01", "resets text");
        expectTrue(alpha->windows[2].percent < 0, "resets text-only");
    } else if (alpha) {
        fail("alpha window count", std::to_string(alpha->windows.size()));
    }

    // codex-beta: "5 hours"->"5h", "每周"->"7d", null available -> "-"
    auto* beta = findCard(cards, "GPT Pro (zqt)");
    if (beta && beta->windows.size() == 3) {
        expectEq(beta->windows[0].name, "5h", "beta 5h");
        expectEq(beta->windows[1].name, "7d", "beta 每周->7d");
        expectEq(beta->windows[2].text, "-", "beta resets dash");
    } else {
        fail("beta windows", "missing or wrong count");
    }

    // claude: suffix kept after compaction
    auto* claude = findCard(cards, "Claude Max");
    if (claude && claude->windows.size() == 3) {
        expectEq(claude->windows[2].name, "7d Opus", "opus suffix");
        expectEq(claude->windows[2].text, "88%", "opus pct");
    } else {
        fail("claude windows", "missing or wrong count");
    }

    // null usedPct window dropped
    auto* kimi = findCard(cards, "Kimi For Coding");
    if (kimi) {
        expectEq(std::to_string(kimi->windows.size()), "1", "null-usedPct skipped");
        if (!kimi->windows.empty()) expectEq(kimi->windows[0].name, "7d", "kimi 7 days->7d");
    }

    // unlimited window
    auto* big = findCard(cards, "BigModel");
    if (big && !big->windows.empty()) {
        expectTrue(big->windows[0].unlimited, "unlimited flag");
        expectEq(big->windows[0].text, "unlimited", "unlimited text");
    } else {
        fail("bigmodel", "missing");
    }
}

void testErrors()
{
    std::string err;
    auto cards = parse("not json at all", err);
    expectEq(err, "bad json", "bad json");
    expectTrue(cards.empty(), "bad json cards");

    err.clear();
    cards = parse(R"({"cpa": [], "magpie": []})", err);
    expectEq(err, "no accounts", "empty arrays");
    expectTrue(cards.empty(), "empty arrays cards");
}

}  // namespace

int main()
{
    // Reset strings are asserted in UTC so the test is timezone-independent
    setenv("TZ", "UTC0", 1);
    tzset();
    testMapping();
    testErrors();
    if (_failures) {
        std::cerr << _failures << " failure(s)\n";
        return 1;
    }
    std::cout << "quota_summary_test: all passed\n";
    return 0;
}
