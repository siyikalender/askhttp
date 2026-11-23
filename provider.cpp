//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of the response providers (see provider.hpp).
//

#include <cctype>
#include <cstdint>
#include <fstream>
#include <random>
#include <stdexcept>
#include <utility>

#include "provider.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "mime_type.hpp"
#include "read_stream.hpp"

namespace
{

template<class T>
response_data json(const T& p_value)
{
  return response_data{boost::json::serialize(p_value), "application/json", {}, 200};
}

response_data no_content()
{
  return response_data{{}, {}, {}, 204};
}

std::string lower_ascii(std::string s)
{
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// File name without any directory part ("a/b\\c.pdf" -> "c.pdf").
std::string base_name(const std::string& p_name)
{
  auto const pos = p_name.find_last_of("/\\");
  return pos == std::string::npos ? p_name : p_name.substr(pos + 1);
}

std::string extension_of(const std::string& p_name)
{
  auto const pos = p_name.rfind('.');
  return pos == std::string::npos ? std::string() : lower_ascii(p_name.substr(pos + 1));
}

// 128 random bits as 32 hex digits.
std::string random_name()
{
  thread_local std::random_device rd;
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < 4; ++i)
  {
    std::uint32_t v = rd();
    for (int k = 0; k < 8; ++k, v >>= 4)
      out += hex[v & 0x0f];
  }
  return out;
}

// Content-Disposition value (RFC 6266): an ASCII fallback in filename= and
// the exact UTF-8 name in filename*= (RFC 5987 percent encoding).
std::string content_disposition(bool p_inline, const std::string& p_file_name)
{
  std::string ascii;
  std::string encoded;
  static const char hex[] = "0123456789ABCDEF";

  for (unsigned char c : p_file_name)
  {
    bool const plain = std::isalnum(c) || std::string_view("!#$&+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;

    ascii += (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? static_cast<char>(c) : '_';

    if (plain)
      encoded += static_cast<char>(c);
    else
    {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0f];
    }
  }

  return std::string(p_inline ? "inline" : "attachment") +
         "; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded;
}

const file_format* find_format(const std::vector<file_format>& p_formats,
                               const std::string&              p_ext)
{
  for (auto const& f : p_formats)
    if (f.has_extension(p_ext))
      return &f;
  return nullptr;
}

} // namespace

namespace detail
{

bool is_stored_name(const std::string& p_name)
{
  if (p_name.size() < 34 || p_name[32] != '.')
    return false;
  for (std::size_t i = 0; i < 32; ++i)
    if (!std::isxdigit(static_cast<unsigned char>(p_name[i])))
      return false;
  for (std::size_t i = 33; i < p_name.size(); ++i)
    if (!std::isalnum(static_cast<unsigned char>(p_name[i])))
      return false;
  return true;
}

} // namespace detail

// ---- response_provider ----------------------------------------------------

// Defined here so that the vtable is emitted in this translation unit only.
response_provider::~response_provider() = default;

std::optional<response_data>
response_provider::produce(const std::string& request_body,
                           const query&       request_query) const
{
  auto body = content(request_body, request_query);
  if (!body)
    return std::nullopt;

  auto const mt = mime_type();
  return response_data{std::move(*body), std::string(mt.data(), mt.size()), {}, 200};
}

std::optional<response_data>
response_provider::respond(const request_context& ctx) const
{
  return produce(ctx.body, ctx.params);
}

bool response_provider::cors_allowed(std::string_view /*origin*/) const
{
  return false;
}

// ---- text_provider --------------------------------------------------------

text_provider::text_provider(generator p_generator)
: generator_(std::move(p_generator))
{}

std::optional<std::string>
text_provider::content(const std::string&  /* request_body */,
                       const query&        request_query) const
{
  return generator_(request_query);
}

string_view text_provider::mime_type() const
{
  return "application/text";
}

response_provider::types text_provider::type() const
{
  return types::text;
}

// ---- json_rpc_provider ----------------------------------------------------

void json_rpc_provider::add(std::string p_method, rpc_handler p_handler)
{
  if (!p_handler)
    throw std::invalid_argument("json_rpc: handler is required");
  methods_[std::move(p_method)] = std::move(p_handler);
}

const rpc_handler& json_rpc_provider::handler(const std::string& p_method) const
{
  return methods_.at(p_method);
}

std::optional<response_data>
json_rpc_provider::produce(const std::string&  request_body,
                           const query&        /* request_query */) const
{
  boost::json::error_code ec;
  boost::json::value const root = boost::json::parse(request_body, ec);

  if (ec)
  {
    log_warning("json-rpc: ", ec.message());
    return json(detail::rpc_error_response(nullptr, rpc_code::parse_error, "Parse error"));
  }

  if (auto const* batch = root.if_array())
  {
    if (batch->empty())
      return json(detail::rpc_error_response(nullptr, rpc_code::invalid_request, "Invalid Request"));

    boost::json::array responses;
    for (auto const& request : *batch)
    {
      auto r = call(request);
      if (r)
        responses.push_back(std::move(*r));
    }

    if (responses.empty())
      return no_content();
    return json(responses);
  }

  auto r = call(root);
  if (!r)
    return no_content();
  return json(*r);
}

std::optional<std::string>
json_rpc_provider::content(const std::string& request_body, const query& request_query) const
{
  auto r = produce(request_body, request_query);
  if (!r)
    return std::nullopt;
  return std::move(r->body);
}

string_view json_rpc_provider::mime_type() const
{
  return "application/json";
}

response_provider::types json_rpc_provider::type() const
{
  return types::json;
}

std::optional<boost::json::object>
json_rpc_provider::call(const boost::json::value& p_request) const
{
  auto const* obj = p_request.if_object();
  auto const* ver = obj ? obj->if_contains("jsonrpc") : nullptr;
  auto const* met = obj ? obj->if_contains("method") : nullptr;
  auto const* id  = obj ? obj->if_contains("id") : nullptr;

  // A valid id is a string, a number or null.
  bool const id_ok = !id || id->is_null() || id->is_string() ||
                     id->is_int64() || id->is_uint64() || id->is_double();
  boost::json::value const reply_id = (id && id_ok) ? *id : boost::json::value(nullptr);

  if (!obj || !ver || !ver->is_string() || ver->as_string() != "2.0" ||
      !met || !met->is_string() || !id_ok)
  {
    log_warning("json-rpc: invalid request");
    return detail::rpc_error_response(reply_id, rpc_code::invalid_request, "Invalid Request");
  }

  std::string const method(met->as_string().data(), met->as_string().size());

  static const boost::json::value no_params;   // null
  auto const* params = obj->if_contains("params");
  if (params && !params->is_object() && !params->is_array())
    return id ? std::optional(detail::rpc_error_response(reply_id, rpc_code::invalid_params, "Invalid params"))
              : std::nullopt;

  auto it = methods_.find(method);
  if (it == methods_.end())
  {
    log_warning("json-rpc: unknown method '", method, "'");
    return id ? std::optional(detail::rpc_error_response(reply_id, rpc_code::method_not_found, "Method not found"))
              : std::nullopt;
  }

  try
  {
    boost::json::value result = it->second(params ? *params : no_params);
    if (!id)
      return std::nullopt;
    return detail::rpc_result(reply_id, std::move(result));
  }
  catch (rpc_error const& e)
  {
    if (e.code() <= -32000 && e.code() >= -32768 && e.code() != rpc_code::invalid_params)
      log_error("json-rpc method '", method, "': ", e.what());
    else
      log_debug("json-rpc method '", method, "': ", e.what());
    if (!id)
      return std::nullopt;
    return detail::rpc_error_response(reply_id, e.code(), e.what(), e.data());
  }
  catch (std::exception const& e)
  {
    // Details stay in the log; they may contain SQL or file paths.
    log_error("json-rpc method '", method, "' failed: ", e.what());
    if (!id)
      return std::nullopt;
    return detail::rpc_error_response(reply_id, rpc_code::internal_error, "Internal error");
  }
}

// ---- file_provider --------------------------------------------------------

file_provider::file_provider(std::string path)
: path_(std::move(path))
, mime_type_(::mime_type(path_))
{}

std::optional<std::string>
file_provider::content(const std::string&  /* request_body */,
                       const query&        /* request_query */) const
{
  std::ifstream f(path_, std::ios::binary);
  if (!f)
  {
    log_warning("file not found: ", path_);
    return std::nullopt;
  }

  std::string data;
  read_whole_stream(f, data);
  return data;
}

string_view file_provider::mime_type() const
{
  return mime_type_;
}

response_provider::types file_provider::type() const
{
  return types::file;
}

// ---- upload_provider ------------------------------------------------------

upload_provider::upload_provider(upload_options p_options, upload_handler p_handler)
: options_(std::move(p_options))
, handler_(std::move(p_handler))
{
  if (options_.directory.empty())
    throw std::invalid_argument("upload: directory is required");
  if (options_.formats.empty())
    throw std::invalid_argument("upload: at least one format is required");
  if (options_.max_size == 0 || options_.max_concurrent <= 0)
    throw std::invalid_argument("upload: max_size and max_concurrent must be positive");
  if (!handler_)
    throw std::invalid_argument("upload: handler is required");

  // Throws std::filesystem::filesystem_error if it cannot be created.
  std::filesystem::create_directories(temp_directory());

  // Remove files left over by an interrupted earlier run.
  std::error_code ec;
  for (auto const& e : std::filesystem::directory_iterator(temp_directory(), ec))
    if (e.path().extension() == ".part")
      std::filesystem::remove(e.path(), ec);
}

const upload_options& upload_provider::options() const
{
  return options_;
}

upload_provider::slot::slot(std::atomic<int>& p_count)
: count_(p_count)
{}

upload_provider::slot::~slot()
{
  --count_;
}

std::unique_ptr<upload_provider::slot> upload_provider::try_acquire() const
{
  if (++active_ > options_.max_concurrent)
  {
    --active_;
    return nullptr;
  }
  return std::make_unique<slot>(active_);
}

std::optional<std::string>
upload_provider::content(const std::string&  request_body,
                         const query&        request_query) const
{
  auto const name_it = request_query.find("name");
  if (name_it == request_query.end() || !name_it->second || name_it->second->empty())
    return make_result(0, "missing file name");

  std::string const original = base_name(*name_it->second);
  std::string const ext      = extension_of(original);

  if (request_body.empty())
    return make_result(0, "empty file");

  if (request_body.size() > options_.max_size)
    return make_result(0, "file too large");

  const file_format* format = find_format(options_.formats, ext);
  bool content_ok = false;
  if (format)
  {
    // A check defined by the application might throw; an exception here
    // would otherwise leave the request handler.
    try { content_ok = format->matches(request_body); }
    catch (std::exception const& e) { log_error("upload: format check failed: ", e.what()); }
  }

  if (!format || !content_ok)
  {
    log_warning("upload refused: '", original, "' (",
                format ? "content does not match the extension" : "extension not allowed", ")");
    return make_result(0, "file type not allowed");
  }

  namespace fs = std::filesystem;
  std::string const id      = random_name();
  std::string const stored  = id + "." + ext;
  fs::path    const temp    = temp_directory() / (id + ".part");
  fs::path    const target  = fs::path(options_.directory) / stored;
  std::error_code ec;

  // Write to a temporary file first and rename it: a file under its final
  // name is always complete.
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    f.write(request_body.data(), static_cast<std::streamsize>(request_body.size()));
    f.close();
    if (!f)
    {
      fs::remove(temp, ec);
      log_error("upload: cannot write ", temp.string());
      return make_result(0, "upload failed");
    }
  }

