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
// Sessions of the HTTP/HTTPS server, based on the Boost.Beast examples: a
// session per connection reads requests and answers them with the response
// provider registered for the path (see provider.hpp).
//
// Internal header: applications must not include it; use server.hpp.
// Only server.cpp and the tests include it. It instantiates the Beast/Asio
// session machinery that server.hpp keeps out of application code.
//

#ifndef ASKHTTP_SERVER_SESSION_HPP
#define ASKHTTP_SERVER_SESSION_HPP

// Boost.Beast/Asio first, before this project's global declarations.
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "server.hpp"
#include "log.hpp"
#include "parse_uri.hpp"
#include "string_view.hpp"

//------------------------------------------------------------------------------

// Report a failure
// Reports a failed I/O operation on a single connection. These are usually
// caused by the client (disconnect, timeout, a scanner sending garbage), so
// they are logged at debug level.
inline void
fail(boost::system::error_code ec, char const* what)
{
  log_debug(what, ": ", ec.message());
}

namespace detail
{

// State shared by the listener, the sessions and the shutdown monitor.
struct server_state
{
  std::atomic<int>  active_sessions{0};
  std::atomic<bool> stopping{false};
  authorizer        authorize;  // empty: every request is allowed
};

} // namespace detail

// Handles an HTTP server connection.
//
// Stream is either boost::beast::ssl_stream<boost::beast::tcp_stream> (HTTPS)
// or boost::beast::tcp_stream (plain HTTP). Only the TLS handshake and the
// shutdown differ between the two; request handling is shared.
template<class Stream>
class basic_session : public std::enable_shared_from_this<basic_session<Stream>>
{
    static constexpr bool is_tls =
      std::is_same_v<Stream, boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    // This is the C++11 equivalent of a generic lambda.
    // The function object is used to send an HTTP message.
    struct send_lambda
    {
        basic_session& self_;

        explicit
        send_lambda(basic_session& self)
            : self_(self)
        {
        }

        template<bool isRequest, class Body, class Fields>
        void
        operator()(http::message<isRequest, Body, Fields>&& msg) const
        {
            // The lifetime of the message has to extend
            // for the duration of the async operation so
            // we use a shared_ptr to manage it.
            auto sp = std::make_shared<
                http::message<isRequest, Body, Fields>>(std::move(msg));

            // Store a type-erased version of the shared
            // pointer in the class to keep it alive.
            self_.res_ = sp;

            // Write the response
            http::async_write(
              self_.stream_,
              *sp,
              boost::beast::bind_front_handler(
                &basic_session::on_write,
                self_.shared_from_this(),
//                std::placeholders::_1,
//                std::placeholders::_2,
                sp->need_eof())
            );
        }
    };

    Stream                              stream_;
    boost::beast::flat_buffer           buffer_;
    std::shared_ptr<response_map const> responses_;
    std::shared_ptr<detail::server_state> state_;

    // Parser of the request being read. Headers are read first, so limits
    // and authorization can be decided before the body arrives.
    std::optional<http::request_parser<http::string_body>> parser_;

    // Set while an upload is received and handled.
    std::unique_ptr<upload_provider::slot> upload_slot_;

    // The current request was authorized from its headers (uploads).
    bool pre_authorized_ = false;

    // Body limit of requests other than uploads (Beast's default).
    static constexpr std::uint64_t default_body_limit = 1024 * 1024;

    // Lingering close after reject_early(), see do_drain().
    bool                    drain_before_close_ = false;
    std::size_t             drained_ = 0;
    std::array<char, 8192>  drain_buffer_;
    std::shared_ptr<void>               res_;
    send_lambda                         lambda_;

    // Builds the stream; the TLS stream additionally needs the context.
    static Stream
    make_stream(tcp::socket&& socket, ssl::context* ctxt)
    {
      if constexpr (is_tls)
        return Stream(std::move(socket), *ctxt);
      else
        return Stream(std::move(socket));
    }

public:
  // Take ownership of the socket.
  // ctxt is required for HTTPS sessions and ignored for plain HTTP ones.
  explicit
  basic_session
  (
    tcp::socket&&     socket,
    ssl::context*     ctxt,
    std::shared_ptr<response_map const> const& responses,
    std::shared_ptr<detail::server_state> state
  ) : stream_(make_stream(std::move(socket), ctxt))
    , responses_(responses)
    , state_(std::move(state))
    , lambda_(*this)
  {
    ++state_->active_sessions;
  }

