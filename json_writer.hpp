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
#include <cstddef>

#include <boost/optional.hpp>

#include "json_escape.hpp"

std::string& operator<<(std::string& p_dest, const char p_src);

// Writes a JSON string. An empty string is written as ""; use an empty
// boost::optional or nullptr for null.
std::string& operator<<(std::string& p_dest, const std::string& p_src);

std::string& operator<<(std::string& p_dest, std::nullptr_t);

std::string& operator<<(std::string& p_dest, const int p_src);

std::string& operator<<(std::string& p_dest, const long long p_src);

std::string& operator<<(std::string& p_dest, const unsigned long long p_src);

std::string& operator<<(std::string& p_dest, const std::size_t p_src);

// Writes the shortest text that reads back as the same double. NaN and
// infinity have no JSON representation and are written as null.
std::string& operator<<(std::string& p_dest, const double p_src);

// Writes an ISO 8601 date, "YYYY-MM-DD".
std::string& operator<<(std::string& p_dest, const std::tm& d);

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
