//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Common string_view alias and a conversion helper that works with every
// Boost version.
//
// Up to Boost 1.80 boost::beast::string_view is boost::string_view, which
// has a to_string() member. From Boost 1.81 on it is boost::core::string_view,
// which has no to_string(). Use to_std_string() instead of to_string().
//

#ifndef ASKHTTP_STRING_VIEW_HPP
#define ASKHTTP_STRING_VIEW_HPP

#include <string>

#include <boost/beast/core/string.hpp>

using string_view = boost::beast::string_view;

// Copies the viewed characters into a std::string.
// Also use this when a NUL-terminated string is needed: string_view::data()
// does not guarantee a terminating NUL.
std::string to_std_string(string_view sv);

// ASKHTTP_STRING_VIEW_HPP
#endif