  ~basic_session()
  {
    --state_->active_sessions;
  }

  // Start the asynchronous operation
  void
  run()
  {
    // We need to be executing within a strand to perform async operations
    // on the I/O objects in this session. Although not strictly necessary
    // for single-threaded contexts, this example code is written to be
    // thread-safe by default.
    net::dispatch
    (
      stream_.get_executor(),
      boost::beast::bind_front_handler
      (
        &basic_session::on_run,
        this->shared_from_this()
      )
    );
  }

  void
  on_run()
  {
    if constexpr (is_tls)
    {
      // Set the timeout.
      boost::beast::get_lowest_layer(stream_).expires_after(
        std::chrono::seconds(30));

      // Perform the SSL handshake
      stream_.async_handshake
      (
        ssl::stream_base::server,
        boost::beast::bind_front_handler
        (
          &basic_session::on_handshake,
          this->shared_from_this()
        )
      );
    }
    else
    {
      // Plain HTTP: no handshake, start reading right away.
      do_read();
    }
  }

  void
  on_handshake(boost::beast::error_code ec)
  {
    if(ec)
      return fail(ec, "handshake");

    do_read();
  }
    
  void
  do_read()
  {
    // A fresh parser for every request. No body limit while the headers
    // are read: Beast would otherwise compare Content-Length with its
    // 1 MB default and refuse an upload before on_header() sees the path.
    // The limit is set in on_header().
    parser_.emplace();
    parser_->body_limit(boost::none);
    upload_slot_.reset();
    pre_authorized_ = false;

    // Set the timeout.
    boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

    http::async_read_header
    (
      stream_,
      buffer_,
      *parser_,
      boost::beast::bind_front_handler
      (
        &basic_session::on_header,
        this->shared_from_this()
      )
    );
  }

  // Sends a response to a request whose body is not read, and closes the
  // connection afterwards: the unread body would otherwise be taken for the
  // next request.
  void
  reject_early(http::status   p_status,
               string_view    p_text,
               string_view    p_header_name  = {},
               string_view    p_header_value = {})
  {
    http::response<http::string_body> res{p_status, parser_->get().version()};
    res.set(http::field::content_type, "text/plain");
    res.set("X-Content-Type-Options", "nosniff");
    if (!p_header_name.empty())
      res.set(p_header_name, p_header_value);
    res.keep_alive(false);
    res.body() = to_std_string(p_text);
    res.prepare_payload();

    log_debug(to_std_string(parser_->get().method_string()), ' ',
              to_std_string(parser_->get().target().substr(0, parser_->get().target().find('?'))),
              " -> ", res.result_int(), " (before body)");

    upload_slot_.reset();
    pre_authorized_ = false;
    drain_before_close_ = true;
    lambda_(std::move(res));
  }

  // Lingering close. After an early response the client may still be
  // sending the body; closing at once would make the kernel reset the
  // connection, and the client could lose the response (browsers then
  // report a network error instead of 401/413). So the incoming data is
  // read and discarded for at most 2 seconds or 32 MB, then the connection
  // is closed.
  void
  do_drain(std::size_t p_bytes)
  {
    drained_ += p_bytes;

    if (p_bytes == 0 && drained_ == 0)
      boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(2));

    if (drained_ > 32 * 1024 * 1024)
      return do_close();

