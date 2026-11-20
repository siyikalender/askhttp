//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Logging (log.hpp): levels, the sink, escaping of control characters and
// replacing the sink while other threads log.
//

#include <boost/test/unit_test.hpp>

#include <mutex>
#include <thread>
#include <vector>

#include "log.hpp"

namespace
{

// Captures messages for the lifetime of the object; restores the default
// sink and level afterwards.
struct capture
{
  std::mutex                mutex;
  std::vector<std::string>  lines;

  capture()
  {
    set_log_sink([this](log_level l, std::string_view m)
    {
      std::lock_guard<std::mutex> lock(mutex);
      lines.push_back(std::string(log_level_name(l)) + "|" + std::string(m));
    });
  }

  ~capture()
  {
    set_log_sink({});
    set_log_level(log_level::info);
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(logging)

// log: default level is info
BOOST_AUTO_TEST_CASE(default_level_info)
{
  capture c;
  log_debug("hidden");
  log_info("port ", 8080);
  log_error("x");
  BOOST_CHECK_EQUAL(c.lines.size(), 2u);
  BOOST_CHECK_EQUAL(c.lines.at(0), "info|port 8080");
  BOOST_CHECK_EQUAL(c.lines.at(1), "error|x");
}

// log: set_log_level filters
BOOST_AUTO_TEST_CASE(level_filter)
{
  capture c;
  set_log_level(log_level::debug);
  log_debug("a");
  set_log_level(log_level::error);
  log_warning("b");
  BOOST_CHECK_EQUAL(c.lines.size(), 1u);
  BOOST_CHECK_EQUAL(c.lines.at(0), "debug|a");
}

// log: control characters cannot forge lines
BOOST_AUTO_TEST_CASE(control_characters_escaped)
{
  capture c;
  log_warning("name 'x\nFAKE\r\t\x01'");
  BOOST_CHECK_EQUAL(c.lines.at(0), "warning|name 'x\\nFAKE\\r\\t\\x01'");
}

// log: concurrent logging while the sink is replaced
BOOST_AUTO_TEST_CASE(concurrent_logging)
{
  capture c;
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([] { for (int k = 0; k < 1000; ++k) log_info("x", k); });

  for (int k = 0; k < 100; ++k)
    set_log_sink([](log_level, std::string_view) {});

  for (auto& t : threads)
    t.join();

  BOOST_CHECK(true);   // the test is that this completes without a crash
}

BOOST_AUTO_TEST_SUITE_END()
