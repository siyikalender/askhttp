//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Minimal levelled logging for the askhttp library.
//
// By default messages of level info and above go to std::cerr as
//   "[warning] accept: Too many open files"
// The application can route them elsewhere (syslog, a file, its own
// logger) and choose the minimum level:
//
//   set_log_sink([](log_level l, std::string_view msg) { ... });
//   set_log_level(log_level::debug);
//
// Both may be called at any time and from any thread. The sink is called
// from server threads concurrently; it must be thread-safe (the default
// sink is). Messages never contain request or response bodies, and control
// characters in them are escaped (a message is always a single line).
//

#ifndef ASKHTTP_LOG_HPP
#define ASKHTTP_LOG_HPP

#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

enum class log_level
{
  error   = 0,
  warning = 1,
  info    = 2,
  debug   = 3
};

using log_sink = std::function<void(log_level, std::string_view)>;

const char* log_level_name(log_level p_level);

// Replaces the sink. An empty function restores the default (std::cerr).
void set_log_sink(log_sink p_sink);

// Messages less severe than p_level are dropped (default: info).
void set_log_level(log_level p_level);

bool log_enabled(log_level p_level);

namespace detail
{

// Escapes control characters in p_message and passes it to the sink.
// Never throws.
void log_write(log_level p_level, const std::string& p_message);

} // namespace detail

// Formats the arguments with operator<< and passes the message to the
// sink. Arguments are only formatted when the level is enabled.
template<typename... Args>
void log_message(log_level p_level, Args&&... p_args)
{
  if (!log_enabled(p_level))
    return;

  std::ostringstream os;
  (os << ... << std::forward<Args>(p_args));
  detail::log_write(p_level, os.str());
}

template<typename... Args> void log_error  (Args&&... a) { log_message(log_level::error,   std::forward<Args>(a)...); }
template<typename... Args> void log_warning(Args&&... a) { log_message(log_level::warning, std::forward<Args>(a)...); }
template<typename... Args> void log_info   (Args&&... a) { log_message(log_level::info,    std::forward<Args>(a)...); }
template<typename... Args> void log_debug  (Args&&... a) { log_message(log_level::debug,   std::forward<Args>(a)...); }

// ASKHTTP_LOG_HPP
#endif
