//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Writes JSON text into a std::string with operator<<:
//
//   std::string out;
//   out << '{' << "id" << ':' << 42 << ',' << "name" << ':' << name << '}';
//
// Strings are quoted and escaped, an empty boost::optional or nullptr is
// written as null, doubles in their shortest round-trip form and std::tm
// as an ISO 8601 date. make_result() builds the {"status":..,"what":..}
// object that upload handlers return.
//

#ifndef ASKHTTP_JSON_WRITER_HPP
#define ASKHTTP_JSON_WRITER_HPP

#include <string>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <charconv>

#include <boost/optional.hpp>

#include "json_escape.hpp"

inline std::string& operator<<(std::string& p_dest, const char p_src)
{
  p_dest += p_src;
  
  return p_dest;
}

// Writes a JSON string. An empty string is written as "" (previously
// null); use an empty boost::optional or nullptr for null.
inline std::string& operator<<(std::string& p_dest, const std::string& p_src)
{
  p_dest += "\"";
  p_dest += json_escape(p_src);
  p_dest += "\"";

  return p_dest;
}

inline std::string& operator<<(std::string& p_dest, std::nullptr_t)
{
  p_dest += "null";
  return p_dest;
}

inline std::string& operator<<(std::string& p_dest, const int p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

inline std::string& operator<<(std::string& p_dest, const long long p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

inline std::string& operator<<(std::string& p_dest, const unsigned long long p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

inline std::string& operator<<(std::string& p_dest, const std::size_t p_src)
{
  p_dest += std::to_string(p_src);
  
  return p_dest;
}

// Writes the shortest text that reads back as the same double.
// std::to_string() used before always printed 6 decimals: 0.0000001 became
// "0.000000" and 1.23456789 became "1.234568". NaN and infinity have no
// JSON representation and are written as null (to_string gave "nan").
inline std::string& operator<<(std::string& p_dest, const double p_src)
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

// Writes an ISO 8601 date, "YYYY-MM-DD" (previously without zero padding,
// e.g. "2026-1-5", which JavaScript's Date does not parse consistently).
inline std::string& operator<<(std::string& p_dest, const std::tm& d)
{
  char buf[32];
  int const n = std::snprintf(buf, sizeof(buf), "\"%04d-%02d-%02d\"",
                              d.tm_year + 1900, d.tm_mon + 1, d.tm_mday);
  p_dest.append(buf, n > 0 ? static_cast<std::size_t>(n) : 0);

  return p_dest;
}

template<typename T>
inline std::string& operator<<
(
  std::string&                p_dest, 
  const boost::optional<T>&   p_src
)
{
  if (p_src)
    p_dest << *p_src;
  else
    p_dest += "null";
  
  return p_dest;
}

template<typename T>
inline std::string make_result
(
  const std::size_t value, 
  T&& id
)
{
  std::string result;
  
  result << '{' << "status" << ':' << value << ',' << "what" << ':' << id << '}';
  
  return result;
}

// ASKHTTP_JSON_WRITER_HPP
#endif
