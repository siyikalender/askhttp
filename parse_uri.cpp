//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of parse_uri() (see parse_uri.hpp): splitting the request
// target and percent decoding.
//

#include <map>
#include <string>

#include "parse_uri.hpp"

namespace
{

int hex_value(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes %XX sequences. A malformed sequence ("%zz", a trailing "%") is
// kept as it is. In the query part '+' stands for a space (HTML form
// encoding); in the path it is a literal '+'.
std::string percent_decode(const std::string& p_input, bool p_plus_is_space)
{
  std::string result;
  result.reserve(p_input.size());

  for (std::size_t i = 0; i < p_input.size(); ++i)
  {
    char c = p_input[i];

    if (c == '%' && i + 2 < p_input.size())
    {
      int hi = hex_value(p_input[i + 1]);
      int lo = hex_value(p_input[i + 2]);

      if (hi >= 0 && lo >= 0)
      {
        result += static_cast<char>(hi * 16 + lo);
        i += 2;
        continue;
      }
    }

    if (c == '+' && p_plus_is_space)
      c = ' ';

    result += c;
  }

  return result;
}

} // namespace

// Splits a request target into path and query and percent-decodes both.
//
//   "/api?name=John+Doe&city=%C4%B0zmir&flag"
//     path  = "/api"
//     query = { name: "John Doe", city: "İzmir", flag: (no value) }
//
// A parameter without '=' is stored with an empty optional; "k=" gives an
// empty string. Empty parts ("&&") are skipped. If a key appears more than
// once, the first occurrence is kept.
//
// The previous implementation reused the end of the previous key when a
// part had no '=', e.g. "?a=1&b&c=2". It then built a std::string from an
// iterator range in reverse order and threw std::length_error, which
// escaped the request handler and could stop the server.
void parse_uri(const std::string  &p_uri,
               std::string        &p_path,
               query              &p_query)
{
  p_path.clear();
  p_query.clear();

  auto const question = p_uri.find('?');

  p_path = percent_decode(p_uri.substr(0, question), false);

  if (question == std::string::npos)
    return;

  std::size_t start = question + 1;

  while (start <= p_uri.size())
  {
    auto amp = p_uri.find('&', start);
    if (amp == std::string::npos)
      amp = p_uri.size();

    std::string const part = p_uri.substr(start, amp - start);

    if (!part.empty())
    {
      auto const eq = part.find('=');

      if (eq == std::string::npos)
      {
        p_query.emplace(percent_decode(part, true), boost::optional<std::string>());
      }
      else
      {
        p_query.emplace(percent_decode(part.substr(0, eq), true),
                        percent_decode(part.substr(eq + 1), true));
      }
    }

    start = amp + 1;
    }
    }
