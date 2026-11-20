//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// parse_uri(): splits a request target ("/api?id=5") into its path and its
// query parameters and percent-decodes both.
//

#ifndef ASKHTTP_PARSE_URI_HPP
#define ASKHTTP_PARSE_URI_HPP

#include "query.hpp"
#include <string>

void parse_uri(const std::string  &p_uri, 
               std::string        &p_path, 
               query              &p_query);

// ASKHTTP_PARSE_URI_HPP
#endif
