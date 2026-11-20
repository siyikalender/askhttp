//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of json_escape() (see json_escape.hpp).
//

#include "json_escape.hpp"

std::string json_escape(const std::string& p_input)
{
  std::string result;
  result.reserve(p_input.size() * 2);
  
  for (auto c : p_input)
  {
    switch(c)
    {
    default:
      // JSON does not allow raw control characters (U+0000..U+001F) in a
      // string; previously the ones without a short form below were copied
      // as they were, which produced invalid JSON.
      if (static_cast<unsigned char>(c) < 0x20)
      {
        static const char hex[] = "0123456789abcdef";
        result += "\\u00";
        result += hex[static_cast<unsigned char>(c) >> 4];
        result += hex[static_cast<unsigned char>(c) & 0x0f];
      }
      else
      {
        result += c;
      }
      break;
    case '\\':
    case '"':
      result += "\\";
      result += c;
      break;
    case '\b':
      result += "\\b";
      break;
    case '\t':
      result += "\\t";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\r':
      result += "\\r";
      break;
    }
  }
  
  return result;
}
