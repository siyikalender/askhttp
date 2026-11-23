//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// MCP endpoint (protocol revision 2026-07-28).
//

#include <boost/test/unit_test.hpp>

#include <map>
#include <type_traits>

#include "server.hpp"
#include "server_session.hpp"
#include "mcp.hpp"

namespace
{

const std::string version = "2026-07-28";

using header_map = std::map<std::string, std::string>;

// Valid request body; extra_params is inserted before _meta.
std::string request(const std::string& method, const std::string& extra_params = "",
                    const std::string& id = "1", const std::string& body_version = version)
{
  return R"({"jsonrpc":"2.0","id":)" + id + R"(,"method":")" + method + R"(","params":{)" +
         extra_params + (extra_params.empty() ? "" : ",") +
         R"("_meta":{"io.modelcontextprotocol/protocolVersion":")" + body_version +
         R"(","io.modelcontextprotocol/clientCapabilities":{}}}})";
}

header_map headers(const std::string& method, const std::string& name = "")
{
  header_map h{{"MCP-Protocol-Version", version}, {"Mcp-Method", method}};
  if (!name.empty())
    h["Mcp-Name"] = name;
  return h;
}

response_data send(const mcp_provider& m, const std::string& body, const header_map& h)
{
  std::string const method = "POST";
  query const q;
  request_context const ctx
  {
    method, body, q,
    [&h](std::string_view name)
    {
      for (auto const& [k, v] : h)    // header names are case-insensitive
        if (boost::beast::iequals(k, boost::beast::string_view(name.data(), name.size())))
          return v;
      return std::string();
    }
  };
  return *m.respond(ctx);
}

boost::json::value parse(const std::string& text)
{
  boost::json::error_code ec;
  auto v = boost::json::parse(text, ec);
  BOOST_REQUIRE(!ec);
  return v;
}

const boost::json::value& at(const boost::json::value& v, std::string_view key)
{
  auto const* p = v.as_object().if_contains(key);
  BOOST_REQUIRE(p);
  return *p;
}

std::string str(const boost::json::value& v)
{
  return std::string(v.as_string().data(), v.as_string().size());
}

long long error_code(const response_data& r)
{
  return at(at(parse(r.body), "error"), "code").as_int64();
}

struct quiet_log
{
  quiet_log()  { set_log_sink([](log_level, std::string_view) {}); }
  ~quiet_log() { set_log_sink({}); }
};

// A server with three tools.
struct fixture
{
  mcp_provider m{{.name = "test-server", .version = "1.2", .instructions = "Use the tools.",
                  .allowed_origins = {"http://localhost:3000"}}};

