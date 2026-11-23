//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of the non-template operator<< overloads of
// json_writer.hpp.
//

#include <charconv>
#include <cmath>
#include <cstdio>

#include "json_writer.hpp"

std::string& operator<<(std::string& p_dest, const char p_src)
{
  p_dest += p_src;
  
  return p_dest;
}

// An empty string was previously written as null.
std::string& operator<<(std::string& p_dest, const std::string& p_src)
{
  p_dest += "\"";
  p_dest += json_escape(p_src);
  p_dest += "\"";

  return p_dest;
}

std::string& operator<<(std::string& p_dest, std::nullptr_t)
{
  p_dest += "null";
  return p_dest;
}

std::string& operator<<(std::string& p_dest, const int p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

std::string& operator<<(std::string& p_dest, const long long p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

std::string& operator<<(std::string& p_dest, const unsigned long long p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

std::string& operator<<(std::string& p_dest, const std::size_t p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

// std::to_string() used before always printed 6 decimals: 0.0000001 became
// "0.000000" and 1.23456789 became "1.234568". For NaN and infinity it gave
// "nan"/"inf", which is not JSON.
std::string& operator<<(std::string& p_dest, const double p_src)
{
  if (!std::isfinite(p_src))
  {
    p_dest += "null";
    return p_dest;
  }

  char buf[32];
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
  auto const r = std::to_chars(buf, buf + sizeof(buf), p_src);
  p_dest.append(buf, r.ptr);
#else
  int const n = std::snprintf(buf, sizeof(buf), "%.17g", p_src);
  p_dest.append(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
#endif

  return p_dest;
}

// Previously written without zero padding, e.g. "2026-1-5", which
// JavaScript's Date does not parse consistently.
std::string& operator<<(std::string& p_dest, const std::tm& d)
{
  char buf[32];
  int const n = std::snprintf(buf, sizeof(buf), "\"%04d-%02d-%02d\"",
                              d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
  p_dest.append(buf, n > 0 ? static_cast<std::size_t>(n) : 0);

  return p_dest;
}