    stream_.async_read_some
    (
      net::buffer(drain_buffer_),
      [self = this->shared_from_this()](boost::beast::error_code ec, std::size_t n)
      {
        if (ec)
          return self->do_close();   // client finished, closed, or 2 s passed
        self->do_drain(n);
      }
    );
  }

  void
  on_header(boost::beast::error_code ec,
            std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    if(ec == http::error::end_of_stream)
      return do_close();

    if(ec)
      return fail(ec, "read header");

    auto const& header = parser_->get();
    auto const  length = parser_->content_length();

    std::string path;
    query q;
    parse_uri(to_std_string(header.target()), path, q);

    auto const it = responses_->find(path);
    auto const* up = it == responses_->end()
      ? nullptr
      : dynamic_cast<const upload_provider*>(it->second.get());

    if (!up)
    {
      // Not an upload: Beast's usual limit. A larger declared body is
      // refused now; a chunked body is stopped at the limit (on_read()).
      if (length && *length > default_body_limit)
        return reject_early(http::status::payload_too_large, "Payload too large");

      parser_->body_limit(default_body_limit);
    }
    else
    {
      // Upload: everything that can be decided from the headers is decided
      // before the body is received.
      if (state_->authorize)
      {
        auth_result decision = auth_result::forbidden;
        try
        {
          decision = state_->authorize(request_info(header, path, q));
        }
        catch(std::exception const& e)
        {
          log_error("authorizer failed: ", e.what());
          return reject_early(http::status::internal_server_error, "Internal server error");
        }
        catch(...)
        {
          log_error("authorizer failed");
          return reject_early(http::status::internal_server_error, "Internal server error");
        }

        if (decision == auth_result::unauthorized)
          return reject_early(http::status::unauthorized, "Unauthorized", "WWW-Authenticate", "Bearer");
        if (decision == auth_result::forbidden)
          return reject_early(http::status::forbidden, "Forbidden");

        pre_authorized_ = true;
      }

      if (header.method() != http::verb::post)
        return reject_early(http::status::method_not_allowed, "Method not allowed", "Allow", "POST");

      if (!length)
        return reject_early(http::status::length_required, "Length required");

      if (*length > up->options().max_size)
        return reject_early(http::status::payload_too_large, "Payload too large");

      upload_slot_ = up->try_acquire();
      if (!upload_slot_)
        return reject_early(http::status::service_unavailable,
                            "Too many uploads in progress", "Retry-After", "5");

      parser_->body_limit(up->options().max_size);
      boost::beast::get_lowest_layer(stream_).expires_after(up->options().timeout);
    }

    http::async_read
    (
      stream_,
      buffer_,
      *parser_,
      boost::beast::bind_front_handler
      (
        &basic_session::on_read,
        this->shared_from_this()
      )
    );
  }

  void
  on_read(boost::beast::error_code ec,
          std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    // This means they closed the connection
    if(ec == http::error::end_of_stream)
      return do_close();

    // A chunked body went over the limit.
    if(ec == http::error::body_limit)
      return reject_early(http::status::payload_too_large, "Payload too large");

    if(ec)
      return fail(ec, "read");

    // Send the response. The upload slot is held until the response is
    // produced (handle_request() calls the provider synchronously).
    handle_request(parser_->release(), lambda_);
    upload_slot_.reset();
    pre_authorized_ = false;
  }

  void
  on_write(bool close,
           boost::beast::error_code ec,
           std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);

    if(ec)
      return fail(ec, "write");

    if(drain_before_close_)
      return do_drain(0);

    if(close || state_->stopping)
    {
      // This means we should close the connection, usually because
      // the response indicated the "Connection: close" semantic, or the
      // server is shutting down and keeps no idle connections.
      return do_close();
    }

    // We're done with the response so delete it
    res_ = nullptr;

    // Read another request
    do_read();
  }

  void
  do_close()
  {
    if constexpr (is_tls)
    {
      // Set the timeout.
      boost::beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

      // Perform the SSL shutdown
      stream_.async_shutdown
      (
        boost::beast::bind_front_handler
        (
          &basic_session::on_shutdown,
          this->shared_from_this()
        )
      );
    }
    else
    {
      // Plain HTTP: send a TCP shutdown. The connection is closed when the
      // last reference to the session goes away.
      boost::beast::error_code ec;
      stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
  }

  void
  on_shutdown(boost::beast::error_code ec)
  {
    if(ec)
      return fail(ec, "shutdown");
      // At this point the connection is closed gracefully
  }    
  // This function produces an HTTP response for the given
  // request. The type of the response object depends on the
  // contents of the request, so the interface requires the
  // caller to pass a generic lambda for receiving the response.
  template<typename Send>
  void
  handle_request
  (
    http::request<http::string_body>&&  req,
    Send&&                              send
  )
  {
    // No "Server" header is sent: it is optional, and the previous value
    // (the Boost.Beast version) told an attacker which library and version
    // to look up vulnerabilities for.

    // Returns a bad request response
    auto const bad_request =
    [&req](string_view why)
    {
      http::response<http::string_body> res{http::status::bad_request, req.version()};
      // text/plain: the body is never interpreted as HTML by a browser.
      res.set(http::field::content_type, "text/plain");
      res.keep_alive(req.keep_alive());
      res.body() = to_std_string(why);
      res.prepare_payload();
      return res;
    };

    // Returns a not found response
    // The request target is not echoed back: in a text/html response it
    // allowed reflected cross-site scripting (e.g. "/<script>...").
    auto const not_found =
    [&req]()
    {
      http::response<http::string_body> res{http::status::not_found, req.version()};
      res.set(http::field::content_type, "text/plain");
      res.keep_alive(req.keep_alive());
      res.body() = "Not found";
      res.prepare_payload();
      return res;
    };

    // Returns a 401 or 403 response.
    auto const auth_failure =
    [&req](auth_result p_result)
    {
      bool const unauthorized = p_result == auth_result::unauthorized;
      http::response<http::string_body> res
      {
        unauthorized ? http::status::unauthorized : http::status::forbidden,
        req.version()
      };
      res.set(http::field::content_type, "text/plain");
      // RFC 9110 requires WWW-Authenticate on 401. "Bearer" does not make
      // browsers show a login dialog (unlike "Basic").
      if (unauthorized)
        res.set(http::field::www_authenticate, "Bearer");
      res.keep_alive(req.keep_alive());
      res.body() = unauthorized ? "Unauthorized" : "Forbidden";
      res.prepare_payload();
      return res;
    };

    auto const internal_error =
    [&req]()
    {
      http::response<http::string_body> res{http::status::internal_server_error, req.version()};
      res.set(http::field::content_type, "text/plain");
      res.keep_alive(req.keep_alive());
      res.body() = "Internal server error";
      res.prepare_payload();
      return res;
    };

    // Returns a server error response
//    auto const server_error =
//    [&req](string_view what)
//    {
//      http::response<http::string_body> res{http::status::internal_server_error, req.version()};
//      res.set(http::field::content_type, "text/plain");
//      res.keep_alive(req.keep_alive());
//      res.body() = "An error occurred: '" + to_std_string(what) + "'";
//      res.prepare_payload();
//      return res;
//    };

    // Returns a "405 Method Not Allowed" response. The Allow header lists
    // the methods the resource supports (required by RFC 9110).
    auto const method_not_allowed =
    [&req](string_view allow)
    {
      http::response<http::string_body> res{http::status::method_not_allowed, req.version()};
      res.set(http::field::allow, allow);
      res.set(http::field::content_type, "text/plain");
      res.keep_alive(req.keep_alive());
      res.body() = "Method not allowed";
      res.prepare_payload();
      return res;
    };

    // Set when the request comes from a browser page whose origin the
    // requested resource allows (CORS); every response then carries
    // Access-Control-Allow-Origin, errors such as 401 included, so the page
    // sees the real status instead of a network error.
    std::string cors_origin;

    // Sends a response. A response to a HEAD request must not carry a
    // body, whatever its status: on a keep-alive connection the client
    // would read those bytes as the start of the next response. The body
    // is dropped here and the header fields, Content-Length included, are
    // kept, so HEAD reports exactly what GET would send.
    auto const reply =
    [&req, &send, &cors_origin](auto&& res)
    {
      if (!cors_origin.empty())
      {
        res.set("Access-Control-Allow-Origin", cors_origin);
        res.set("Vary", "Origin");
      }

      // Access log at debug level: method, path (without the query, which
      // may carry personal data) and status. Bodies are never logged.
      if (log_enabled(log_level::debug))
      {
        auto const target = req.target();
        log_debug(to_std_string(req.method_string()), ' ',
                  to_std_string(target.substr(0, target.find('?'))),
                  " -> ", res.result_int());
      }

      // Browsers must not guess another type than Content-Type (e.g. treat
      // an uploaded text file as HTML).
      res.set("X-Content-Type-Options", "nosniff");

      if (req.method() == http::verb::head)
      {
        http::response<http::empty_body> head;
        head.base() = res.base();
        send(std::move(head));
      }
      else
      {
        send(std::move(res));
      }
    };

    // Produces the response; the caller clears the request body afterwards.
    auto const produce_response =
    [&]
    {
      // Request path must be absolute and not contain "..".
      if( req.target().empty() ||
          req.target()[0] != '/' ||
          req.target().find("..") != string_view::npos)
        return reply(bad_request("Illegal request-target"));

      std::string path;
      query q;
      parse_uri(to_std_string(req.target()), path, q);

      // Cross-origin access, decided by the resource (see
      // response_provider::cors_allowed()).
      {
        auto const origin = req["Origin"];
        auto const target = responses_->find(path);
        if (!origin.empty() && target != responses_->end() &&
            target->second->cors_allowed(std::string_view(origin.data(), origin.size())))
          cors_origin = to_std_string(origin);
      }

      // CORS preflight: before a cross-origin POST with JSON or custom
      // headers, the browser sends OPTIONS without credentials, so it is
      // answered before authorization. Only origins allowed by the resource
      // get a positive answer; the browser then sends the real request,
      // which is authorized as usual.
      if (req.method() == http::verb::options && !req["Access-Control-Request-Method"].empty())
      {
        if (cors_origin.empty())
          return reply(auth_failure(auth_result::forbidden));

        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.set("Access-Control-Allow-Methods", "POST");
        // The requested header names are echoed if they are plain header
        // names (letters, digits, '-', separated by commas and spaces).
        auto const wanted = req["Access-Control-Request-Headers"];
        bool const plain = std::all_of(wanted.begin(), wanted.end(), [](char c)
        {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == ',' || c == ' ';
        });
        res.set("Access-Control-Allow-Headers",
                plain && !wanted.empty()
                  ? to_std_string(wanted)
                  : std::string("Content-Type, Authorization, MCP-Protocol-Version, Mcp-Method, Mcp-Name"));
        res.set("Access-Control-Max-Age", "600");
        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        return reply(std::move(res));
      }

      // Authorization comes before the path lookup, so unknown paths are
      // not revealed to clients without access.
      // Upload requests were already authorized from their headers, before
      // the body was received (see on_header()).
      if (state_->authorize && !pre_authorized_)
      {
        auth_result decision = auth_result::forbidden;

        try
        {
          decision = state_->authorize(request_info(req, path, q));
        }
        catch(std::exception const& e)
        {
          log_error("authorizer failed: ", e.what());
          return reply(internal_error());
        }
        catch(...)
        {
          log_error("authorizer failed");
          return reply(internal_error());
        }

        if (decision != auth_result::allow)
          return reply(auth_failure(decision));
      }

      auto it = responses_->find(path);
      if (it == responses_->end())
        return reply(not_found());

      auto const& provider = it->second;

      // JSON resources answer POST; text and file resources answer GET
      // and HEAD. Any other method gets 405 (previously 400, and a HEAD
      // request got no response at all).
      // JSON-RPC, MCP and upload resources answer POST; the others GET
      // and HEAD.
      auto const type = provider->type();
      bool const post_only = type == response_provider::types::json ||
                             type == response_provider::types::mcp ||
                             type == response_provider::types::upload;
      bool const allowed = post_only
        ? req.method() == http::verb::post
        : req.method() == http::verb::get || req.method() == http::verb::head;

      if (!allowed)
        return reply(method_not_allowed(post_only ? "POST" : "GET, HEAD"));

      // Providers are stateless and produce() returns the response by value,
      // so concurrent requests to the same path do not interfere. The
      // response is produced first, in its own statement, so it does not
      // depend on argument evaluation order.
      std::string const method = to_std_string(req.method_string());
      request_context const ctx
      {
        method,
        req.body(),
        q,
        [&req](std::string_view name)
        {
          auto const v = req[boost::beast::string_view(name.data(), name.size())];
          return std::string(v.data(), v.size());
        }
      };

      auto r = provider->respond(ctx);
      if (!r)
        return reply(not_found());

      http::response<http::string_body> res
      {
        static_cast<http::status>(r->status), req.version()
      };
      if (!r->content_type.empty())
        res.set(http::field::content_type, r->content_type);
      if (!r->content_disposition.empty())
        res.set(http::field::content_disposition, r->content_disposition);
      res.keep_alive(req.keep_alive());
      res.body() = std::move(r->body);
      res.prepare_payload();
      reply(std::move(res));
    };

    produce_response();

    req.body().clear(); // Buna garip bir şekilde ihtiyaç var. Unutulmamalı ki bu yazılımın temel alındığı örnek post metodunu desteklemiyor.
  }

};

using https_session = basic_session<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
using http_session  = basic_session<boost::beast::tcp_stream>;

// ASKHTTP_SERVER_SESSION_HPP
#endif
