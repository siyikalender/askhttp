//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of the logging functions (see log.hpp): log state, the
// default sink and control character escaping.
//

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>

#include "log.hpp"

namespace
{

// Control characters are escaped so that text taken from requests (paths,
// names) cannot start a new, forged line in a log file.
std::string escape_control_chars(const std::string& p_text)
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

// Function-local static: safe to use from other static initializers.
log_state& get_log_state()
{
  static log_state state;
  return state;
}

void default_sink(log_level p_level, std::string_view p_message)
{
  auto& st = get_log_state();
  std::lock_guard<std::mutex> lock(st.stderr_mutex);
  std::cerr << '[' << log_level_name(p_level) << "] " << p_message << '\n';
}

} // namespace

const char* log_level_name(log_level p_level)
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

void set_log_sink(log_sink p_sink)
{
  auto& st = get_log_state();
  auto ptr = p_sink ? std::make_shared<log_sink>(std::move(p_sink)) : nullptr;
  std::lock_guard<std::mutex> lock(st.sink_mutex);
  st.sink = std::move(ptr);
}

void set_log_level(log_level p_level)
{
  get_log_state().min_level = static_cast<int>(p_level);
}

bool log_enabled(log_level p_level)
{
  return static_cast<int>(p_level) <= get_log_state().min_level.load();
}

namespace detail
{

void log_write(log_level p_level, const std::string& p_message)
{
  try
  {
    std::string const message = escape_control_chars(p_message);

    auto& st = get_log_state();
    std::shared_ptr<log_sink> sink;
    {
      std::lock_guard<std::mutex> lock(st.sink_mutex);
      sink = st.sink;
    }

    // The sink is called without holding sink_mutex, so a sink may itself
    // call set_log_sink() or log() without deadlocking.
    if (sink)
      (*sink)(p_level, message);
    else
      default_sink(p_level, message);
  }
  catch (...)
  {
    // Logging must never take a server thread down.
  }
}

} // namespace detail
