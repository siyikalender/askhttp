//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of file_format::has_extension() and the content checks
// of the built-in formats (see file_format.hpp).
//

#include <algorithm>

#include "file_format.hpp"

namespace
{

bool starts_with(std::string_view data, std::string_view prefix)
{
  return data.substr(0, prefix.size()) == prefix;
}

} // namespace

bool file_format::has_extension(std::string_view p_ext) const
{
  return std::find(extensions.begin(), extensions.end(), p_ext) != extensions.end();
}

namespace detail
{

// PDF readers accept the header anywhere in the first 1024 bytes.
bool match_pdf(std::string_view d)
{
  return d.substr(0, 1024).find("%PDF-") != std::string_view::npos;
}

bool match_png(std::string_view d)
{
  return starts_with(d, "\x89PNG\r\n\x1A\n");
}

bool match_jpeg(std::string_view d)
{
  return starts_with(d, "\xFF\xD8\xFF");
}

bool match_gif(std::string_view d)
{
  return starts_with(d, "GIF87a") || starts_with(d, "GIF89a");
}

bool match_webp(std::string_view d)
{
  return d.size() >= 12 && starts_with(d, "RIFF") && d.substr(8, 4) == "WEBP";
}

bool match_bmp(std::string_view d)
{
  return d.size() >= 26 && starts_with(d, "BM");
}

bool match_tiff(std::string_view d)
{
  return starts_with(d, std::string_view("II*\0", 4)) ||
         starts_with(d, std::string_view("MM\0*", 4));
}

// SVG is XML text: after an optional BOM, white space, XML declaration,
// comments and DOCTYPE, the first element must be <svg.
bool match_svg(std::string_view d)
{
  if (starts_with(d, "\xEF\xBB\xBF"))
    d.remove_prefix(3);

  for (int guard = 0; guard < 64; ++guard)
  {
    auto const first = d.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
      return false;
    d.remove_prefix(first);

    if (starts_with(d, "<?") || starts_with(d, "<!DOCTYPE") || starts_with(d, "<!--"))
    {
      auto const end = starts_with(d, "<!--") ? d.find("-->") : d.find('>');
      if (end == std::string_view::npos)
        return false;
      d.remove_prefix(end + (starts_with(d, "<!--") ? 3 : 1));
      continue;
    }

    return starts_with(d, "<svg") &&
           d.size() > 4 && std::string_view(" \t\r\n>/").find(d[4]) != std::string_view::npos;
  }

  return false;
}

bool is_zip(std::string_view d)
{
  return starts_with(d, std::string_view("PK\x03\x04", 4));
}

bool is_ole(std::string_view d)
{
  return starts_with(d, std::string_view("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8));
}

} // namespace detail
