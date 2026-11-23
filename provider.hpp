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
// server.hpp. The implementation is in provider.cpp.
//

#ifndef ASKHTTP_PROVIDER_HPP
#define ASKHTTP_PROVIDER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "file_format.hpp"
#include "json_rpc.hpp"
#include "json_writer.hpp"
#include "log.hpp"
#include "query.hpp"
#include "string_view.hpp"

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

  virtual ~response_provider();

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
          const query&       request_query) const;

  // Entry point used by the server. The default ignores headers and calls
  // produce(); providers that need headers or set the status (mcp)
  // override it.
  virtual std::optional<response_data>
  respond(const request_context& ctx) const;

  // Cross-origin (CORS) access from browser pages. When this returns true
  // for the request's Origin header, the server answers the browser's
  // preflight (OPTIONS) request and adds Access-Control-Allow-Origin to the
  // responses of this resource. Default: no cross-origin access.
  virtual bool cors_allowed(std::string_view origin) const;
};

struct text_provider : response_provider
{
  explicit text_provider(generator p_generator);

  std::optional<std::string>
  content(const std::string&  request_body,
          const query&        request_query) const override;

  string_view mime_type() const override;

  types type() const override;

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
  // Throws std::invalid_argument if p_handler is empty.
  void add(std::string p_method, rpc_handler p_handler);

  // The handler of a registered method, e.g. to expose it as an MCP tool.
  // Throws std::out_of_range if the method is not registered.
  const rpc_handler& handler(const std::string& p_method) const;

  std::optional<response_data>
  produce(const std::string&  request_body,
          const query&        request_query) const override;

  std::optional<std::string>
  content(const std::string& request_body, const query& request_query) const override;

  string_view mime_type() const override;

  types type() const override;

private:
  // Handles one request; an empty optional for notifications.
  std::optional<boost::json::object> call(const boost::json::value& p_request) const;

  std::map<std::string, rpc_handler> methods_;
};

struct file_provider : response_provider
{
  explicit file_provider(std::string path);

  std::optional<std::string>
  content(const std::string&  request_body,
          const query&        request_query) const override;

  string_view mime_type() const override;

  types type() const override;

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

// True for names upload() generates: 32 hex digits, '.', extension.
// Internal; exposed for tests.
bool is_stored_name(const std::string& p_name);

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
  // Throws std::invalid_argument for invalid options and
  // std::filesystem::filesystem_error if "<directory>/tmp" cannot be
  // created.
  upload_provider(upload_options p_options, upload_handler p_handler);

  const upload_options& options() const;

  // Held while an upload is received and handled; see try_acquire().
  class slot
  {
  public:
    explicit slot(std::atomic<int>& p_count);
    ~slot();
    slot(const slot&) = delete;
    slot& operator=(const slot&) = delete;
  private:
    std::atomic<int>& count_;
  };

  // Reserves one of max_concurrent upload slots; nullptr if all are in use.
  std::unique_ptr<slot> try_acquire() const;

  std::optional<std::string>
  content(const std::string&  request_body,
          const query&        request_query) const override;

  string_view mime_type() const override;

  types type() const override;

private:
  std::filesystem::path temp_directory() const;

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
  // Throws std::invalid_argument if directory, formats or resolver is
  // missing.
  download_provider(upload_options p_options, download_resolver p_resolver);

  std::optional<response_data>
  produce(const std::string&  request_body,
          const query&        request_query) const override;

  std::optional<std::string>
  content(const std::string& request_body, const query& request_query) const override;

  string_view mime_type() const override;

  types type() const override;

private:
  upload_options      options_;
  download_resolver   resolver_;
};

response_provider* upload(upload_options p_options, upload_handler p_handler);

response_provider* download(upload_options p_options, download_resolver p_resolver);

response_provider* text(const generator& g);

json_rpc_provider* json_rpc();

response_provider* file(std::string path);

using response_map =
  std::map<
    std::string,
    std::unique_ptr<response_provider>
  >;

void add(response_map&      m,
         std::string        path,
         response_provider  *p);

// ASKHTTP_PROVIDER_HPP
#endif