  fixture()
  {
    m.add_tool({.name = "echo", .title = "Echo", .description = "Returns its arguments.",
                .input_schema = parse(R"({"type":"object","properties":{"x":{"type":"integer"}}})"),
                .read_only = true},
               [](const boost::json::value& args) { return args; });

    m.add_tool({.name = "hello", .description = "Says hello."},
               [](const boost::json::value&) { return boost::json::value("merhaba"); });

    m.add_tool({.name = "fail", .description = "Fails.", .destructive = true},
               [](const boost::json::value& args) -> boost::json::value
               {
                 if (args.as_object().if_contains("internal"))
                   throw std::runtime_error("SQL: select secret");
                 throw rpc_error("date must be in the future");
               });
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(mcp_endpoint)

// server/discover: versions, capabilities, identity
BOOST_AUTO_TEST_CASE(discover)
{
  fixture fx;
  auto const r = send(fx.m, request("server/discover"), headers("server/discover"));
  BOOST_CHECK_EQUAL(r.status, 200u);
  BOOST_CHECK_EQUAL(r.content_type, "application/json");

  auto const v = parse(r.body);
  BOOST_CHECK_EQUAL(str(at(v, "jsonrpc")), "2.0");
  BOOST_CHECK_EQUAL(at(v, "id").as_int64(), 1);
  auto const& res = at(v, "result");
  BOOST_CHECK_EQUAL(str(at(res, "resultType")), "complete");
  BOOST_CHECK_EQUAL(str(at(res, "supportedVersions").as_array().at(0)), version);
  BOOST_CHECK(at(at(res, "capabilities"), "tools").is_object());
  BOOST_CHECK_EQUAL(str(at(res, "instructions")), "Use the tools.");
  BOOST_CHECK_EQUAL(str(at(at(at(res, "_meta"), "io.modelcontextprotocol/serverInfo"), "name")), "test-server");
  BOOST_CHECK(at(res, "ttlMs").is_int64() || at(res, "ttlMs").is_uint64());
}

// tools/list: registration order, schemas, annotations
BOOST_AUTO_TEST_CASE(tools_list)
{
  fixture fx;
  auto const v = parse(send(fx.m, request("tools/list"), headers("tools/list")).body);
  auto const& res   = at(v, "result");
  auto const& tools = at(res, "tools").as_array();

  BOOST_REQUIRE_EQUAL(tools.size(), 3u);
  BOOST_CHECK_EQUAL(str(at(tools[0], "name")), "echo");
  BOOST_CHECK_EQUAL(str(at(tools[1], "name")), "hello");
  BOOST_CHECK_EQUAL(str(at(tools[2], "name")), "fail");

  BOOST_CHECK_EQUAL(str(at(tools[0], "title")), "Echo");
  BOOST_CHECK(at(at(tools[0], "annotations"), "readOnlyHint").as_bool());
  BOOST_CHECK(at(at(tools[2], "annotations"), "destructiveHint").as_bool());
  BOOST_CHECK(!tools[1].as_object().contains("annotations"));
  BOOST_CHECK_EQUAL(str(at(at(tools[1], "inputSchema"), "type")), "object");   // default schema

  BOOST_CHECK_EQUAL(str(at(res, "resultType")), "complete");
  BOOST_CHECK_EQUAL(str(at(res, "cacheScope")), "private");
}

// tools/call: structured result, text result
BOOST_AUTO_TEST_CASE(tools_call_success)
{
  fixture fx;

  auto const a = parse(send(fx.m, request("tools/call", R"("name":"echo","arguments":{"x":5})"),
                            headers("tools/call", "echo")).body);
  auto const& ra = at(a, "result");
  BOOST_CHECK(!at(ra, "isError").as_bool());
  BOOST_CHECK_EQUAL(at(at(ra, "structuredContent"), "x").as_int64(), 5);
  auto const& text = at(ra, "content").as_array().at(0);
  BOOST_CHECK_EQUAL(str(at(text, "type")), "text");
  BOOST_CHECK_EQUAL(str(at(text, "text")), R"({"x":5})");

  auto const b = parse(send(fx.m, request("tools/call", R"("name":"hello")"),
                            headers("tools/call", "hello")).body);
  auto const& rb = at(b, "result");
  BOOST_CHECK_EQUAL(str(at(at(rb, "content").as_array().at(0), "text")), "merhaba");
  BOOST_CHECK(!rb.as_object().contains("structuredContent"));
}

// tools/call: tool errors are results with isError, protocol errors are errors
BOOST_AUTO_TEST_CASE(tools_call_errors)
{
  quiet_log quiet;
  fixture fx;

  auto const e1 = parse(send(fx.m, request("tools/call", R"("name":"fail","arguments":{})"),
                             headers("tools/call", "fail")).body);
  BOOST_CHECK(at(at(e1, "result"), "isError").as_bool());
  BOOST_CHECK_EQUAL(str(at(at(at(e1, "result"), "content").as_array().at(0), "text")),
                    "date must be in the future");

  auto const e2 = parse(send(fx.m, request("tools/call", R"("name":"fail","arguments":{"internal":1})"),
                             headers("tools/call", "fail")).body);
  BOOST_CHECK_EQUAL(str(at(at(at(e2, "result"), "content").as_array().at(0), "text")),
                    "Internal error");   // exception text is not sent

  auto const e3 = send(fx.m, request("tools/call", R"("name":"nope")"), headers("tools/call", "nope"));
  BOOST_CHECK_EQUAL(error_code(e3), -32602);
  BOOST_CHECK_EQUAL(str(at(at(parse(e3.body), "error"), "message")), "Unknown tool: nope");

  auto const e4 = send(fx.m, request("tools/call", R"("name":"echo","arguments":[1])"), headers("tools/call", "echo"));
  BOOST_CHECK_EQUAL(error_code(e4), -32602);
}

// request metadata headers must be present and match the body
BOOST_AUTO_TEST_CASE(header_validation)
{
  fixture fx;
  auto const list = request("tools/list");

  auto h = headers("tools/list");
  h.erase("MCP-Protocol-Version");
  auto r = send(fx.m, list, h);
  BOOST_CHECK_EQUAL(r.status, 400u);
  BOOST_CHECK_EQUAL(error_code(r), -32020);

  h = headers("tools/list");
  h.erase("Mcp-Method");
  BOOST_CHECK_EQUAL(error_code(send(fx.m, list, h)), -32020);

  BOOST_CHECK_EQUAL(error_code(send(fx.m, list, headers("tools/call"))), -32020);

  auto const call = request("tools/call", R"("name":"echo")");
  r = send(fx.m, call, headers("tools/call"));                       // Mcp-Name missing
  BOOST_CHECK_EQUAL(r.status, 400u);
  BOOST_CHECK_EQUAL(error_code(r), -32020);
  BOOST_CHECK_EQUAL(error_code(send(fx.m, call, headers("tools/call", "other"))), -32020);
  BOOST_CHECK_EQUAL(error_code(send(fx.m, call, headers("tools/call", "=?base64?b3RoZXI=?="))), -32020);
  BOOST_CHECK_EQUAL(error_code(send(fx.m, call, headers("tools/call", "=?base64?!!!!?="))), -32020);

  // Base64-encoded Mcp-Name that matches
  BOOST_CHECK_EQUAL(send(fx.m, call, headers("tools/call", "=?base64?ZWNobw==?=")).status, 200u);

  // Header names are case-insensitive
  header_map lower{{"mcp-protocol-version", version}, {"mcp-method", "tools/list"}};
  BOOST_CHECK_EQUAL(send(fx.m, list, lower).status, 200u);
}

// _meta and protocol version
BOOST_AUTO_TEST_CASE(meta_and_version)
{
  fixture fx;

  auto r = send(fx.m, R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})", headers("tools/list"));
  BOOST_CHECK_EQUAL(r.status, 400u);
  BOOST_CHECK_EQUAL(error_code(r), -32602);

  r = send(fx.m, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", headers("tools/list"));
  BOOST_CHECK_EQUAL(error_code(r), -32602);

  // header and body disagree
  r = send(fx.m, request("tools/list", "", "1", "2025-11-25"), headers("tools/list"));
  BOOST_CHECK_EQUAL(error_code(r), -32020);

  // both name a version this server does not support
  auto h = headers("tools/list");
  h["MCP-Protocol-Version"] = "2024-11-05";
  r = send(fx.m, request("tools/list", "", "1", "2024-11-05"), h);
  BOOST_CHECK_EQUAL(r.status, 400u);
  BOOST_CHECK_EQUAL(error_code(r), -32022);
  auto const data = at(at(parse(r.body), "error"), "data");
  BOOST_CHECK_EQUAL(str(at(data, "supported").as_array().at(0)), version);
  BOOST_CHECK_EQUAL(str(at(data, "requested")), "2024-11-05");
}

// revisions before 2026-07-28: initialize handshake, no metadata headers
BOOST_AUTO_TEST_CASE(legacy_versions)
{
  quiet_log quiet;
  fixture fx;
  header_map const none;

  // initialize: the requested version if supported, otherwise the latest
  auto r = send(fx.m, R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"c","version":"1"}}})", none);
  BOOST_CHECK_EQUAL(r.status, 200u);
  auto res = at(parse(r.body), "result");
  BOOST_CHECK_EQUAL(str(at(res, "protocolVersion")), "2025-06-18");
  BOOST_CHECK_EQUAL(str(at(at(res, "serverInfo"), "name")), "test-server");
  BOOST_CHECK(at(res, "capabilities").as_object().contains("tools"));
  BOOST_CHECK_EQUAL(str(at(res, "instructions")), "Use the tools.");

  r = send(fx.m, R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})", none);
  BOOST_CHECK_EQUAL(str(at(at(parse(r.body), "result"), "protocolVersion")), "2025-11-25");

  // notifications/initialized
  r = send(fx.m, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", none);
  BOOST_CHECK_EQUAL(r.status, 202u);

  header_map const v{{"MCP-Protocol-Version", "2025-11-25"}};

  r = send(fx.m, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", v);
  BOOST_CHECK_EQUAL(r.status, 200u);
  res = at(parse(r.body), "result");
  BOOST_CHECK_EQUAL(at(res, "tools").as_array().size(), 3u);
  BOOST_CHECK(!res.as_object().contains("resultType"));

  // tools/call without Mcp-Name; no header at all means 2025-03-26
  r = send(fx.m, R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"x":5}}})", none);
  BOOST_CHECK_EQUAL(r.status, 200u);
  res = at(parse(r.body), "result");
  BOOST_CHECK_EQUAL(at(at(res, "structuredContent"), "x").as_int64(), 5);
  BOOST_CHECK(!at(res, "isError").as_bool());
  BOOST_CHECK(!res.as_object().contains("_meta"));

  r = send(fx.m, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"fail","arguments":{}}})", v);
  BOOST_CHECK(at(at(parse(r.body), "result"), "isError").as_bool());

  r = send(fx.m, R"({"jsonrpc":"2.0","id":4,"method":"ping"})", v);
  BOOST_CHECK_EQUAL(r.status, 200u);
  BOOST_CHECK(at(parse(r.body), "result").as_object().empty());

  // errors are JSON-RPC errors with HTTP 200
  r = send(fx.m, R"({"jsonrpc":"2.0","id":5,"method":"resources/list"})", v);
  BOOST_CHECK_EQUAL(r.status, 200u);
  BOOST_CHECK_EQUAL(error_code(r), -32601);

  r = send(fx.m, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"nope"}})", v);
  BOOST_CHECK_EQUAL(error_code(r), -32602);

  // disabled: initialize is treated as a 2026-07-28 request and refused
  mcp_provider strict{{.name = "s", .version = "1", .legacy_versions = false}};
  r = send(strict, R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})", none);
  BOOST_CHECK_EQUAL(r.status, 400u);
  BOOST_CHECK_EQUAL(error_code(r), -32020);
}

