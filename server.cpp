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
// listener that accepts connections and launches the sessions
// (server_session.hpp), the shutdown monitor, TLS setup and
// start_server() / start_ssl_server(). See server.hpp for the interface.
//

// Boost.Beast/Asio first, before this project's global declarations;
// server_session.hpp includes the rest of them first, too.
#include <boost/asio/steady_timer.hpp>

#include "server_session.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void server_control::stop() noexcept
{
  stop_requested_.store(true);
}

bool server_control::stop_requested() const noexcept
{
  return stop_requested_.load();
}

request_info::request_info(const http::request<http::string_body>&  p_req,
                           const std::string&                       p_path,
                           const query&                             p_query)
: req_(p_req), path_(p_path), query_(p_query)
{}

http::verb request_info::method() const
{
  return req_.method();
}

const std::string& request_info::path() const
{
  return path_;
}

const query& request_info::params() const
{
  return query_;
}

string_view request_info::header(string_view p_name) const
{
  return req_[p_name];
}

const std::string& request_info::body() const
{
  return req_.body();
}

boost::optional<std::string> request_info::bearer_token() const
{
  string_view const h = req_[http::field::authorization];
  string_view const scheme = "Bearer ";

  if (h.size() <= scheme.size() ||
      !boost::beast::iequals(h.substr(0, scheme.size()), scheme))
    return boost::optional<std::string>();

  return to_std_string(h.substr(scheme.size()));
}

bool constant_time_equals(string_view a, string_view b)
{
  unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
  std::size_t const n = a.size() < b.size() ? a.size() : b.size();

  for (std::size_t i = 0; i < n; ++i)
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);

  return diff == 0;
}

namespace
{

// Accepts incoming connections and launches the sessions.
// With a TLS context the sessions speak HTTPS, without one (nullptr) they
// speak plain HTTP.
//
// The acceptor and the retry timer run on their own strand, so stop()
// (called from the shutdown monitor) never runs concurrently with
// on_accept() or on_retry().
class listener : public std::enable_shared_from_this<listener>
{
  net::io_context& ioc_;
  ssl::context*    ctx_;
  // All listener state (acceptor, timer, stopped_) is touched only through
  // this strand. The acceptor itself is built from the io_context; its
  // completion handlers are bound to the strand with bind_executor, which
  // avoids constructing the acceptor from a strand executor (not accepted
  // by every Boost/Asio configuration).
  net::strand<net::io_context::executor_type> strand_;
  tcp::acceptor acceptor_;
  net::steady_timer retry_timer_;
  std::shared_ptr<response_map const>  responses_;
  std::shared_ptr<detail::server_state> state_;
  bool stopped_ = false;  // accessed on strand_ only

public:

  listener
  (
    net::io_context&      ioc,
    ssl::context*         ctx,
    tcp::endpoint         endpoint,
    std::shared_ptr<response_map const> const& responses,
    std::shared_ptr<detail::server_state> state
  ) : ioc_(ioc)
    , ctx_(ctx)
    , strand_(net::make_strand(ioc))
    , acceptor_(ioc)
    , retry_timer_(ioc)
    , responses_(responses)
    , state_(std::move(state))
  {
    // The throwing overloads are used on purpose: if the port cannot be
    // opened (e.g. "bind: Address already in use"), the server must not
    // start. Previously the error was only printed and the following
    // async_accept failed immediately and forever, spinning the CPU.
    // boost::system::system_error propagates out of start_server() /
    // start_ssl_server() to the caller.

    // Open the acceptor
    acceptor_.open(endpoint.protocol());

    // Allow address reuse
    acceptor_.set_option(net::socket_base::reuse_address(true));

    // Bind to the server address
    acceptor_.bind(endpoint);

    // Start listening for connections
    acceptor_.listen(net::socket_base::max_listen_connections);
  }

  // Start accepting incoming connections
  void
  run()
  {
    net::dispatch
    (
      strand_,
      boost::beast::bind_front_handler
      (
        &listener::do_accept,
        shared_from_this()
      )
    );
  }

