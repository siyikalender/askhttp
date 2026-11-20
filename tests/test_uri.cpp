//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// parse_uri() (parse_uri.hpp): splitting a request target into path and
// query, percent decoding and malformed input.
//

#include <boost/test/unit_test.hpp>
#include "parse_uri.hpp"

namespace
{

struct parsed
{
  std::string path;
  query       q;
};

parsed parse(const std::string& uri)
{
  parsed p;
  parse_uri(uri, p.path, p.q);
  return p;
}

std::string value(const query& q, const std::string& key)
{
  auto it = q.find(key);
  if (it == q.end()) return "<missing>";
  if (!it->second)   return "<none>";
  return *it->second;
}

} // namespace

BOOST_AUTO_TEST_SUITE(uri)

// parse_uri: path only
BOOST_AUTO_TEST_CASE(path_only)
{
  auto p = parse("/api");
  BOOST_CHECK_EQUAL(p.path, "/api");
  BOOST_CHECK(p.q.empty());
}

// parse_uri: empty input, and '=' without '?' belongs to the path
BOOST_AUTO_TEST_CASE(empty_and_no_question_mark)
{
  auto e = parse("");
  BOOST_CHECK_EQUAL(e.path, "");
  BOOST_CHECK(e.q.empty());

  auto p = parse("key1=value1");
  BOOST_CHECK_EQUAL(p.path, "key1=value1");
  BOOST_CHECK(p.q.empty());
}

// parse_uri: query parameters
BOOST_AUTO_TEST_CASE(query_parameters)
{
  auto p = parse("/api?a=1&b=two");
  BOOST_CHECK_EQUAL(p.path, "/api");
  BOOST_CHECK_EQUAL(p.q.size(), 2u);
  BOOST_CHECK_EQUAL(value(p.q, "a"), "1");
  BOOST_CHECK_EQUAL(value(p.q, "b"), "two");
}

// parse_uri: parameter without '=' (used to crash the server)
BOOST_AUTO_TEST_CASE(parameter_without_value)
{
  auto p = parse("/api?a=1&b&c=2");
  BOOST_CHECK_EQUAL(value(p.q, "a"), "1");
  BOOST_CHECK_EQUAL(value(p.q, "b"), "<none>");
  BOOST_CHECK_EQUAL(value(p.q, "c"), "2");

  auto t = parse("path?key1=value1&key2");
  BOOST_CHECK_EQUAL(value(t.q, "key1"), "value1");
  BOOST_CHECK_EQUAL(value(t.q, "key2"), "<none>");
}

// parse_uri: empty parts, '=' in value, empty value
BOOST_AUTO_TEST_CASE(empty_parts_and_values)
{
  auto p = parse("/x?&&k=v=w&e=&");
  BOOST_CHECK_EQUAL(p.q.size(), 2u);
  BOOST_CHECK_EQUAL(value(p.q, "k"), "v=w");
  BOOST_CHECK_EQUAL(value(p.q, "e"), "");

  auto t = parse("path?&&a=1&");
  BOOST_CHECK_EQUAL(t.path, "path");
  BOOST_CHECK_EQUAL(t.q.size(), 1u);
  BOOST_CHECK_EQUAL(value(t.q, "a"), "1");
}

// parse_uri: percent decoding
BOOST_AUTO_TEST_CASE(percent_decoding)
{
  auto p = parse("/yol%20bir+iki?ad=Ali+Veli&sehir=%C4%B0zmir&p=%2B1");
  BOOST_CHECK_EQUAL(p.path, "/yol bir+iki");          // '+' is a space only in the query
  BOOST_CHECK_EQUAL(value(p.q, "ad"), "Ali Veli");
  BOOST_CHECK_EQUAL(value(p.q, "sehir"), "\xC4\xB0zmir");
  BOOST_CHECK_EQUAL(value(p.q, "p"), "+1");
}

// parse_uri: malformed percent sequences are kept
BOOST_AUTO_TEST_CASE(malformed_percent_sequences)
{
  auto p = parse("/x?a=%zz&b=%4&c=%");
  BOOST_CHECK_EQUAL(value(p.q, "a"), "%zz");
  BOOST_CHECK_EQUAL(value(p.q, "b"), "%4");
  BOOST_CHECK_EQUAL(value(p.q, "c"), "%");
}

// parse_uri: first occurrence of a key wins
BOOST_AUTO_TEST_CASE(first_key_wins)
{
  auto p = parse("/x?a=1&a=2");
  BOOST_CHECK_EQUAL(value(p.q, "a"), "1");
}

BOOST_AUTO_TEST_SUITE_END()
