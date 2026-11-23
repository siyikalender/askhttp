//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// JSON output (json_writer.hpp), field access (json_fields.hpp) and the JSON request
// provider. Generated JSON is checked by parsing it back with Boost.JSON.
//

#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>

#include <boost/json.hpp>

#include "json_fields.hpp"
#include "json_writer.hpp"
#include "provider.hpp"

namespace
{

boost::json::value parse(const std::string& text)
{
  boost::json::error_code ec;
  auto v = boost::json::parse(text, ec);
  if (ec)
    throw std::runtime_error("invalid JSON: " + text);
  return v;
}

std::string str(const boost::json::value& v)
{
  return std::string(v.as_string().data(), v.as_string().size());
}

double number(const boost::json::value& v)
{
  if (v.is_double()) return v.as_double();
  if (v.is_int64())  return static_cast<double>(v.as_int64());
  return static_cast<double>(v.as_uint64());
}

} // namespace

// ---- output -----------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(json)

// json output: every control character gives valid JSON
BOOST_AUTO_TEST_CASE(output_control_characters)
{
  std::string input;
  for (int c = 0; c < 0x20; ++c)
    input += static_cast<char>(c);
  input += "\"\\/ \xC3\xA7" "ay";   // quotes, backslash, slash, UTF-8

  std::string out;
  out << input;

  BOOST_CHECK_EQUAL(str(parse(out)), input);
}

// json output: empty string is "", empty optional is null
BOOST_AUTO_TEST_CASE(output_empty_string_and_null)
{
  std::string a, b;
  a << std::string();
  b << boost::optional<std::string>();
  BOOST_CHECK_EQUAL(a, "\"\"");
  BOOST_CHECK_EQUAL(b, "null");
}

// json output: doubles round-trip exactly
BOOST_AUTO_TEST_CASE(output_doubles_round_trip)
{
  for (double d : {0.1, 1e-7, 1.23456789, 1e300, -2.5, 2.0, 123456.789})
  {
    std::string out;
    out << d;
    BOOST_CHECK_EQUAL(number(parse(out)), d);
  }
}

// json output: NaN and infinity are null
BOOST_AUTO_TEST_CASE(output_nan_and_infinity)
{
  std::string a, b;
  a << std::numeric_limits<double>::quiet_NaN();
  b << std::numeric_limits<double>::infinity();
  BOOST_CHECK_EQUAL(a, "null");
  BOOST_CHECK_EQUAL(b, "null");
}

// json output: dates are ISO 8601
BOOST_AUTO_TEST_CASE(output_dates_iso_8601)
{
  std::tm d{};
  d.tm_year = 2026 - 1900;
  d.tm_mon  = 0;
  d.tm_mday = 5;

  std::string out;
  out << d;
  BOOST_CHECK_EQUAL(out, "\"2026-01-05\"");
}

// json output: make_result is an object, not a string
BOOST_AUTO_TEST_CASE(output_make_result_is_object)
{
  auto ok  = parse(make_result(1, 5));
  auto err = parse(make_result(0, "remove failed"));

  BOOST_CHECK(ok.is_object());
  BOOST_CHECK_EQUAL(number(*ok.as_object().if_contains("status")), 1.0);
  BOOST_CHECK_EQUAL(number(*ok.as_object().if_contains("what")), 5.0);
  BOOST_CHECK_EQUAL(str(*err.as_object().if_contains("what")), "remove failed");
}

// ---- field access -------------------------------------------------------------

// get_field: scalar types
BOOST_AUTO_TEST_CASE(get_field_scalar_types)
{
  auto d = parse(R"({"s":"Ali \"Veli\"","i":42,"n":-7,"f":0.1,"t":true,"b":false,"e":""})");
  BOOST_CHECK(*get_field(d, "s") == "Ali \"Veli\"");
  BOOST_CHECK(*get_field(d, "i") == "42");
  BOOST_CHECK(*get_field(d, "n") == "-7");
  BOOST_CHECK(*get_field(d, "f") == "0.1");
  BOOST_CHECK(*get_field(d, "t") == "true");
  BOOST_CHECK(*get_field(d, "b") == "false");
  BOOST_CHECK(*get_field(d, "e") == "");
}

// get_field: null and missing fields give an empty optional
BOOST_AUTO_TEST_CASE(get_field_null_and_missing)
{
  auto d = parse(R"({"x":null})");
  BOOST_CHECK(!get_field(d, "x"));          // property_tree returned the text "null"
  BOOST_CHECK(!get_field(d, "missing"));
  BOOST_CHECK(!get_field(parse("null"), "x"));
  BOOST_CHECK(!get_field(parse("[1,2]"), "x"));
}

