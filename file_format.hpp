//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// File formats accepted by upload(). A format couples the file name
// extensions with a check of the file content (its signature, the "magic
// bytes"), so a file is accepted only if its name and its content agree.
// The type a client declares (Content-Type) is never trusted.
//
// The application chooses the formats per upload service:
//
//   .formats = { file_format::pdf, file_format::png, file_format::docx }
//
// and can define its own:
//
//   file_format dxf{"dxf", {"dxf"}, "image/vnd.dxf", false,
//                   [](std::string_view d) { return d.substr(0, 2) == "0\n"; }};
//

#ifndef ASKHTTP_FILE_FORMAT_HPP
#define ASKHTTP_FILE_FORMAT_HPP

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct file_format
{
  // Short name, e.g. "pdf".
  std::string name;

  // File name extensions in lower case, without the dot.
  std::vector<std::string> extensions;

  // Content-Type used when the file is downloaded.
  std::string content_type;

  // true: a browser may display the file (Content-Disposition: inline).
  // false: the file is always offered as a download (attachment); used for
  // formats that can carry script (SVG) or that browsers do not display.
  bool inline_safe = false;

  // Checks the file content. Must not read past the end of data: files of
  // any length, including empty ones, are passed to it.
  std::function<bool(std::string_view data)> matches;

  bool has_extension(std::string_view p_ext) const
  {
    return std::find(extensions.begin(), extensions.end(), p_ext) != extensions.end();
  }

  // Built-in formats
  static const file_format pdf;
  static const file_format png;
  static const file_format jpeg;
  static const file_format gif;
  static const file_format webp;
  static const file_format bmp;
  static const file_format tiff;
  static const file_format svg;
  static const file_format docx;    // MS Office 2007+ (ZIP based)
  static const file_format xlsx;
  static const file_format pptx;
  static const file_format odt;     // LibreOffice / OpenDocument (ZIP based)
  static const file_format ods;
  static const file_format odp;
  static const file_format doc;     // MS Office 97-2003 (OLE based)
  static const file_format xls;
  static const file_format ppt;
};

namespace detail
{

inline bool starts_with(std::string_view data, std::string_view prefix)
{
  return data.substr(0, prefix.size()) == prefix;
}

inline bool is_zip(std::string_view d)
{
  return starts_with(d, std::string_view("PK\x03\x04", 4));
}

inline bool is_ole(std::string_view d)
{
  return starts_with(d, std::string_view("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8));
}

// SVG is XML text: after an optional BOM, white space, XML declaration,
// comments and DOCTYPE, the first element must be <svg.
inline bool is_svg(std::string_view d)
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

} // namespace detail

// PDF readers accept the header anywhere in the first 1024 bytes.
inline const file_format file_format::pdf{"pdf", {"pdf"}, "application/pdf", true,
  [](std::string_view d) { return d.substr(0, 1024).find("%PDF-") != std::string_view::npos; }};

inline const file_format file_format::png{"png", {"png"}, "image/png", true,
  [](std::string_view d) { return detail::starts_with(d, "\x89PNG\r\n\x1A\n"); }};

inline const file_format file_format::jpeg{"jpeg", {"jpg", "jpeg", "jpe"}, "image/jpeg", true,
  [](std::string_view d) { return detail::starts_with(d, "\xFF\xD8\xFF"); }};

inline const file_format file_format::gif{"gif", {"gif"}, "image/gif", true,
  [](std::string_view d) { return detail::starts_with(d, "GIF87a") || detail::starts_with(d, "GIF89a"); }};

inline const file_format file_format::webp{"webp", {"webp"}, "image/webp", true,
  [](std::string_view d) { return d.size() >= 12 && detail::starts_with(d, "RIFF") && d.substr(8, 4) == "WEBP"; }};

inline const file_format file_format::bmp{"bmp", {"bmp"}, "image/bmp", true,
  [](std::string_view d) { return d.size() >= 26 && detail::starts_with(d, "BM"); }};

inline const file_format file_format::tiff{"tiff", {"tif", "tiff"}, "image/tiff", false,
  [](std::string_view d)
  {
    return detail::starts_with(d, std::string_view("II*\0", 4)) ||
           detail::starts_with(d, std::string_view("MM\0*", 4));
  }};

// Not inline_safe: an SVG can contain script, which would run on this
// server's origin if the browser displayed the file.
inline const file_format file_format::svg{"svg", {"svg"}, "image/svg+xml", false,
  [](std::string_view d) { return detail::is_svg(d); }};

// ZIP-based office formats are recognised by the ZIP signature only; any
// ZIP file with one of these extensions passes.
inline const file_format file_format::docx{"docx", {"docx"},
  "application/vnd.openxmlformats-officedocument.wordprocessingml.document", false, detail::is_zip};

inline const file_format file_format::xlsx{"xlsx", {"xlsx"},
  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", false, detail::is_zip};

inline const file_format file_format::pptx{"pptx", {"pptx"},
  "application/vnd.openxmlformats-officedocument.presentationml.presentation", false, detail::is_zip};

inline const file_format file_format::odt{"odt", {"odt"},
  "application/vnd.oasis.opendocument.text", false, detail::is_zip};

inline const file_format file_format::ods{"ods", {"ods"},
  "application/vnd.oasis.opendocument.spreadsheet", false, detail::is_zip};

inline const file_format file_format::odp{"odp", {"odp"},
  "application/vnd.oasis.opendocument.presentation", false, detail::is_zip};

inline const file_format file_format::doc{"doc", {"doc"}, "application/msword", false, detail::is_ole};

inline const file_format file_format::xls{"xls", {"xls"}, "application/vnd.ms-excel", false, detail::is_ole};

inline const file_format file_format::ppt{"ppt", {"ppt"}, "application/vnd.ms-powerpoint", false, detail::is_ole};

// ASKHTTP_FILE_FORMAT_HPP
#endif
