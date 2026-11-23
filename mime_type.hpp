//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// MIME type lookup, moved out of server.hpp so that provider.hpp can be
// included on its own without depending on include order.
//

#ifndef ASKHTTP_MIME_TYPE_HPP
#define ASKHTTP_MIME_TYPE_HPP

#include "string_view.hpp"

// Return a reasonable mime type based on the extension of a file.
// The returned view refers to static storage, so it stays valid for the
// whole lifetime of the program.
string_view mime_type(string_view path);

// ASKHTTP_MIME_TYPE_HPP
#endif