// Origin: absent or allowed passes, anything else is 403
BOOST_AUTO_TEST_CASE(origin_validation)
{
  quiet_log quiet;
  fixture fx;
  auto const body = request("tools/list");

  auto h = headers("tools/list");
  BOOST_CHECK_EQUAL(send(fx.m, body, h).status, 200u);

  h["Origin"] = "http://localhost:3000";
  BOOST_CHECK_EQUAL(send(fx.m, body, h).status, 200u);

  h["Origin"] = "http://evil.example";
  auto const r = send(fx.m, body, h);
  BOOST_CHECK_EQUAL(r.status, 403u);
  BOOST_CHECK(!parse(r.body).as_object().contains("id"));
}

// message-level rules
BOOST_AUTO_TEST_CASE(message_rules)
{
  fixture fx;
  auto const h = headers("tools/list");

  auto r = send(fx.m, R"({"jsonrpc":"2.0","method":"notifications/x"})", h);
  BOOST_CHECK_EQUAL(r.status, 202u);
  BOOST_CHECK(r.body.empty());

  r = send(fx.m, "[" + request("tools/list") + "]", h);           // no batches
  BOOST_CHECK_EQUAL(r.status, 400u);
  BOOST_CHECK_EQUAL(error_code(r), -32600);

  r = send(fx.m, "{oops", h);
  BOOST_CHECK_EQUAL(r.status, 400u);
  BOOST_CHECK_EQUAL(error_code(r), -32700);

  r = send(fx.m, request("tools/list", "", "null"), h);           // id must not be null
  BOOST_CHECK_EQUAL(error_code(r), -32600);

  r = send(fx.m, request("resources/list"), headers("resources/list"));
  BOOST_CHECK_EQUAL(r.status, 404u);
  BOOST_CHECK_EQUAL(error_code(r), -32601);

  r = send(fx.m, request("tools/list", "", R"("abc")"), h);       // string id echoed
  BOOST_CHECK_EQUAL(str(at(parse(r.body), "id")), "abc");
}

