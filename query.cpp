//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of get_param() (see query.hpp).
//

#include "query.hpp"

boost::optional<std::string>  get_param(const query         &p_query,
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
