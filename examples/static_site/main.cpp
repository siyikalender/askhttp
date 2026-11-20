//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Minimal static site on libaskhttp: plain HTTP serving HTML, JS and CSS.
//
// Build and run (see README.md):
//   cmake --preset debug && cmake --build --preset debug --target static_example
//   ./build/debug/static_example 8080 [www_dir]
//
// Stop with Ctrl+C or SIGTERM: requests in progress are finished first.
//

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "server.hpp"

// Set by CMake to the absolute path of examples/static_site/www.
#ifndef STATIC_EXAMPLE_WWW_DIR
#define STATIC_EXAMPLE_WWW_DIR "www"
#endif

namespace
{

// stop() only sets an atomic flag, so it may be called from a signal handler.
server_control control;

void on_signal(int)
{
  control.stop();
}

} // namespace

int main(int argc, char** argv)
{
  unsigned short const port = static_cast<unsigned short>(argc > 1 ? std::atoi(argv[1]) : 8080);
  std::string const www = argc > 2 ? argv[2] : STATIC_EXAMPLE_WWW_DIR;

  // Paths are matched exactly: only these files can be requested.
  auto responses = std::make_shared<response_map>();
  add(*responses, "/",           file(www + "/index.html"));
  add(*responses, "/index.html", file(www + "/index.html"));
  add(*responses, "/style.css",  file(www + "/style.css"));
  add(*responses, "/app.js",     file(www + "/app.js"));

  for (char const* name : {"index.html", "style.css", "app.js"})
  {
    if (!std::filesystem::exists(www + "/" + name))
      log_warning("missing file: ", www, "/", name);
  }

  std::signal(SIGINT,  on_signal);
  std::signal(SIGTERM, on_signal);

  // Bound to 127.0.0.1 only. Use another address to reach it from the
  // local network.
  log_info("listening on http://127.0.0.1:", port, "  (serving ", www, ")");

  try
  {
    start_server(tcp::endpoint(net::ip::make_address("127.0.0.1"), port),
                 2, responses, &control);
  }
  catch (std::exception const& e)
  {
    log_error("server: ", e.what());   // e.g. "bind: Address already in use"
    return 1;
  }

  log_info("stopped");
  return 0;
}