  fs::rename(temp, target, ec);
  if (ec)
  {
    fs::remove(temp, ec);
    log_error("upload: cannot move file into ", options_.directory);
    return make_result(0, "upload failed");
  }

  try
  {
    return handler_(uploaded_file{stored, target.string(), original,
                                  request_body.size(), *format});
  }
  catch (std::exception const& e)
  {
    fs::remove(target, ec);
    log_error("upload handler failed: ", e.what());
    return make_result(0, "upload failed");
  }
}

string_view upload_provider::mime_type() const
{
  return "application/json";
}

response_provider::types upload_provider::type() const
{
  return types::upload;
}

std::filesystem::path upload_provider::temp_directory() const
{
  return std::filesystem::path(options_.directory) / "tmp";
}

// ---- download_provider ----------------------------------------------------

download_provider::download_provider(upload_options p_options, download_resolver p_resolver)
: options_(std::move(p_options))
, resolver_(std::move(p_resolver))
{
  if (options_.directory.empty() || options_.formats.empty() || !resolver_)
    throw std::invalid_argument("download: directory, formats and resolver are required");
}

std::optional<response_data>
download_provider::produce(const std::string&  /* request_body */,
                           const query&        request_query) const
{
  auto const info = resolver_(request_query);
  if (!info)
    return std::nullopt;

  const file_format* format = detail::is_stored_name(info->stored_name)
    ? find_format(options_.formats, extension_of(info->stored_name))
    : nullptr;

  if (!format)
  {
    log_warning("download: invalid stored name '", info->stored_name, "'");
    return std::nullopt;
  }

  std::ifstream f(std::filesystem::path(options_.directory) / info->stored_name,
                  std::ios::binary);
  if (!f)
  {
    log_warning("download: file not found: ", info->stored_name);
    return std::nullopt;
  }

  response_data r;
  read_whole_stream(f, r.body);
  r.content_type = format->content_type;
  r.content_disposition = content_disposition
  (
    format->inline_safe,
    info->original_name.empty() ? info->stored_name : info->original_name
  );
  return r;
}

std::optional<std::string>
download_provider::content(const std::string& request_body, const query& request_query) const
{
  auto r = produce(request_body, request_query);
  if (!r)
    return std::nullopt;
  return std::move(r->body);
}

string_view download_provider::mime_type() const
{
  return "application/octet-stream";
}

response_provider::types download_provider::type() const
{
  return types::download;
}

// ---- factories ------------------------------------------------------------

response_provider* upload(upload_options p_options, upload_handler p_handler)
{
  return new upload_provider(std::move(p_options), std::move(p_handler));
}

response_provider* download(upload_options p_options, download_resolver p_resolver)
{
  return new download_provider(std::move(p_options), std::move(p_resolver));
}

response_provider* text(const generator& g)
{
  return new text_provider(g);
}

json_rpc_provider* json_rpc()
{
  return new json_rpc_provider();
}

response_provider* file(std::string path)
{
  return new file_provider(std::move(path));
}

void add(response_map&      m,
         std::string        path,
         response_provider  *p)
{
  m.insert(
    m.end(),
    std::make_pair(
      std::move(path),
      std::unique_ptr<response_provider>(p)
    )
  );
}
