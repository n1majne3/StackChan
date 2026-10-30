// Minimal host stub — drops mclog formatting calls used by quota_fetch.cpp
#pragma once
#include <string_view>

namespace mclog {
template <typename... A> inline void tagWarn(std::string_view, const char*, A...) {}
template <typename... A> inline void tagInfo(std::string_view, const char*, A...) {}
template <typename... A> inline void tagError(std::string_view, const char*, A...) {}
template <typename... A> inline void tagDebug(std::string_view, const char*, A...) {}
}  // namespace mclog
