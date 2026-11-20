//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Response providers: the objects that produce the response for a path.
// The application maps paths to providers in a response_map:
//
//   add(responses, "/",        file("www/index.html"));
//   add(responses, "/hello",   text([](const query&) { return "hi"; }));
//   add(responses, "/api",     json_rpc());           // JSON-RPC 2.0
//   add(responses, "/upload",  upload(options, handler));
//   add(responses, "/file",    download(options, resolver));
//
// The MCP provider is in mcp.hpp, the server that uses the map in
// server.hpp.
//

#ifndef ASKHTTP_PROVIDER_HPP
#define ASKHTTP_PROVIDER_HPP

// Boost.Asio/Beast come first, before Boost.JSON and before any of this
// project's global declarations (query, get_param(), operator<<, ::detail, ...).
// With mcp.hpp included before server.hpp, a Boost 1.83 build failed with
// any_io_executor not constructible from io_context/strand executors, while
// the same server.hpp compiled when included first. Asio resolves some names
// when its templates are defined, so it is parsed here before anything else
// to make the include order of the application irrelevant.
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "file_format.hpp"
#include "json_rpc.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "mime_type.hpp"
#include "query.hpp"
#include "read_stream.hpp"

using generator = std::function<std::string(const query&)>;

// A response body with the headers that depend on it.
struct response_data
{
  std::string body;
  std::string content_type;          // empty: no Content-Type (e.g. 202 without body)
  std::string content_disposition;   // empty: no Content-Disposition header
  unsigned    status = 200;          // HTTP status code
};

// What a provider may look at in a request.
struct request_context
{
  const std::string&  method;   // "GET", "POST", ...
  const std::string&  body;
  const query&        params;   // decoded query string

  // Value of a request header (case-insensitive name); empty if missing.
  std::function<std::string(std::string_view name)> header;
  };

  // Base class of all response providers.
  //
  // A provider object is shared by every session and may be called
  // concurrently from several io_context threads. To make that safe,
  // providers are stateless: content() is const and returns the response
  // body by value instead of storing it in a member.
  //
  // Consequences for users of this header:
  //  - Configure providers (generators, json handlers, file paths) before
  //    start_server() / start_ssl_server() is called and do not modify them
  //    afterwards.
  //  - Generators and json handlers themselves must be thread-safe if the
  //    server runs with more than one thread.
  struct response_provider
{
  enum class types
  {
    text,       // GET, HEAD
    json,       // POST (JSON-RPC)
    file,       // GET, HEAD
    upload,     // POST
    download,   // GET, HEAD
    mcp         // POST (Model Context Protocol)
  };

  virtual ~response_provider() = default;

  // Produces the response body for one request.
  // An empty optional means the resource is not available; the server
  // then answers with "404 Not Found" instead of sending stale data.
  virtual std::optional<std::string>
  content(const std::string& request_body,
          const query&       request_query) const = 0;

  virtual string_view mime_type() const = 0;

  virtual types type() const = 0;

  // Produces the complete response. The default combines content() and
  // mime_type(); providers whose headers differ per response (download)
  // override it.
  virtual std::optional<response_data>
  produce(const std::string& request_body,
          const query&       request_query) const
  {
    auto body = content(request_body, request_query);
    if (!body)
      return std::nullopt;

    auto const mt = mime_type();
    return response_data{std::move(*body), std::string(mt.data(), mt.size()), {}, 200};
  }

  // Entry point used by the server. The default ignores headers and calls
  // produce(); providers that need headers or set the status (mcp)
  // override it.
  virtual std::optional<response_data>
  respond(const request_context& ctx) const
  {
    return produce(ctx.body, ctx.params);
  }

  // Cross-origin (CORS) access from browser pages. When this returns true
  // for the request's Origin header, the server answers the browser's
  // preflight (OPTIONS) request and adds Access-Control-Allow-Origin to the
  // responses of this resource. Default: no cross-origin access.
  virtual bool cors_allowed(std::string_view /*origin*/) const
  {
    return false;
  }
};

struct text_provider : response_provider
{
  explicit text_provider(generator p_generator)
  : generator_(std::move(p_generator)) {}

  std::optional<std::string>
  content(const std::string&  /* request_body */,
          const query&        request_query) const override
  {
    return generator_(request_query);
  }

  string_view mime_type() const override
  {
    return "application/text";
  }

  types type() const override
  {
    return types::text;
  }

  // Held by value on purpose. Holding a reference would dangle whenever
  // the caller passes a lambda, because the temporary std::function that
  // wraps it is destroyed at the end of the call to text().
  generator generator_;
};