// get_field: nested paths, arrays and objects
BOOST_AUTO_TEST_CASE(get_field_nested_paths_arrays_objects)
{
  auto d = parse(R"({"adres":{"sehir":"Izmir","no":5},"liste":[1,2]})");
  BOOST_CHECK(*get_field(d, "adres.sehir") == "Izmir");
  BOOST_CHECK(*get_field(d, "adres.no") == "5");
  BOOST_CHECK(!get_field(d, "adres.posta"));
  BOOST_CHECK(!get_field(d, "liste.x"));
  BOOST_CHECK(*get_field(d, "liste") == "[1,2]");
  BOOST_CHECK(find_field(d, "adres") != nullptr);
  BOOST_CHECK(find_field(d, "adres")->is_object());
}

// get_required_field: present, missing and null
BOOST_AUTO_TEST_CASE(get_required_field_present_missing_null)
{
  auto d = parse(R"({"id":7,"n":null})");
  BOOST_CHECK_EQUAL(get_required_field(d, "id"), "7");
  BOOST_CHECK_THROW(get_required_field(d, "missing"), std::exception);
  BOOST_CHECK_THROW(get_required_field(d, "n"), std::exception);
}

// json_escape: escapes like JSON, keeps empty optionals
BOOST_AUTO_TEST_CASE(json_escape_escapes)
{
  BOOST_CHECK_EQUAL(json_escape(std::string("a\"b\\c\n")), "a\\\"b\\\\c\\n");
  BOOST_CHECK(*json_escape(boost::optional<std::string>(std::string("x\ty"))) == "x\\ty");
  BOOST_CHECK(!json_escape(boost::optional<std::string>()));
}

// get_param: present, without value, missing
BOOST_AUTO_TEST_CASE(get_param_query)
{
  query q;
  q["a"] = std::string("1");
  q["flag"] = boost::none;
  BOOST_CHECK(*get_param(q, "a") == "1");
  BOOST_CHECK(!get_param(q, "flag"));
  BOOST_CHECK(!get_param(q, "missing"));
  BOOST_CHECK(q.at("a"));   // the query is not modified
}

// ---- JSON-RPC ---------------------------------------------------------------------

namespace
{

response_data rpc(const json_rpc_provider& p, const std::string& body)
{
  return *p.produce(body, query{});
}

std::string rpc_body(const json_rpc_provider& p, const std::string& body)
{
  return rpc(p, body).body;
}

// Silences the log for the lifetime of the object.
struct quiet_log
{
  quiet_log()  { set_log_sink([](log_level, std::string_view) {}); }
  ~quiet_log() { set_log_sink({}); }
};

} // namespace

// json-rpc: result with the request id, params passed through
BOOST_AUTO_TEST_CASE(rpc_result)
{
  json_rpc_provider p;
  p.add("echo", [](const boost::json::value& params) { return params; });

  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":"echo","params":{"a":1}})"),
                    R"({"jsonrpc":"2.0","id":1,"result":{"a":1}})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":"x7","method":"echo","params":[1,2]})"),
                    R"({"jsonrpc":"2.0","id":"x7","result":[1,2]})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":2,"method":"echo"})"),
                    R"({"jsonrpc":"2.0","id":2,"result":null})");
  BOOST_CHECK_EQUAL(rpc(p, R"({"jsonrpc":"2.0","id":1,"method":"echo"})").content_type, "application/json");
}

// json-rpc: errors from handlers
BOOST_AUTO_TEST_CASE(rpc_handler_errors)
{
  quiet_log quiet;
  json_rpc_provider p;
  p.add("app",  [](const boost::json::value&) -> boost::json::value { throw rpc_error(1001, "record not found"); });
  p.add("data", [](const boost::json::value&) -> boost::json::value
  {
    boost::json::object d;
    d["field"] = "id";
    throw rpc_error(rpc_code::invalid_params, "bad id", d);
  });
  p.add("boom", [](const boost::json::value&) -> boost::json::value { throw std::runtime_error("SQL: select secret"); });
  p.add("need", [](const boost::json::value& params) -> boost::json::value
  {
    // get_required_field() returns std::string; boost::json::value is built from a
    // string_view, so the conversion must be spelled out.
    std::string const id = get_required_field(params, "id");
    return boost::json::value(boost::json::string_view(id.data(), id.size()));
  });

  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":"app"})"),
                    R"({"jsonrpc":"2.0","id":1,"error":{"code":1001,"message":"record not found"}})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":"data"})"),
                    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"bad id","data":{"field":"id"}}})");
  // The exception text is not sent to the client.
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":"boom"})"),
                    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32603,"message":"Internal error"}})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":"need","params":{}})"),
                    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"missing required field: id"}})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":"nope"})"),
                    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}})");
}