  // Stops accepting connections. Thread-safe: the work is done on strand_.
  void
  stop()
  {
    net::dispatch
    (
      strand_,
      [self = shared_from_this()]
      {
        self->stopped_ = true;
        boost::beast::error_code ec;
        self->acceptor_.close(ec);
        self->retry_timer_.cancel();
      }
    );
  }

private:
  void
  do_accept()
  {
    // The new connection gets its own strand; on_accept runs on strand_
    acceptor_.async_accept
    (
      net::make_strand(ioc_),
      net::bind_executor
      (
        strand_,
        boost::beast::bind_front_handler
        (
          &listener::on_accept,
          shared_from_this()
        )
      )
    );
  }

  void
  on_accept(boost::beast::error_code ec, tcp::socket socket)
  {
    if(ec)
    {
      // The acceptor was closed: stop accepting.
      if(stopped_ || ec == net::error::operation_aborted || !acceptor_.is_open())
        return;

      log_warning("accept: ", ec.message());

      // Errors such as EMFILE (too many open files) are usually temporary,
      // but accepting again at once fails again at once and spins the CPU.
      // Wait a little before the next attempt.
      retry_timer_.expires_after(accept_retry_delay);
      retry_timer_.async_wait
      (
        net::bind_executor
        (
          strand_,
          boost::beast::bind_front_handler
          (
            &listener::on_retry,
            shared_from_this()
          )
        )
      );
      return;
    }

    // Create the session and run it
    if (ctx_)
      std::make_shared<https_session>(std::move(socket), ctx_, responses_, state_)->run();
    else
      std::make_shared<http_session>(std::move(socket), nullptr, responses_, state_)->run();

    // Accept another connection
    if (!stopped_)
      do_accept();
  }

  void
  on_retry(boost::beast::error_code ec)
  {
    if(ec || stopped_)
      return;

    do_accept();
  }

  static constexpr std::chrono::milliseconds accept_retry_delay{500};
};

// Watches server_control and carries out the graceful shutdown.
// Runs on the io_context; it only touches atomics, the listener (whose
// stop() is thread-safe) and io_context::stop() (thread-safe).
class shutdown_monitor
{
public:
  shutdown_monitor
  (
    net::io_context&                        ioc,
    server_control&                         control,
    std::shared_ptr<listener>               l,
    std::shared_ptr<detail::server_state>   state
    ) : ioc_(ioc)
    , control_(control)
    , listener_(std::move(l))
    , state_(std::move(state))
    , timer_(ioc)
  {}

  void start()
  {
    schedule();
  }

private:
  void schedule()
  {
    timer_.expires_after(poll_interval);
    timer_.async_wait([this](boost::beast::error_code ec)
    {
      if (!ec)
        check();
    });
  }

  void check()
  {
    if (!state_->stopping && control_.stop_requested())
    {
      state_->stopping = true;
      listener_->stop();
      deadline_ = std::chrono::steady_clock::now() + control_.grace_period;
    }

    if (state_->stopping)
    {
      // Done when no connection is left, or when the grace period is over
      // (e.g. idle keep-alive connections waiting for a next request).
      if (state_->active_sessions == 0 ||
          std::chrono::steady_clock::now() >= deadline_)
      {
        ioc_.stop();
        return;
      }
    }

    schedule();
  }

  static constexpr std::chrono::milliseconds poll_interval{100};