// JSON-RPC 2.0 over HTTP POST.
//
//   POST /api
//   {"jsonrpc":"2.0","id":1,"method":"find_record","params":{"id":5}}
//   -> {"jsonrpc":"2.0","id":1,"result":{...}}
//   -> {"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}}
//
// Batches (a JSON array of requests) are answered with an array of
// responses. Notifications (requests without "id") are executed and get no
// response; a request or batch of notifications only is answered with
// 204 No Content.
class json_rpc_provider : public response_provider
{
public:
  // Registers a method. Must only be called before the server is started.
  void add(std::string p_method, rpc_handler p_handler)
  {
    if (!p_handler)
      throw std::invalid_argument("json_rpc: handler is required");
    methods_[std::move(p_method)] = std::move(p_handler);
  }

  // The handler of a registered method, e.g. to expose it as an MCP tool.
  // Throws std::out_of_range if the method is not registered.
  const rpc_handler& handler(const std::string& p_method) const
  {
    return methods_.at(p_method);
  }

  std::optional<response_data>
  produce(const std::string&  request_body,
          const query&        /* request_query */) const override
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
  content(const std::string& request_body, const query& request_query) const override
  {
    auto r = produce(request_body, request_query);
    if (!r)
      return std::nullopt;
    return std::move(r->body);
  }

  string_view mime_type() const override { return "application/json"; }

  types type() const override { return types::json; }

private:
  // Handles one request; an empty optional for notifications.
  std::optional<boost::json::object> call(const boost::json::value& p_request) const
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

  template<class T>
  static response_data json(const T& p_value)
  {
    return response_data{boost::json::serialize(p_value), "application/json", {}, 200};
  }

  static response_data no_content()
  {
    return response_data{{}, {}, {}, 204};
  }

  std::map<std::string, rpc_handler> methods_;
};

struct file_provider : response_provider
{
  explicit file_provider(std::string path)
  : path_(std::move(path))
  , mime_type_(::mime_type(path_))
  {}

  std::optional<std::string>
  content(const std::string&  /* request_body */,
          const query&        /* request_query */) const override
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

  string_view mime_type() const override
  {
    return mime_type_;
  }

  types type() const override
  {
    return types::file;
  }

  // Declaration order matters: mime_type_ is computed from path_.
  // The view returned by ::mime_type() refers to static storage.
  std::string                   path_;
  string_view                   mime_type_;
};

// ---- file upload and download --------------------------------------------

// Configuration shared by upload() and download().
struct upload_options
{
  // Largest accepted file. Larger uploads are refused with 413 as soon as
  // their headers arrive, before the body is received.
  std::size_t                 max_size = 10 * 1024 * 1024;

  // Where files are stored (required). Upload creates "<directory>/tmp" for
  // files being written. Must not be a directory that is served directly.
  std::string                 directory = {};

  // Accepted formats (required, at least one). A file is accepted if its
  // extension and its content both match one of them.
  std::vector<file_format>    formats = {};

  // Time allowed for receiving an upload body (other requests: 30 s).
  std::chrono::seconds        timeout{120};

  // Uploads received at the same time; more get 503. Bounds the memory
  // used for bodies (max_concurrent * max_size).
  int                         max_concurrent = 4;
};

// A file that was received, checked and stored.
struct uploaded_file
{
  std::string          stored_name;     // "<32 hex digits>.<ext>", chosen by the server
  std::string          stored_path;     // directory + "/" + stored_name
  std::string          original_name;   // as sent by the client; metadata only
  std::size_t          size;
  const file_format&   format;
};

// Called after the file is stored; returns the JSON response, e.g.
// make_result(1, id). If it throws, the stored file is removed.
using upload_handler = std::function<std::string(const uploaded_file&)>;

// Identifies a stored file for download.
struct download_info
{
  std::string stored_name;     // as given in uploaded_file::stored_name
  std::string original_name;   // offered to the browser as the file name
};

// Maps a download request (e.g. ?id=42) to a stored file; an empty
// optional answers 404.
using download_resolver = std::function<std::optional<download_info>(const query&)>;

namespace detail
{

inline std::string lower_ascii(std::string s)
{
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// File name without any directory part ("a/b\\c.pdf" -> "c.pdf").
inline std::string base_name(const std::string& p_name)
{
  auto const pos = p_name.find_last_of("/\\");
  return pos == std::string::npos ? p_name : p_name.substr(pos + 1);
}

inline std::string extension_of(const std::string& p_name)
{
  auto const pos = p_name.rfind('.');
  return pos == std::string::npos ? std::string() : lower_ascii(p_name.substr(pos + 1));
}

// 128 random bits as 32 hex digits.
inline std::string random_name()
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

// True for names upload() generates: 32 hex digits, '.', extension.
inline bool is_stored_name(const std::string& p_name)
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

// Content-Disposition value (RFC 6266): an ASCII fallback in filename= and
// the exact UTF-8 name in filename*= (RFC 5987 percent encoding).
inline std::string content_disposition(bool p_inline, const std::string& p_file_name)
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

inline const file_format* find_format(const std::vector<file_format>& p_formats,
                                      const std::string&              p_ext)
{
  for (auto const& f : p_formats)
    if (f.has_extension(p_ext))
      return &f;
  return nullptr;
}

} // namespace detail

// Receives a file sent as the raw request body:
//
//   POST /upload?name=<original file name>
//   Content-Length: <size>
//   <file bytes>
//
// The server checks authorization, method, size and the number of uploads
// in progress before the body is received (see server.hpp). This provider
// then checks the format, writes the file to "<directory>/tmp", renames it
// into <directory> and calls the handler.
class upload_provider : public response_provider
{
public:
  upload_provider(upload_options p_options, upload_handler p_handler)
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