// tool registration is checked
BOOST_AUTO_TEST_CASE(registration)
{
  auto ok = [](const boost::json::value& v) { return v; };
  BOOST_CHECK_THROW(mcp_provider({.name = "", .version = "1"}), std::invalid_argument);

  mcp_provider m{{.name = "s", .version = "1"}};
  BOOST_CHECK_THROW(m.add_tool({.name = "has space"}, ok), std::invalid_argument);
  BOOST_CHECK_THROW(m.add_tool({.name = ""}, ok), std::invalid_argument);
  BOOST_CHECK_THROW(m.add_tool({.name = "x", .input_schema = boost::json::value(5)}, ok), std::invalid_argument);
  BOOST_CHECK_THROW(m.add_tool({.name = "x"}, {}), std::invalid_argument);
  m.add_tool({.name = "x"}, ok);
  BOOST_CHECK_THROW(m.add_tool({.name = "x"}, ok), std::invalid_argument);
}

// one handler serves the frontend (JSON-RPC) and MCP
BOOST_AUTO_TEST_CASE(shared_handler)
{
  json_rpc_provider rpc;
  rpc.add("double", [](const boost::json::value& p) -> boost::json::value
  {
    return 2 * p.as_object().if_contains("n")->as_int64();
  });

  mcp_provider m{{.name = "s", .version = "1"}};
  m.add_tool({.name = "double", .description = "Doubles n."}, rpc.handler("double"));

  BOOST_CHECK_EQUAL(rpc.produce(R"({"jsonrpc":"2.0","id":1,"method":"double","params":{"n":21}})", query{})->body,
                    R"({"jsonrpc":"2.0","id":1,"result":42})");
  auto const v = parse(send(m, request("tools/call", R"("name":"double","arguments":{"n":21})"),
                            headers("tools/call", "double")).body);
  // 42 is not an object: text only, no structuredContent
  auto const& res = at(v, "result");
  BOOST_CHECK(!res.as_object().contains("structuredContent"));
  BOOST_CHECK_EQUAL(str(at(at(res, "content").as_array().at(0), "text")), "42");

  // an array result (e.g. a list of records) likewise
  m.add_tool({.name = "list", .description = "Lists."},
             [](const boost::json::value&) -> boost::json::value
             {
               boost::json::array a;
               a.push_back(1);
               a.push_back(2);
               return a;
             });
  auto const l = parse(send(m, request("tools/call", R"("name":"list","arguments":{})"),
                            headers("tools/call", "list")).body);
  BOOST_CHECK(!at(l, "result").as_object().contains("structuredContent"));
  BOOST_CHECK_EQUAL(str(at(at(at(l, "result"), "content").as_array().at(0), "text")), "[1,2]");
}

