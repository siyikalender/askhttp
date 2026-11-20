//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// File formats, upload_provider and download_provider (no network).
//

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <random>

#include "provider.hpp"

namespace
{

namespace fs = std::filesystem;

const std::string pdf_data  = "%PDF-1.7\n1 0 obj\n<<>>\nendobj\n";
const std::string png_data  = std::string("\x89PNG\r\n\x1A\n", 8) + "IHDR....";
const std::string jpeg_data = "\xFF\xD8\xFF\xE0" "JFIF";
const std::string zip_data  = std::string("PK\x03\x04", 4) + "rest of zip";
const std::string ole_data  = std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + "ole";
const std::string svg_data  = "<?xml version=\"1.0\"?>\n<!-- c -->\n<!DOCTYPE svg>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>";

// A fresh, empty directory removed again at the end of the test.
struct temp_dir
{
  fs::path path;

  temp_dir()
  {
    std::random_device rd;
    path = fs::temp_directory_path() / ("askhttp_test_" + std::to_string(rd()));
    fs::create_directories(path);
  }

  ~temp_dir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::string read_file(const fs::path& p)
{
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

query name_query(const std::string& name)
{
  query q;
  q["name"] = name;
  return q;
}

std::size_t entries(const fs::path& p)
{
  return static_cast<std::size_t>(std::distance(fs::directory_iterator(p), fs::directory_iterator()));
}

// Silences the log for the lifetime of the object.
struct quiet_log
{
  quiet_log()  { set_log_sink([](log_level, std::string_view) {}); }
  ~quiet_log() { set_log_sink({}); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(file_upload)

// formats: signatures of the built-in formats
BOOST_AUTO_TEST_CASE(builtin_signatures)
{
  BOOST_CHECK(file_format::pdf.matches(pdf_data));
  BOOST_CHECK(file_format::pdf.matches("\n\n" + pdf_data));   // header within 1024 bytes
  BOOST_CHECK(file_format::png.matches(png_data));
  BOOST_CHECK(file_format::jpeg.matches(jpeg_data));
  BOOST_CHECK(file_format::gif.matches("GIF89a...."));
  BOOST_CHECK(file_format::webp.matches(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16)));
  BOOST_CHECK(file_format::bmp.matches("BM" + std::string(30, '\0')));
  BOOST_CHECK(file_format::tiff.matches(std::string("II*\0", 4) + "...."));
  BOOST_CHECK(file_format::docx.matches(zip_data));
  BOOST_CHECK(file_format::odt.matches(zip_data));
  BOOST_CHECK(file_format::doc.matches(ole_data));

  BOOST_CHECK(!file_format::pdf.matches(png_data));
  BOOST_CHECK(!file_format::png.matches(pdf_data));
  BOOST_CHECK(!file_format::bmp.matches("BMW is a car"));      // too short for a header
  BOOST_CHECK(!file_format::docx.matches(ole_data));
  BOOST_CHECK(!file_format::doc.matches(zip_data));
  BOOST_CHECK(!file_format::jpeg.matches(""));

  // Short input must not throw (an exception here used to escape the
  // request handler).
  for (auto const& f : {file_format::pdf, file_format::png, file_format::jpeg, file_format::gif,
                        file_format::webp, file_format::bmp, file_format::tiff, file_format::svg,
                        file_format::docx, file_format::doc})
    for (std::string_view d : {std::string_view(""), std::string_view("R"), std::string_view("RIFF")})
      BOOST_CHECK(!f.matches(d));
}

// formats: SVG detection
BOOST_AUTO_TEST_CASE(svg_detection)
{
  BOOST_CHECK(file_format::svg.matches(svg_data));
  BOOST_CHECK(file_format::svg.matches("\xEF\xBB\xBF<svg>"));
  BOOST_CHECK(file_format::svg.matches("  <svg\nwidth=\"1\"/>"));
  BOOST_CHECK(!file_format::svg.matches("<html><svg/></html>"));
  BOOST_CHECK(!file_format::svg.matches("<svgx/>"));
  BOOST_CHECK(!file_format::svg.matches("<!-- unterminated"));
  BOOST_CHECK(!file_format::svg.matches(pdf_data));
}

// formats: only SVG and office documents are attachment-only
BOOST_AUTO_TEST_CASE(inline_safety)
{
  BOOST_CHECK(file_format::pdf.inline_safe);
  BOOST_CHECK(file_format::png.inline_safe);
  BOOST_CHECK(!file_format::svg.inline_safe);
  BOOST_CHECK(!file_format::docx.inline_safe);
  BOOST_CHECK(!file_format::doc.inline_safe);
}

// upload: invalid configuration is refused when the service is created
BOOST_AUTO_TEST_CASE(invalid_configuration)
{
  temp_dir dir;
  auto handler = [](const uploaded_file&) { return std::string("{}"); };

  BOOST_CHECK_THROW(upload_provider({.directory = "", .formats = {file_format::pdf}}, handler),
                    std::invalid_argument);
  BOOST_CHECK_THROW(upload_provider({.directory = dir.path.string(), .formats = {}}, handler),
                    std::invalid_argument);
  BOOST_CHECK_THROW(upload_provider({.directory = dir.path.string(), .formats = {file_format::pdf}}, {}),
                    std::invalid_argument);
  BOOST_CHECK_THROW(upload_provider({.max_size = 0, .directory = dir.path.string(),
                                     .formats = {file_format::pdf}}, handler),
                    std::invalid_argument);
}

// upload: a valid file is stored and passed to the handler
BOOST_AUTO_TEST_CASE(stores_valid_file)
{
  temp_dir dir;
  std::optional<std::string> stored_name, stored_path, original, format;
  std::size_t size = 0;

  upload_provider up({.directory = dir.path.string(), .formats = {file_format::pdf, file_format::png}},
    [&](const uploaded_file& f)
    {
      stored_name = f.stored_name;
      stored_path = f.stored_path;
      original    = f.original_name;
      format      = f.format.name;
      size        = f.size;
      return make_result(1, 42);
    });

  auto const r = up.content(pdf_data, name_query("../../Rapor 2026.PDF"));

  BOOST_CHECK_EQUAL(*r, R"({"status":1,"what":42})");
  BOOST_REQUIRE(stored_name);
  BOOST_CHECK(detail::is_stored_name(*stored_name));
  BOOST_CHECK_EQUAL(fs::path(*stored_name).extension().string(), ".pdf");
  BOOST_CHECK_EQUAL(*original, "Rapor 2026.PDF");          // directory part removed
  BOOST_CHECK_EQUAL(*format, "pdf");
  BOOST_CHECK_EQUAL(size, pdf_data.size());
  BOOST_CHECK_EQUAL(read_file(*stored_path), pdf_data);
  BOOST_CHECK(fs::is_empty(dir.path / "tmp"));              // no temporary file left
}

// upload: refused files
BOOST_AUTO_TEST_CASE(refuses_invalid_files)
{
  temp_dir dir;
  quiet_log quiet;
  int calls = 0;
  upload_provider up({.max_size = 100, .directory = dir.path.string(), .formats = {file_format::pdf}},
    [&](const uploaded_file&) { ++calls; return std::string("{}"); });

  auto const not_allowed = std::string(R"({"status":0,"what":"file type not allowed"})");

  BOOST_CHECK_EQUAL(*up.content(png_data, name_query("x.png")), not_allowed);   // extension
  BOOST_CHECK_EQUAL(*up.content(png_data, name_query("x.pdf")), not_allowed);   // content
  BOOST_CHECK_EQUAL(*up.content(pdf_data, name_query("x")),     not_allowed);   // no extension
  BOOST_CHECK_EQUAL(*up.content("",       name_query("x.pdf")), R"({"status":0,"what":"empty file"})");
  BOOST_CHECK_EQUAL(*up.content(pdf_data, query{}),             R"({"status":0,"what":"missing file name"})");
  BOOST_CHECK_EQUAL(*up.content(pdf_data + std::string(200, ' '), name_query("x.pdf")),
                    R"({"status":0,"what":"file too large"})");

  BOOST_CHECK_EQUAL(calls, 0);
  BOOST_CHECK_EQUAL(entries(dir.path), 1u);   // only tmp/
}

// upload: if the handler fails, the stored file is removed
BOOST_AUTO_TEST_CASE(handler_failure_removes_file)
{
  temp_dir dir;
  quiet_log quiet;
  upload_provider up({.directory = dir.path.string(), .formats = {file_format::pdf}},
    [](const uploaded_file&) -> std::string { throw std::runtime_error("database down"); });

  BOOST_CHECK_EQUAL(*up.content(pdf_data, name_query("a.pdf")), R"({"status":0,"what":"upload failed"})");
  BOOST_CHECK_EQUAL(entries(dir.path), 1u);   // only tmp/
}

// upload: leftover temporary files are removed at start-up
BOOST_AUTO_TEST_CASE(removes_stale_temporary_files)
{
  temp_dir dir;
  fs::create_directories(dir.path / "tmp");
  std::ofstream(dir.path / "tmp" / "old.part") << "x";

  upload_provider up({.directory = dir.path.string(), .formats = {file_format::pdf}},
                     [](const uploaded_file&) { return std::string("{}"); });

  BOOST_CHECK(!fs::exists(dir.path / "tmp" / "old.part"));
}

// upload: at most max_concurrent slots
BOOST_AUTO_TEST_CASE(concurrency_slots)
{
  temp_dir dir;
  upload_provider up({.directory = dir.path.string(), .formats = {file_format::pdf}, .max_concurrent = 2},
                     [](const uploaded_file&) { return std::string("{}"); });

  auto a = up.try_acquire();
  auto b = up.try_acquire();
  BOOST_CHECK(a && b);
  BOOST_CHECK(!up.try_acquire());
  a.reset();
  BOOST_CHECK(up.try_acquire());
}

// download: headers come from the stored format, not from the client
BOOST_AUTO_TEST_CASE(download_headers)
{
  temp_dir dir;
  upload_options opts{.directory = dir.path.string(),
                      .formats = {file_format::pdf, file_format::svg}};

  std::string pdf_name, svg_name;
  upload_provider up(opts, [&](const uploaded_file& f)
  {
    (f.format.name == "pdf" ? pdf_name : svg_name) = f.stored_name;
    return std::string("{}");
  });
  up.content(pdf_data, name_query("rapor \xC3\xA7 \"son\".pdf"));
  up.content(svg_data, name_query("logo.svg"));

  download_provider down(opts, [&](const query& q) -> std::optional<download_info>
  {
    auto const id = *q.at("id");
    if (id == "pdf") return download_info{pdf_name, "rapor \xC3\xA7 \"son\".pdf"};
    if (id == "svg") return download_info{svg_name, "logo.svg"};
    return std::nullopt;
  });

  query q;
  q["id"] = std::string("pdf");
  auto const pdf = down.produce("", q);
  BOOST_REQUIRE(pdf);
  BOOST_CHECK_EQUAL(pdf->body, pdf_data);
  BOOST_CHECK_EQUAL(pdf->content_type, "application/pdf");
  BOOST_CHECK_EQUAL(pdf->content_disposition,
    "inline; filename=\"rapor __ _son_.pdf\"; filename*=UTF-8''rapor%20%C3%A7%20%22son%22.pdf");

  q["id"] = std::string("svg");
  auto const svg = down.produce("", q);
  BOOST_REQUIRE(svg);
  BOOST_CHECK_EQUAL(svg->content_type, "image/svg+xml");
  BOOST_CHECK_EQUAL(svg->content_disposition.rfind("attachment;", 0), 0u);   // never inline

  q["id"] = std::string("none");
  BOOST_CHECK(!down.produce("", q));
}

// download: names the resolver returns are checked again
BOOST_AUTO_TEST_CASE(download_rejects_bad_names)
{
  temp_dir dir;
  quiet_log quiet;
  upload_options opts{.directory = dir.path.string(), .formats = {file_format::pdf}};
  std::string name;

  download_provider down(opts, [&](const query&) { return std::optional<download_info>({name, ""}); });

  for (auto const& bad : {std::string("../../etc/passwd"),
                          std::string("0123456789abcdef0123456789abcdef.png"),     // format not allowed
                          std::string("0123456789abcdef0123456789abcdef.pdf"),     // does not exist
                          std::string("0123456789abcdef0123456789abcdeg.pdf"),     // not hex
                          std::string("0123456789abcdef0123456789abcdef.p/f")})
  {
    name = bad;
    BOOST_CHECK(!down.produce("", query{}));
  }
}

BOOST_AUTO_TEST_SUITE_END()