  const upload_options& options() const { return options_; }

  // Held while an upload is received and handled; see try_acquire().
  class slot
  {
  public:
    explicit slot(std::atomic<int>& p_count) : count_(p_count) {}
    ~slot() { --count_; }
    slot(const slot&) = delete;
    slot& operator=(const slot&) = delete;
  private:
    std::atomic<int>& count_;
  };

  // Reserves one of max_concurrent upload slots; nullptr if all are in use.
  std::unique_ptr<slot> try_acquire() const
  {
    if (++active_ > options_.max_concurrent)
    {
      --active_;
      return nullptr;
    }
    return std::make_unique<slot>(active_);
  }

  std::optional<std::string>
  content(const std::string&  request_body,
          const query&        request_query) const override
  {
    auto const name_it = request_query.find("name");
    if (name_it == request_query.end() || !name_it->second || name_it->second->empty())
      return make_result(0, "missing file name");

    std::string const original = detail::base_name(*name_it->second);
    std::string const ext      = detail::extension_of(original);

    if (request_body.empty())
      return make_result(0, "empty file");

    if (request_body.size() > options_.max_size)
      return make_result(0, "file too large");

    const file_format* format = detail::find_format(options_.formats, ext);
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
    std::string const id      = detail::random_name();
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

  string_view mime_type() const override { return "application/json"; }

  types type() const override { return types::upload; }

private:
  std::filesystem::path temp_directory() const
  {
    return std::filesystem::path(options_.directory) / "tmp";
  }

  upload_options              options_;
  upload_handler              handler_;
  mutable std::atomic<int>    active_{0};
};

// Serves files stored by upload(). The resolver maps the request to a stored
// file (typically by an id the application looked up). The stored name is
// checked again here, so a resolver bug cannot expose other files.
//
// Content-Type comes from the format of the stored file, never from the
// client. Formats that are not inline_safe (e.g. SVG, office documents) are
// sent as attachments.
class download_provider : public response_provider
{
public:
  download_provider(upload_options p_options, download_resolver p_resolver)
  : options_(std::move(p_options))
  , resolver_(std::move(p_resolver))
  {
    if (options_.directory.empty() || options_.formats.empty() || !resolver_)
      throw std::invalid_argument("download: directory, formats and resolver are required");
  }

  std::optional<response_data>
  produce(const std::string&  /* request_body */,
          const query&        request_query) const override
  {
    auto const info = resolver_(request_query);
    if (!info)
      return std::nullopt;

    const file_format* format = detail::is_stored_name(info->stored_name)
      ? detail::find_format(options_.formats, detail::extension_of(info->stored_name))
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
    r.content_disposition = detail::content_disposition
    (
      format->inline_safe,
      info->original_name.empty() ? info->stored_name : info->original_name
    );
    return r;
  }

  std::optional<std::string>
  content(const std::string& request_body, const query& request_query) const override
  {
    auto r = produce(request_body, request_query);
    if (!r)
      return std::nullopt;
    return std::move(r->body);
  }

  string_view mime_type() const override { return "application/octet-stream"; }

  types type() const override { return types::download; }

private:
  upload_options      options_;
  download_resolver   resolver_;
};

inline response_provider* upload(upload_options p_options, upload_handler p_handler)
{
  return new upload_provider(std::move(p_options), std::move(p_handler));
}

inline response_provider* download(upload_options p_options, download_resolver p_resolver)
{
  return new download_provider(std::move(p_options), std::move(p_resolver));
}

inline response_provider* text(const generator& g)
{
  return new text_provider(g);
}

inline json_rpc_provider* json_rpc()
{
  return new json_rpc_provider();
}

inline response_provider* file(std::string path)
{
  return new file_provider(std::move(path));
}

using response_map =
  std::map<
    std::string,
    std::unique_ptr<response_provider>
  >;

inline void add(response_map&      m,
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

// ASKHTTP_PROVIDER_HPP
#endif
