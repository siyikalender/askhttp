//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Request handling (server_session.hpp) without a network: handle_request()
// is called directly and the response is captured instead of being written.
//

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <map>
#include <random>
#include <type_traits>

#include "server.hpp"
#include "server_session.hpp"

namespace
{

struct captured
{
  unsigned                            status = 0;
  bool                                has_body = false;
  std::string                         body;
  std::map<std::string, std::string>  headers;   // only the ones below
};

std::string header(const auto& msg, http::field f)
{
  auto const v = msg[f];
  return std::string(v.data(), v.size());
}

struct fixture
{
  net::io_context                         ioc{1};
  std::shared_ptr<response_map>           responses = std::make_shared<response_map>();
  std::shared_ptr<detail::server_state>   state = std::make_shared<detail::server_state>();

  fixture()
  {
    add(*responses, "/hello", text([](const query&) { return std::string("hello world"); }));

    auto* j = json_rpc();
    j->add("ping", [](const boost::json::value&) { return boost::json::value("pong"); });
    add(*responses, "/api", j);
  }

  captured request(http::verb          method,
                   const std::string&  target,
                   const std::string&  body = "",
                   const std::string&  authorization = "")
  {
    http_session session(tcp::socket(ioc), nullptr, responses, state);

    http::request<http::string_body> req{method, target, 11};
    req.body() = body;
    if (!authorization.empty())
      req.set(http::field::authorization, authorization);
    req.prepare_payload();

    captured r;
    session.handle_request(std::move(req), [&](auto&& msg)
    {
      r.status = msg.result_int();
      if constexpr (std::is_same_v<std::decay_t<decltype(msg.body())>, std::string>)
      {
        r.has_body = true;
        r.body     = msg.body();
      }
      for (auto f : {http::field::content_type, http::field::content_length,
                     http::field::allow, http::field::www_authenticate,
                     http::field::server, http::field::content_disposition})
      {
        auto const v = header(msg, f);
        if (!v.empty())
          r.headers[std::string(http::to_string(f))] = v;
      }
      auto const nosniff = msg["X-Content-Type-Options"];
      if (!nosniff.empty())
        r.headers["X-Content-Type-Options"] = std::string(nosniff.data(), nosniff.size());
    });
    return r;
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(request_handling)

// http: GET returns the generated text
BOOST_AUTO_TEST_CASE(get_returns_text)
{
  fixture fx;
  auto r = fx.request(http::verb::get, "/hello");
  BOOST_CHECK_EQUAL(r.status, 200u);
  BOOST_CHECK_EQUAL(r.body, "hello world");
  BOOST_CHECK_EQUAL(r.headers["Content-Length"], "11");
}

// http: HEAD has the headers of GET and no body
BOOST_AUTO_TEST_CASE(head_without_body)
{
  fixture fx;
  auto r = fx.request(http::verb::head, "/hello");
  BOOST_CHECK_EQUAL(r.status, 200u);
  BOOST_CHECK(!r.has_body);
  BOOST_CHECK_EQUAL(r.headers["Content-Length"], "11");
}

// http: POST to a JSON-RPC resource
BOOST_AUTO_TEST_CASE(post_json_resource)
{
  fixture fx;
  auto r = fx.request(http::verb::post, "/api", R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
  BOOST_CHECK_EQUAL(r.status, 200u);
  BOOST_CHECK_EQUAL(r.body, R"({"jsonrpc":"2.0","id":1,"result":"pong"})");
  BOOST_CHECK_EQUAL(r.headers["Content-Type"], "application/json");
}

// http: unsupported methods get 405 with Allow
BOOST_AUTO_TEST_CASE(method_not_allowed)
{
  fixture fx;
  auto a = fx.request(http::verb::post, "/hello");
  auto b = fx.request(http::verb::get,  "/api");
  auto c = fx.request(http::verb::head, "/api");
  BOOST_CHECK_EQUAL(a.status, 405u);
  BOOST_CHECK_EQUAL(a.headers["Allow"], "GET, HEAD");
  BOOST_CHECK_EQUAL(b.status, 405u);
  BOOST_CHECK_EQUAL(b.headers["Allow"], "POST");
  BOOST_CHECK_EQUAL(c.status, 405u);
  BOOST_CHECK(!c.has_body);
}

// http: 404 is plain text and does not echo the target
BOOST_AUTO_TEST_CASE(not_found_without_echo)
{
  fixture fx;
  auto r = fx.request(http::verb::get, "/%3Cscript%3Ealert(1)%3C/script%3E");
  BOOST_CHECK_EQUAL(r.status, 404u);
  BOOST_CHECK_EQUAL(r.headers["Content-Type"], "text/plain");
  BOOST_CHECK(r.body.find("script") == std::string::npos);
}

// http: illegal targets get 400
BOOST_AUTO_TEST_CASE(illegal_target)
{
  fixture fx;
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/../etc/passwd").status, 400u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "relative").status, 400u);
}

// http: no Server header
BOOST_AUTO_TEST_CASE(no_server_header)
{
  fixture fx;
  BOOST_CHECK(fx.request(http::verb::get, "/hello").headers.count("Server") == 0);
  BOOST_CHECK(fx.request(http::verb::get, "/nope").headers.count("Server") == 0);
}

// http: query parameter without '=' does not crash
BOOST_AUTO_TEST_CASE(parameter_without_value)
{
  fixture fx;
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/hello?a=1&b&c=2").status, 200u);
}

// http: authorizer
BOOST_AUTO_TEST_CASE(authorizer)
{
  fixture fx;
  fx.state->authorize = [](const request_info& r)
  {
    if (r.path() == "/public")
      return auth_result::allow;
    if (r.path() == "/boom")
      throw std::runtime_error("bug");
    auto t = r.bearer_token();
    if (!t || !constant_time_equals(*t, "s3cr3t"))
      return auth_result::unauthorized;
    return r.path() == "/admin" ? auth_result::forbidden : auth_result::allow;
  };
  add(*fx.responses, "/admin", text([](const query&) { return std::string("x"); }));

  set_log_sink([](log_level, std::string_view) {});

  auto no_token = fx.request(http::verb::get, "/hello");
  BOOST_CHECK_EQUAL(no_token.status, 401u);
  BOOST_CHECK_EQUAL(no_token.headers["WWW-Authenticate"], "Bearer");

  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/hello", "", "Bearer wrong").status, 401u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/hello", "", "Basic s3cr3t").status, 401u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/hello", "", "Bearer s3cr3t").status, 200u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/hello", "", "bearer s3cr3t").status, 200u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/admin", "", "Bearer s3cr3t").status, 403u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/nope").status, 401u);   // paths not revealed
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/nope", "", "Bearer s3cr3t").status, 404u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/boom").status, 500u);
  BOOST_CHECK(!fx.request(http::verb::head, "/hello").has_body);

  set_log_sink({});
}

