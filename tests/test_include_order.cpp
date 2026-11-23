//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Include order of the public headers: server.hpp after mcp.hpp,
// json_fields.hpp and provider.hpp. With Boost 1.83 this order once failed
// to compile while the Beast/Asio session templates were instantiated in
// application code; compiling this file guards against a regression.
//

#include <boost/test/unit_test.hpp>

#include <memory>

#include "mcp.hpp"
#include "json_fields.hpp"
#include "provider.hpp"
#include "server.hpp"

BOOST_AUTO_TEST_SUITE(include_order)

BOOST_AUTO_TEST_CASE(server_after_mcp)
{
  auto responses = std::make_shared<response_map>();
  add(*responses, "/mcp", mcp({.name = "include_order", .version = "1.0"}));
  add(*responses, "/api", json_rpc());
  BOOST_TEST(responses->size() == 2u);

  void (*start)(tcp::endpoint const&, const int, std::shared_ptr<response_map>,
                server_control*, authorizer) = &start_server;
  BOOST_TEST(start != nullptr);

  BOOST_TEST(get_field(boost::json::parse(R"({"a":"b"})"), "a").value_or("") == "b");
  }

BOOST_AUTO_TEST_SUITE_END()
