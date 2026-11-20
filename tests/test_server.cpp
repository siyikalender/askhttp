//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// End-to-end tests: a real server on 127.0.0.1 and a plain socket client.
// Port: ASKHTTP_TEST_PORT (default 18089).
//

#include <boost/test/unit_test.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <thread>

#include <boost/system/system_error.hpp>

#include "server.hpp"
#include "mcp.hpp"

namespace
{

using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

unsigned short test_port()
{
  const char* p = std::getenv("ASKHTTP_TEST_PORT");
  return static_cast<unsigned short>(p ? std::atoi(p) : 18089);
}

// Minimal blocking client socket.
struct client
{
  int fd = -1;

  ~client() { close(); }

  bool connect(unsigned short port)
  {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
      close();
      return false;
    }
    timeval tv{5, 0};   // never hang the test run
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
  }

  // MSG_NOSIGNAL: a closed connection gives an error, not SIGPIPE (which
  // would end the test process).
  void send(const std::string& data)
  {
    std::size_t sent = 0;
    while (sent < data.size())
    {
      ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0)
        return;
      sent += static_cast<std::size_t>(n);
    }
  }

  // Reads until the peer closes the connection (or the timeout).
  std::string read_all()
  {
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
      out.append(buf, static_cast<std::size_t>(n));
    return out;
  }

  // Reads one response whose body length is known.
  std::string read_response(std::size_t body_length)
  {
    std::string out;
    char buf[4096];
    while (true)
    {
      auto const end = out.find("\r\n\r\n");
      if (end != std::string::npos && out.size() >= end + 4 + body_length)
        return out;
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0)
        return out;
      out.append(buf, static_cast<std::size_t>(n));
    }
  }

  void close()
  {
    if (fd >= 0)
      ::close(fd);
    fd = -1;
  }
};

// Runs start_server() on a thread; stops and joins it when destroyed.
//
// Resources: /hello (text), /upload (PDF up to 1000 bytes, one at a time)
// and /secure-upload (the same, but requires "Authorization: Bearer t0ken").
struct running_server
{
  server_control  control;
  std::thread     thread;
  std::string     error;
  double          stop_to_return_ms = -1;
  clock_type::time_point stopped_at;
  std::filesystem::path  upload_dir;

  explicit running_server(std::chrono::milliseconds grace = 2000ms)
  {
    control.grace_period = grace;

    std::random_device rd;
    upload_dir = std::filesystem::temp_directory_path() / ("askhttp_e2e_" + std::to_string(rd()));
    upload_options opts{.max_size = 1000, .directory = upload_dir.string(),
                        .formats = {file_format::pdf}, .max_concurrent = 1};
    auto stored = [](const uploaded_file&) { return make_result(1, 1); };

    auto responses = std::make_shared<response_map>();
    add(*responses, "/hello", text([](const query&) { return std::string("hello world"); }));
    add(*responses, "/upload", upload(opts, stored));
    add(*responses, "/secure-upload", upload(opts, stored));

    auto* m = mcp({.name = "e2e", .version = "1"});
    m->add_tool({.name = "ping", .description = "Answers pong."},
                [](const boost::json::value&) { return boost::json::value("pong"); });
    add(*responses, "/mcp", m);

    authorizer auth = [](const request_info& r)
    {
      if (r.path() != "/secure-upload")
        return auth_result::allow;
      auto t = r.bearer_token();
      return t && *t == "t0ken" ? auth_result::allow : auth_result::unauthorized;
    };

    thread = std::thread([this, responses, auth]
    {
      try
      {
        start_server(tcp::endpoint(net::ip::make_address("127.0.0.1"), test_port()),
                     2, responses, &control, auth);
      }
      catch (std::exception const& e)
      {
        error = e.what();
      }
      if (stopped_at != clock_type::time_point{})
        stop_to_return_ms =
          std::chrono::duration<double, std::milli>(clock_type::now() - stopped_at).count();
    });

    // Wait until the port accepts connections.
    for (int i = 0; i < 100; ++i)
    {
      client c;
      if (c.connect(test_port()))
        return;
      std::this_thread::sleep_for(20ms);
    }
  }

  void stop()
  {
    if (!thread.joinable())
      return;
    stopped_at = clock_type::now();
    control.stop();
    thread.join();
  }

  ~running_server()
  {
    stop();
    std::error_code ec;
    std::filesystem::remove_all(upload_dir, ec);
  }
};

// Status line of the first response, e.g. "HTTP/1.1 413 Payload Too Large".
std::string status_line(const std::string& response)
{
  return response.substr(0, response.find("\r\n"));
}

std::string upload_request(const std::string& path, std::size_t length,
                           const std::string& extra_headers = "")
{
  return "POST " + path + "?name=a.pdf HTTP/1.1\r\nHost: t\r\n" + extra_headers +
         "Content-Length: " + std::to_string(length) + "\r\n\r\n";
}

