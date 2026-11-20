//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Compiles the Boost.JSON implementation into libaskhttp.a (header-only
// mode), so the application does not have to link libboost_json.
//
// This must happen in exactly one translation unit of the program. If the
// application also includes <boost/json/src.hpp> or links libboost_json,
// remove this file from the library to avoid duplicate symbols.
//

#include <boost/json/src.hpp>
