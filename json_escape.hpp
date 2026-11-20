//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// JSON string escaping: '"' -> \", '\' -> \\, control characters -> \n,
// \t, ... or \u00XX. The result goes between the quotes of a JSON string;
// json_writer.hpp uses it for operator<<(std::string&, const std::string&).
//

#ifndef ASKHTTP_JSON_ESCAPE_HPP
#define ASKHTTP_JSON_ESCAPE_HPP

#include <string>

#include <boost/optional.hpp>

std::string json_escape(const std::string& p_input);

// Optional overload: an empty value stays empty.
inline boost::optional<std::string>
json_escape(const boost::optional<std::string>& p_value)
{
  if (p_value)
    return json_escape(*p_value);

  return p_value;
}

// ASKHTTP_JSON_ESCAPE_HPP
#endif
