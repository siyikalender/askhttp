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

#include <optional>
#include <utility>

#include <boost/beast/core/string.hpp>

#include <haluj/pair_lookup.hpp>

#include "string_view.hpp"

// Return a reasonable mime type based on the extension of a file.
// The returned view refers to static storage, so it stays valid for the
// whole lifetime of the program.
inline string_view
mime_type(string_view path)
{
  using boost::beast::iequals;
  auto const ext =
    [&path]
    {
      auto const pos = path.rfind(".");
      if(pos == string_view::npos)
        return string_view{};
      return path.substr(pos);
    }();

  static const std::pair<string_view, string_view> mime_map[]=
  {
    {".htm"   , "text/html"},
    {".html"  , "text/html"},
    {".php"   , "text/html"},
    {".css"   , "text/css"},
    {".txt"   , "text/plain"},
    {".js"    , "application/javascript"},
    {".json"  , "application/json"},
    {".xml"   , "application/xml"},
    {".swf"   , "application/x-shockwave-flash"},
    {".flv"   , "video/x-flv"},
    {".png"   , "image/png"},
    {".jpe"   , "image/jpeg"},
    {".jpeg"  , "image/jpeg"},
    {".jpg"   , "image/jpeg"},
    {".gif"   , "image/gif"},
    {".bmp"   , "image/bmp"},
    {".ico"   , "image/vnd.microsoft.icon"},
    {".tiff"  , "image/tiff"},
    {".tif"   , "image/tiff"},
    {".svg"   , "image/svg+xml"},
    {".svgz"  , "image/svg+xml"},
    {".eot"   , "application/vnd.ms-fontobject"},
    {".ttf"   , "application/font-sfnt"},
    {".woff"  , "application/font-woff"},
    {".woff2" , "application/font-woff2"}
  };

  auto r = haluj::to_second
  (
    ext,
    mime_map,
    [&](string_view a, string_view b)
    {
      return iequals(a, b);
    }
  );

  return (r) ? *r : "application/text";
}

// ASKHTTP_MIME_TYPE_HPP
#endif