// over the server: only POST is accepted on the MCP endpoint
BOOST_AUTO_TEST_CASE(http_methods)
{
  net::io_context ioc{1};
  auto responses = std::make_shared<response_map>();
  add(*responses, "/mcp", mcp({.name = "s", .version = "1"}));
  auto state = std::make_shared<detail::server_state>();
  http_session session(tcp::socket(ioc), nullptr, responses, state);

  for (auto verb : {http::verb::get, http::verb::delete_})
  {
    http::request<http::string_body> req{verb, "/mcp", 11};
    unsigned status = 0;
    std::string allow;
    session.handle_request(std::move(req), [&](auto&& msg)
    {
      status = msg.result_int();
      auto const a = msg[http::field::allow];
      allow.assign(a.data(), a.size());
    });
    BOOST_CHECK_EQUAL(status, 405u);
    BOOST_CHECK_EQUAL(allow, "POST");
  }
}

// CORS: preflight for allowed origins, Access-Control-Allow-Origin on the
// responses, nothing for other origins
BOOST_AUTO_TEST_CASE(cors)
{
  quiet_log quiet;
  net::io_context ioc{1};
  auto responses = std::make_shared<response_map>();
  add(*responses, "/mcp", mcp({.name = "s", .version = "1",
                               .allowed_origins = {"http://127.0.0.1:8080"}}));
  auto state = std::make_shared<detail::server_state>();
  // Every request without credentials is refused: the preflight must still
  // succeed, since browsers send it without the Authorization header.
  state->authorize = [](const request_info&) { return auth_result::unauthorized; };
  http_session session(tcp::socket(ioc), nullptr, responses, state);

  struct result { unsigned status = 0; std::string acao, methods, headers; };
  auto run = [&](http::verb verb, std::string origin, bool preflight)
  {
    http::request<http::string_body> req{verb, "/mcp", 11};
    if (!origin.empty())
      req.set("Origin", origin);
    if (preflight)
    {
      req.set("Access-Control-Request-Method", "POST");
      req.set("Access-Control-Request-Headers", "content-type");
    }
    result r;
    session.handle_request(std::move(req), [&](auto&& msg)
    {
      auto get = [&](const char* name)
      {
        auto const v = msg[name];
        return std::string(v.data(), v.size());
      };
      r.status  = msg.result_int();
      r.acao    = get("Access-Control-Allow-Origin");
      r.methods = get("Access-Control-Allow-Methods");
      r.headers = get("Access-Control-Allow-Headers");
    });
    return r;
  };

  auto const ok = run(http::verb::options, "http://127.0.0.1:8080", true);
  BOOST_CHECK_EQUAL(ok.status, 204u);
  BOOST_CHECK_EQUAL(ok.acao, "http://127.0.0.1:8080");
  BOOST_CHECK_EQUAL(ok.methods, "POST");
  BOOST_CHECK_EQUAL(ok.headers, "content-type");

  auto const evil = run(http::verb::options, "http://evil.example", true);
  BOOST_CHECK_EQUAL(evil.status, 403u);
  BOOST_CHECK(evil.acao.empty());

  // The real request: 401 here, readable by the allowed page
  auto const post = run(http::verb::post, "http://127.0.0.1:8080", false);
  BOOST_CHECK_EQUAL(post.status, 401u);
  BOOST_CHECK_EQUAL(post.acao, "http://127.0.0.1:8080");

  auto const plain = run(http::verb::post, "", false);
  BOOST_CHECK_EQUAL(plain.status, 401u);
  BOOST_CHECK(plain.acao.empty());
}

BOOST_AUTO_TEST_SUITE_END()
