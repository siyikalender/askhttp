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

  bool has_extension(std::string_view p_ext) const;

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

// Content checks of the built-in formats.
bool match_pdf (std::string_view d);
bool match_png (std::string_view d);
bool match_jpeg(std::string_view d);
bool match_gif (std::string_view d);
bool match_webp(std::string_view d);
bool match_bmp (std::string_view d);
bool match_tiff(std::string_view d);
bool match_svg (std::string_view d);
bool is_zip    (std::string_view d);
bool is_ole    (std::string_view d);

} // namespace detail

inline const file_format file_format::pdf{"pdf", {"pdf"}, "application/pdf", true, detail::match_pdf};

inline const file_format file_format::png{"png", {"png"}, "image/png", true, detail::match_png};

inline const file_format file_format::jpeg{"jpeg", {"jpg", "jpeg", "jpe"}, "image/jpeg", true, detail::match_jpeg};

inline const file_format file_format::gif{"gif", {"gif"}, "image/gif", true, detail::match_gif};

inline const file_format file_format::webp{"webp", {"webp"}, "image/webp", true, detail::match_webp};

inline const file_format file_format::bmp{"bmp", {"bmp"}, "image/bmp", true, detail::match_bmp};

inline const file_format file_format::tiff{"tiff", {"tif", "tiff"}, "image/tiff", false, detail::match_tiff};

// Not inline_safe: an SVG can contain script, which would run on this
// server's origin if the browser displayed the file.
inline const file_format file_format::svg{"svg", {"svg"}, "image/svg+xml", false, detail::match_svg};

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