  net::io_context&                        ioc_;
  server_control&                         control_;
  std::shared_ptr<listener>               listener_;
  std::shared_ptr<detail::server_state>   state_;
  net::steady_timer                       timer_;
  std::chrono::steady_clock::time_point   deadline_;
};

// Runs the listener on number_of_threads threads and blocks the caller.
// Without a control object the server runs until the process ends.
// ctx == nullptr selects plain HTTP.
void run_server
(
  net::io_context&                            ioc,
  ssl::context*                               ctx,
  tcp::endpoint const&                        endpoint,
  const int                                   number_of_threads,
  std::shared_ptr<response_map>               responses,
  server_control*                             control,
  authorizer                                  authorize
)
{
  auto state = std::make_shared<detail::server_state>();
  state->authorize = std::move(authorize);

  // Create and launch a listening port
  auto l = std::make_shared<listener>
  (
    ioc,
    ctx,
    endpoint,
    responses,
    state
  );
  l->run();

  // Declared before the threads: it must outlive every ioc.run() call.
  std::optional<shutdown_monitor> monitor;
  if (control)
  {
    monitor.emplace(ioc, *control, l, state);
    monitor->start();
  }

  // Run the I/O service on the requested number of threads.
  // The threads are always joined before returning; previously they were
  // never joined, and a returning ioc.run() would have called
  // std::terminate() from the std::thread destructors.
  std::vector<std::thread> v;
  v.reserve(number_of_threads - 1);

  try
  {
    for(auto i = number_of_threads - 1; i > 0; --i)
        v.emplace_back(
        [&ioc]
        {
          ioc.run();
        });
    ioc.run();
  }
  catch(...)
  {
    ioc.stop();
    for (auto& t : v)
      t.join();
    throw;
  }

  for (auto& t : v)
    t.join();
}

} // namespace

void load_server_certificate
(
  ssl::context&  ctx, 
  string_view    cert_file, 
  string_view    key_file,
  std::string    key_password
)
{
  // TLS 1.2 and 1.3 only; SSL 2/3 and TLS 1.0/1.1 are insecure.
  ctx.set_options(ssl::context::default_workarounds
                  | ssl::context::no_sslv2
                  | ssl::context::no_sslv3
                  | ssl::context::no_tlsv1
                  | ssl::context::no_tlsv1_1
                  | ssl::context::single_dh_use);

  // TLS 1.2 cipher suites: forward secrecy (ECDHE) and authenticated
  // encryption (GCM / ChaCha20) only, as in Mozilla's "intermediate"
  // profile. TLS 1.3 suites are configured separately and are secure by
  // default.
  if (SSL_CTX_set_cipher_list
      (
        ctx.native_handle(),
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305"
      ) != 1)
  {
    throw std::runtime_error("TLS cipher list could not be set");
  }

  ctx.set_password_callback
  (
    [password = std::move(key_password)]
    (std::size_t, ssl::context::password_purpose)
    {
      return password;
    }
  );
  // to_std_string(): data() of a string_view is not guaranteed to be
  // NUL-terminated, so it must not be passed where a C string is expected.
  //
  // use_certificate_chain_file: the file may hold the server certificate
  // followed by intermediate certificates (e.g. "fullchain.pem"); a single
  // certificate works as before.
  // use_private_key_file: accepts RSA and EC keys (previously RSA only).
  ctx.use_certificate_chain_file(to_std_string(cert_file));
  ctx.use_private_key_file(to_std_string(key_file), ssl::context::pem);
// ctx.use_tmp_dh_file("dh2048.pem");  
}

void start_ssl_server
(
  tcp::endpoint const&                        endpoint,
  const int                                   number_of_threads,
  string_view                                 cert_file,
  string_view                                 key_file,
  std::string                                 key_password,
  std::shared_ptr<response_map>               responses,
  server_control*                             control,
  authorizer                                  authorize
)
{
  // Declared before the io_context so that it is destroyed after it:
  // handlers still queued in the io_context may refer to TLS streams.
  // tls_server negotiates the highest version both sides support; the
  // options in load_server_certificate() limit it to TLS 1.2 and 1.3.
  // (tlsv12 allowed TLS 1.2 only.)
  ssl::context ctx{ssl::context::tls_server};

  load_server_certificate(ctx, cert_file, key_file, std::move(key_password));

  // The io_context is required for all I/O
  net::io_context ioc{number_of_threads};

  run_server(ioc, &ctx, endpoint, number_of_threads, std::move(responses), control, std::move(authorize));
}

void start_server
(
  tcp::endpoint const&                        endpoint,
  const int                                   number_of_threads,
  std::shared_ptr<response_map>               responses,
  server_control*                             control,
  authorizer                                  authorize
)
{
  // The io_context is required for all I/O
  net::io_context ioc{number_of_threads};

  run_server(ioc, nullptr, endpoint, number_of_threads, std::move(responses), control, std::move(authorize));
}
