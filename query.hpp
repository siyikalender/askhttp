//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// The decoded query string of a request (see parse_uri.hpp). A parameter
// without '=' ("?flag") is stored with an empty optional.
//
//   auto id = get_param(params, "id");   // boost::optional<std::string>
//

#ifndef ASKHTTP_QUERY_HPP
#define ASKHTTP_QUERY_HPP

#include <string>
#include <map>
#include <boost/optional.hpp>

using query = std::map<std::string, boost::optional<std::string>>;

// Value of a parameter; an empty optional if it is missing or has no value.
inline boost::optional<std::string>  get_param(const query         &p_query,
                                               const std::string   &p_key)
{
  boost::optional<std::string> result;
  
  auto it = p_query.find(p_key);
  if (it != p_query.end())
  {
    result = it->second;
  }
  
  return result;
}

// ASKHTTP_QUERY_HPP
#endif