// json-rpc: malformed requests
BOOST_AUTO_TEST_CASE(rpc_malformed_requests)
{
  quiet_log quiet;
  json_rpc_provider p;
  p.add("echo", [](const boost::json::value& v) { return v; });

  std::string const parse_error = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})";
  std::string const invalid     = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Request"}})";

  BOOST_CHECK_EQUAL(rpc_body(p, ""), parse_error);
  BOOST_CHECK_EQUAL(rpc_body(p, "garbage"), parse_error);
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","method":"echo",})"), parse_error);
  std::string deep(100, '[');
  deep += std::string(100, ']');
  BOOST_CHECK_EQUAL(rpc_body(p, deep), parse_error);      // nesting depth limit

  BOOST_CHECK_EQUAL(rpc_body(p, "5"), invalid);
  BOOST_CHECK_EQUAL(rpc_body(p, "[]"), invalid);           // empty batch
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"id":1,"method":"echo"})"), R"({"jsonrpc":"2.0","id":1,"error":{"code":-32600,"message":"Invalid Request"}})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"1.0","id":1,"method":"echo"})"), R"({"jsonrpc":"2.0","id":1,"error":{"code":-32600,"message":"Invalid Request"}})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":5})"), R"({"jsonrpc":"2.0","id":1,"error":{"code":-32600,"message":"Invalid Request"}})");
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":{},"method":"echo"})"), invalid);
  BOOST_CHECK_EQUAL(rpc_body(p, R"({"jsonrpc":"2.0","id":1,"method":"echo","params":3})"),
                    R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"Invalid params"}})");
}

// json-rpc: notifications run but get no response
BOOST_AUTO_TEST_CASE(rpc_notifications)
{
  json_rpc_provider p;
  int calls = 0;
  p.add("tick", [&](const boost::json::value&) { ++calls; return boost::json::value(); });

  auto const r = rpc(p, R"({"jsonrpc":"2.0","method":"tick"})");
  BOOST_CHECK_EQUAL(r.status, 204u);
  BOOST_CHECK(r.body.empty());
  BOOST_CHECK_EQUAL(calls, 1);
}

// json-rpc: batches
BOOST_AUTO_TEST_CASE(rpc_batch)
{
  quiet_log quiet;
  json_rpc_provider p;
  p.add("echo", [](const boost::json::value& v) { return v; });

  BOOST_CHECK_EQUAL(
    rpc_body(p, R"([{"jsonrpc":"2.0","id":1,"method":"echo","params":[1]},
                    {"jsonrpc":"2.0","method":"echo"},
                    {"jsonrpc":"2.0","id":2,"method":"nope"},
                    7])"),
    R"([{"jsonrpc":"2.0","id":1,"result":[1]},)"
    R"({"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"Method not found"}},)"
    R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Request"}}])");

  BOOST_CHECK_EQUAL(rpc(p, R"([{"jsonrpc":"2.0","method":"echo"}])").status, 204u);
}

// json-rpc: a registered handler can be looked up (to share it with MCP)
BOOST_AUTO_TEST_CASE(rpc_handler_lookup)
{
  json_rpc_provider p;
  p.add("echo", [](const boost::json::value& v) { return v; });
  BOOST_CHECK(p.handler("echo"));
  BOOST_CHECK_THROW(p.handler("nope"), std::out_of_range);
}

// text provider: generator receives the query
BOOST_AUTO_TEST_CASE(text_provider_receives_query)
{
  auto p = std::unique_ptr<response_provider>(text([](const query& q)
  {
    auto it = q.find("n");
    return std::string("hi ") + (it != q.end() && it->second ? *it->second : "");
  }));

  query q;
  q["n"] = std::string("Ali");
  BOOST_CHECK(*p->content("", q) == "hi Ali");
}

// file provider: content, mime type, missing file
BOOST_AUTO_TEST_CASE(file_provider_content)
{
  std::string const path = "askhttp_test_file.html";
  {
    std::ofstream f(path, std::ios::binary);
    f << "<p>x</p>";
  }

  auto p = std::unique_ptr<response_provider>(file(path));
  auto missing = std::unique_ptr<response_provider>(file("does/not/exist.css"));

  BOOST_CHECK(*p->content("", query{}) == "<p>x</p>");
  BOOST_CHECK_EQUAL(to_std_string(p->mime_type()), "text/html");
  BOOST_CHECK_EQUAL(to_std_string(missing->mime_type()), "text/css");

  set_log_sink([](log_level, std::string_view) {});
  BOOST_CHECK(!missing->content("", query{}));
  set_log_sink({});

  std::remove(path.c_str());
  }

  // mime_type(): case-insensitive, first exact extension match, fallback
  BOOST_AUTO_TEST_CASE(mime_type_lookup)
  {
  BOOST_CHECK_EQUAL(to_std_string(::mime_type("index.html")), "text/html");
  BOOST_CHECK_EQUAL(to_std_string(::mime_type("A.PNG")), "image/png");
  BOOST_CHECK_EQUAL(to_std_string(::mime_type("x.woff2")), "application/font-woff2");
  BOOST_CHECK_EQUAL(to_std_string(::mime_type("archive.tar.gz")), "application/text");
  BOOST_CHECK_EQUAL(to_std_string(::mime_type("noext")), "application/text");
  }

  BOOST_AUTO_TEST_SUITE_END()
