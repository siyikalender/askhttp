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

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
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

inline const char* log_level_name(log_level p_level)
{
  switch (p_level)
  {
  case log_level::error:   return "error";
  case log_level::warning: return "warning";
  case log_level::info:    return "info";
  case log_level::debug:   return "debug";
  }
  return "?";
}

namespace detail
{

// Control characters are escaped so that text taken from requests (paths,
// names) cannot start a new, forged line in a log file.
inline std::string escape_control_chars(std::string p_text)
{
  static const char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(p_text.size());

  for (char c : p_text)
  {
    auto const u = static_cast<unsigned char>(c);
    if      (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if (u < 0x20 || u == 0x7f)
    {
      result += "\\x";
      result += hex[u >> 4];
      result += hex[u & 0x0f];
    }
    else result += c;
  }

  return result;
}

struct log_state
{
  std::atomic<int>           min_level{static_cast<int>(log_level::info)};
  std::mutex                 sink_mutex;   // guards sink (the pointer only)
  std::shared_ptr<log_sink>  sink;
  std::mutex                 stderr_mutex; // serializes the default sink
};

inline log_state& get_log_state()
{
  static log_state state;
  return state;
}

inline void default_sink(log_level p_level, std::string_view p_message)
{
  auto& st = get_log_state();
  std::lock_guard<std::mutex> lock(st.stderr_mutex);
  std::cerr << '[' << log_level_name(p_level) << "] " << p_message << '\n';
}

} // namespace detail

// Replaces the sink. An empty function restores the default (std::cerr).
inline void set_log_sink(log_sink p_sink)
{
  auto& st = detail::get_log_state();
  auto ptr = p_sink ? std::make_shared<log_sink>(std::move(p_sink)) : nullptr;
  std::lock_guard<std::mutex> lock(st.sink_mutex);
  st.sink = std::move(ptr);
}

// Messages less severe than p_level are dropped (default: info).
inline void set_log_level(log_level p_level)
{
  detail::get_log_state().min_level = static_cast<int>(p_level);
}

inline bool log_enabled(log_level p_level)
{
  return static_cast<int>(p_level) <= detail::get_log_state().min_level.load();
}

// Formats the arguments with operator<< and passes the message to the
// sink. Arguments are only formatted when the level is enabled.
template<typename... Args>
void log_message(log_level p_level, Args&&... p_args)
{
  if (!log_enabled(p_level))
    return;

  std::ostringstream os;
  (os << ... << std::forward<Args>(p_args));
  std::string const message = detail::escape_control_chars(os.str());

  auto& st = detail::get_log_state();
  std::shared_ptr<log_sink> sink;
  {
    std::lock_guard<std::mutex> lock(st.sink_mutex);
    sink = st.sink;
  }

  // The sink is called without holding sink_mutex, so a sink may itself
  // call set_log_sink() or log() without deadlocking.
  try
  {
    if (sink)
      (*sink)(p_level, message);
    else
      detail::default_sink(p_level, message);
  }
  catch (...)
  {
    // Logging must never take a server thread down.
  }
}

template<typename... Args> void log_error  (Args&&... a) { log_message(log_level::error,   std::forward<Args>(a)...); }
template<typename... Args> void log_warning(Args&&... a) { log_message(log_level::warning, std::forward<Args>(a)...); }
template<typename... Args> void log_info   (Args&&... a) { log_message(log_level::info,    std::forward<Args>(a)...); }
template<typename... Args> void log_debug  (Args&&... a) { log_message(log_level::debug,   std::forward<Args>(a)...); }

// ASKHTTP_LOG_HPP
#endif