// http: constant_time_equals
BOOST_AUTO_TEST_CASE(constant_time_equals_works)
{
  BOOST_CHECK(constant_time_equals("abc", "abc"));
  BOOST_CHECK(!constant_time_equals("abc", "abd"));
  BOOST_CHECK(!constant_time_equals("abc", "ab"));
  BOOST_CHECK(constant_time_equals("", ""));
}

// every response carries X-Content-Type-Options: nosniff
BOOST_AUTO_TEST_CASE(nosniff_on_every_response)
{
  fixture fx;
  BOOST_CHECK_EQUAL(fx.request(http::verb::get,  "/hello").headers["X-Content-Type-Options"], "nosniff");
  BOOST_CHECK_EQUAL(fx.request(http::verb::get,  "/nope").headers["X-Content-Type-Options"], "nosniff");
  BOOST_CHECK_EQUAL(fx.request(http::verb::post, "/hello").headers["X-Content-Type-Options"], "nosniff");
}

// upload and download through handle_request
BOOST_AUTO_TEST_CASE(upload_and_download)
{
  namespace fs = std::filesystem;
  std::random_device rd;
  fs::path const dir = fs::temp_directory_path() / ("askhttp_http_" + std::to_string(rd()));

  fixture fx;
  upload_options opts{.directory = dir.string(), .formats = {file_format::pdf}};
  std::string stored;

  add(*fx.responses, "/upload", upload(opts, [&](const uploaded_file& f)
  {
    stored = f.stored_name;
    return make_result(1, 7);
  }));
  add(*fx.responses, "/file", download(opts, [&](const query&)
  {
    return std::optional<download_info>({stored, "belge.pdf"});
  }));

  std::string const pdf = "%PDF-1.4 test";

  auto up = fx.request(http::verb::post, "/upload?name=belge.pdf", pdf);
  BOOST_CHECK_EQUAL(up.status, 200u);
  BOOST_CHECK_EQUAL(up.body, R"({"status":1,"what":7})");
  BOOST_CHECK_EQUAL(up.headers["Content-Type"], "application/json");

  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/upload?name=belge.pdf").status, 405u);
  BOOST_CHECK_EQUAL(fx.request(http::verb::get, "/upload").headers["Allow"], "POST");

  auto down = fx.request(http::verb::get, "/file?id=7");
  BOOST_CHECK_EQUAL(down.status, 200u);
  BOOST_CHECK_EQUAL(down.body, pdf);
  BOOST_CHECK_EQUAL(down.headers["Content-Type"], "application/pdf");
  BOOST_CHECK_EQUAL(down.headers["Content-Disposition"],
                    "inline; filename=\"belge.pdf\"; filename*=UTF-8''belge.pdf");

  std::error_code ec;
  fs::remove_all(dir, ec);
}

BOOST_AUTO_TEST_SUITE_END()
