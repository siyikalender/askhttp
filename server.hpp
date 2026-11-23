//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//
// Modified by:
//   Selçuk İyikalender
//   Claude (Anthropic)
//
// SPDX-License-Identifier: BSL-1.0
//
// Asynchronous HTTP/HTTPS server, based on the Boost.Beast examples: the
// listener accepts connections, a session per connection reads requests and
// answers them with the response provider registered for the path (see
// provider.hpp). start_server() / start_ssl_server() run the server and
// block until it stops; server_control stops it gracefully. An optional
// authorizer decides per request whether it is allowed.
//
// This header is the public server API. The sessions are in the internal
// header server_session.hpp, the listener and the rest of the
// implementation in server.cpp.
//

#ifndef ASKHTTP_SERVER_HPP
#define ASKHTTP_SERVER_HPP

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <boost/optional.hpp>

#include "string_view.hpp"
#include "query.hpp"

///////////////////////////////////////////////////

using     tcp   = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace http  = boost::beast::http;    // from <boost/beast/http.hpp>
namespace ssl   = boost::asio::ssl;
namespace net   = boost::asio;            // from <boost/asio.hpp>

#include "provider.hpp"

//------------------------------------------------------------------------------

// Lets the application stop a running server.
//
//   server_control control;               // must outlive the server
//   ...
//   start_server(endpoint, threads, responses, &control);   // blocks
//
//   // elsewhere: another thread or a signal handler
//   control.stop();
//
// After stop() the server accepts no new connections, lets requests in
// progress finish, and returns from start_server()/start_ssl_server() when
// no connection is left or grace_period has passed, whichever comes first.
// Connections still open at that point are closed.
class server_control
{
public:
  // Requests a graceful shutdown. Only sets an atomic flag, so it is safe
  // to call from any thread and from a signal handler.
  void stop() noexcept;

  bool stop_requested() const noexcept;

  // Upper limit for waiting for open connections after stop(). Set it
  // before the server is started.
  std::chrono::milliseconds grace_period{5000};

private:
  static_assert(std::atomic<bool>::is_always_lock_free,
                "stop() must be async-signal-safe");

  std::atomic<bool> stop_requested_{false};
};

// Result of an authorization check.
enum class auth_result
{
  allow,          // handle the request
  unauthorized,   // 401: no or invalid credentials
  forbidden       // 403: valid credentials, but not for this request
};

// Read-only view of a request, passed to the authorizer.
class request_info
{
public:
  request_info(const http::request<http::string_body>&  p_req,
               const std::string&                       p_path,
               const query&                             p_query);

  http::verb          method() const;

  // Percent-decoded path without the query, e.g. "/api".
  const std::string&  path()   const;

  // Percent-decoded query parameters.
  const query&        params() const;

  // Value of a request header; empty if the header is missing.
  string_view         header(string_view p_name) const;

  // Request body, e.g. the JSON of a POST request ({"name":...,"data":...}).
  const std::string&  body()   const;

  // The token of an "Authorization: Bearer <token>" header, if present.
  boost::optional<std::string> bearer_token() const;

private:
  const http::request<http::string_body>&  req_;
  const std::string&                       path_;
  const query&                             query_;
};

// Decides whether a request may be handled. Called for every request, before
// the path is looked up, so a client without access cannot find out which
// paths exist. Called concurrently from server threads: it must be
// thread-safe. If it throws, the request is answered with 500.
using authorizer = std::function<auth_result(const request_info&)>;

// Compares two secrets (e.g. tokens) in time independent of where they
// differ, so the comparison does not leak the matching prefix length.
bool constant_time_equals(string_view a, string_view b);

//------------------------------------------------------------------------------

// key_password is the pass phrase of the private key file. It is supplied
// by the caller (e.g. from a configuration file or an environment variable)
// instead of being compiled into the library. An empty string is fine for
// an unencrypted key file.
void load_server_certificate
(
  ssl::context&  ctx,
  string_view    cert_file,
  string_view    key_file,
  std::string    key_password
);

// Starts an HTTPS (SSL/TLS) server. Blocks until the server is stopped
// through control (see server_control); without control it runs until the
// process ends. With an authorizer, every request is checked first (see
// authorizer); without one, every request is allowed.
// Throws boost::system::system_error if the certificate cannot be loaded or
// the endpoint cannot be used (e.g. the port is already in use).
// The response map must be fully populated before this call; providers
// are shared by all server threads and are not modified afterwards.
void start_ssl_server
(
  tcp::endpoint const&                        endpoint,
  const int                                   number_of_threads,
  string_view                                 cert_file,
  string_view                                 key_file,
  std::string                                 key_password,
  std::shared_ptr<response_map>               responses,
  server_control*                             control = nullptr,
  authorizer                                  authorize = {}
);

// Starts a plain HTTP server without TLS. Blocks like start_ssl_server().
// Note: with plain HTTP, credentials such as bearer tokens travel
// unencrypted.
// Throws boost::system::system_error if the endpoint cannot be used
// (e.g. the port is already in use).
// Traffic, including request bodies, is not encrypted. Intended for
// development, for a trusted local network, or for running behind a
// reverse proxy that terminates TLS.
// The same rules for the response map apply as for start_ssl_server().
void start_server
(
  tcp::endpoint const&                        endpoint,
  const int                                   number_of_threads,
  std::shared_ptr<response_map>               responses,
  server_control*                             control = nullptr,
  authorizer                                  authorize = {}
);

// ASKHTTP_SERVER_HPP
#endif
