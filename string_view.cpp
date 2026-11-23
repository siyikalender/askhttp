//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of to_std_string() (see string_view.hpp).
//

#include "string_view.hpp"

std::string to_std_string(string_view sv)
{
  return std::string(sv.data(), sv.size());
}