bool starts_with(const std::string& s, const std::string& prefix)
{
  return s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

BOOST_AUTO_TEST_SUITE(server)

// server: GET and HEAD over a real connection
BOOST_AUTO_TEST_CASE(get_and_head, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv;
  BOOST_CHECK_EQUAL(srv.error, "");

  client a;
  BOOST_CHECK(a.connect(test_port()));
  a.send("GET /hello HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
  auto get = a.read_all();
  BOOST_CHECK(starts_with(get, "HTTP/1.1 200"));
  BOOST_CHECK(get.size() >= 11 && get.substr(get.size() - 11) == "hello world");

  client b;
  BOOST_CHECK(b.connect(test_port()));
  b.send("HEAD /hello HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
  auto head = b.read_all();
  BOOST_CHECK(starts_with(head, "HTTP/1.1 200"));
  BOOST_CHECK(head.find("Content-Length: 11") != std::string::npos);
  BOOST_CHECK(head.size() >= 4 && head.substr(head.size() - 4) == "\r\n\r\n");   // no body

  srv.stop();
  set_log_sink({});
}

// server: port already in use throws instead of spinning
BOOST_AUTO_TEST_CASE(port_in_use_throws, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server first;

  auto responses = std::make_shared<response_map>();
  bool threw = false;
  try
  {
    start_server(tcp::endpoint(net::ip::make_address("127.0.0.1"), test_port()), 1, responses);
  }
  catch (boost::system::system_error const&)
  {
    threw = true;
  }
  BOOST_CHECK(threw);

  first.stop();
  set_log_sink({});
}

// server: stop() without connections returns quickly
BOOST_AUTO_TEST_CASE(stop_without_connections, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv(5000ms);
  srv.stop();
  BOOST_CHECK_EQUAL(srv.error, "");
  BOOST_CHECK_GE(srv.stop_to_return_ms, 0.0);
  BOOST_CHECK_LT(srv.stop_to_return_ms, 1000.0);
  set_log_sink({});
}

// server: stop() waits at most grace_period for idle connections
BOOST_AUTO_TEST_CASE(stop_with_idle_connection, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv(1000ms);

  client idle;   // keep-alive connection that stays open after a request
  BOOST_CHECK(idle.connect(test_port()));
  idle.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
  BOOST_CHECK(starts_with(idle.read_response(11), "HTTP/1.1 200"));

  srv.stop();
  BOOST_CHECK_GE(srv.stop_to_return_ms, 900.0);
  BOOST_CHECK_LT(srv.stop_to_return_ms, 2500.0);
  set_log_sink({});
}

// upload over a real connection
BOOST_AUTO_TEST_CASE(upload_file, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv;

  std::string const pdf = "%PDF-1.4 small";
  client c;
  BOOST_REQUIRE(c.connect(test_port()));
  c.send(upload_request("/upload", pdf.size(), "Connection: close\r\n") + pdf);
  auto const r = c.read_all();
  BOOST_CHECK_EQUAL(status_line(r), "HTTP/1.1 200 OK");
  BOOST_CHECK(r.find(R"({"status":1,"what":1})") != std::string::npos);

  srv.stop();
  set_log_sink({});
}

// uploads are refused from their headers alone: no body is sent here, and
// the response still arrives
BOOST_AUTO_TEST_CASE(upload_refused_before_body, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv;

  struct { const char* name; std::string request; const char* status; } cases[] =
  {
    {"too large",    upload_request("/upload", 5000),               "HTTP/1.1 413 Payload Too Large"},
    {"no token",     upload_request("/secure-upload", 10),          "HTTP/1.1 401 Unauthorized"},
    {"wrong token",  upload_request("/secure-upload", 10, "Authorization: Bearer x\r\n"),
                                                                     "HTTP/1.1 401 Unauthorized"},
    {"chunked",      "POST /upload?name=a.pdf HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n",
                                                                     "HTTP/1.1 411 Length Required"},
    {"text route",   "POST /hello HTTP/1.1\r\nHost: t\r\nContent-Length: 2000000\r\n\r\n",
                                                                     "HTTP/1.1 413 Payload Too Large"},
  };

  for (auto const& tc : cases)
  {
    client c;
    BOOST_REQUIRE(c.connect(test_port()));
    c.send(tc.request);
    BOOST_TEST_CONTEXT(tc.name)
    {
      BOOST_CHECK_EQUAL(status_line(c.read_response(0)), tc.status);
    }
  }

  srv.stop();
  set_log_sink({});
}

// with a token, the protected upload works
BOOST_AUTO_TEST_CASE(upload_with_token, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv;

  std::string const pdf = "%PDF-1.4 small";
  client c;
  BOOST_REQUIRE(c.connect(test_port()));
  c.send(upload_request("/secure-upload", pdf.size(),
                        "Authorization: Bearer t0ken\r\nConnection: close\r\n") + pdf);
  BOOST_CHECK_EQUAL(status_line(c.read_all()), "HTTP/1.1 200 OK");

  srv.stop();
  set_log_sink({});
}

// a client that sends the body anyway still receives the 413 (lingering
// close; without it the connection would be reset)
BOOST_AUTO_TEST_CASE(refused_upload_with_body_gets_response, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv;

  client c;
  BOOST_REQUIRE(c.connect(test_port()));
  c.send(upload_request("/upload", 200000) + std::string(200000, 'x'));
  BOOST_CHECK_EQUAL(status_line(c.read_all()), "HTTP/1.1 413 Payload Too Large");

  srv.stop();
  set_log_sink({});
}

// an MCP tool call over a real connection
BOOST_AUTO_TEST_CASE(mcp_tool_call, * boost::unit_test::timeout(20))
{
  set_log_sink([](log_level, std::string_view) {});
  running_server srv;

  std::string const body =
    R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"ping","arguments":{},)"
    R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
    R"("io.modelcontextprotocol/clientCapabilities":{}}}})";

  client c;
  BOOST_REQUIRE(c.connect(test_port()));
  c.send("POST /mcp HTTP/1.1\r\nHost: t\r\nConnection: close\r\n"
         "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
         "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: ping\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  auto const r = c.read_all();
  BOOST_CHECK_EQUAL(status_line(r), "HTTP/1.1 200 OK");
  BOOST_CHECK(r.find(R"("text":"pong")") != std::string::npos);
  BOOST_CHECK(r.find(R"("isError":false)") != std::string::npos);

  srv.stop();
  set_log_sink({});
}

BOOST_AUTO_TEST_SUITE_END()
