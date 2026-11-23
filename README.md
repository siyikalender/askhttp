<!-- Authors: Selçuk İyikalender, Claude (Anthropic) -->
<!-- SPDX-License-Identifier: Unlicense -->

# libaskhttp

A small C++20 library for building HTTP(S) backends on Boost.Beast. It is
built as a static library (`libaskhttp.a`). Most of it is header-only.

An application registers **response providers** under URL paths and starts
the server. The library has providers for:

- JSON-RPC 2.0 endpoints (for web frontends)
- Model Context Protocol (MCP) endpoints (for AI clients). They can reuse
  the same handlers as JSON-RPC.
- Static files and generated text
- File uploads and downloads with content checks

## Features

- **HTTP and HTTPS server** (`server.hpp`)
  - Runs on several threads.
  - Supports keep-alive and HEAD requests.
  - HTTPS allows TLS 1.2 and 1.3 only, with Mozilla "intermediate" cipher
    suites.
  - `server_control` stops the server gracefully. It is safe to call from a
    signal handler.
  - An optional `authorizer` checks every request before the path is looked
    up. Helpers: `bearer_token()` and `constant_time_equals()`.
  - CORS is enabled per resource, including preflight handling.
  - Request bodies are limited to 1 MB by default. Uploads have their own
    limits, which are checked before the body is read.
- **JSON-RPC 2.0** (`json_rpc.hpp`, `provider.hpp`)
  - Supports batches and notifications.
  - A handler throws `rpc_error` to return an error to the client.
  - Any other exception is logged and sent to the client only as
    `-32603 Internal error`.
- **MCP server** (`mcp.hpp`)
  - Implements protocol revision 2026-07-28 over Streamable HTTP. It is
    stateless and offers tools only.
  - Also serves clients of revisions 2025-11-25, 2025-06-18 and 2025-03-26.
  - Checks the `Origin` header against an allow list to protect against DNS
    rebinding.
- **Uploads and downloads** (`provider.hpp`, `file_format.hpp`)
  - An upload is accepted only if both its file extension and its magic
    bytes match an allowed format.
  - Files are stored under random names and written atomically (temporary
    file, then rename).
  - The number of concurrent uploads is limited.
  - Downloads use a safe `Content-Disposition`. Formats that can carry
    script, such as SVG, are always sent as attachments.
- **Logging** (`log.hpp`)
  - Levelled logging with a replaceable sink.
  - Control characters are escaped, so request data cannot forge log lines.
  - Request and response bodies are never logged.

## Layout

| File | Contents |
|---|---|
| `server.hpp` | Sessions, listener, `start_server()` / `start_ssl_server()`, `server_control`, `authorizer` |
| `provider.hpp` | `response_provider` base class; `text`, `file`, `json_rpc`, `upload`, `download` providers; `response_map`, `add()` |
| `mcp.hpp` | `mcp_provider`, `mcp_options`, `mcp_tool` |
| `json_rpc.hpp` | `rpc_error`, `rpc_handler`, error codes |
| `file_format.hpp` | Built-in upload formats (PDF, images, Office, OpenDocument, ...) |
| `json_fields.hpp` | Field access in request parameters: `find_field()`, `get_field()`, `get_required_field()`, `to_text()` |
| `json_writer.hpp` | `std::string& operator<<` overloads for building JSON text, `make_result()` |
| `json_escape.*` | `json_escape()`: JSON string escaping |
| `log.hpp` | `log_error/warning/info/debug`, `set_log_sink()`, `set_log_level()` |
| `parse_uri.*`, `query.hpp` | Splits a request target into a path and decoded query parameters; `get_param()` |
| `read_stream.hpp` | `read_whole_stream()` |
| `mime_type.hpp`, `string_view.hpp` | Small helpers |
| `json_impl.cpp` | Compiles Boost.JSON (header-only mode) into the library |
| `CMakeLists.txt` | Build: library, tests, examples |
| `CMakePresets.json` | `debug` and `release` presets |
| `tests/` | Boost.Test unit and end-to-end tests |
| `examples/mcp_server/` | Example server with JSON-RPC and MCP endpoints ([README](examples/mcp_server/README.md)) |
| `examples/static_site/` | Minimal plain HTTP server serving HTML, JS and CSS ([README](examples/static_site/README.md)) |

## Requirements

- CMake 3.21 or newer
- A C++20 compiler (GCC or Clang)
- Boost 1.75 or newer (header-only parts: Beast/Asio, JSON, Test, Optional)
- OpenSSL (`libssl`, `libcrypto`)

Boost and OpenSSL are found with `find_package`. If Boost has no CMake
package, `boost/version.hpp` is searched in the standard include paths and
under `CMAKE_PREFIX_PATH`. You can also set `ASKHTTP_BOOST_INCLUDE_DIR`
directly. For a non-standard OpenSSL location set `OPENSSL_ROOT_DIR`.

## Building

```sh
cmake --preset debug   && cmake --build --preset debug     # -> build/debug/libaskhttp.a
cmake --preset release && cmake --build --preset release   # -O2 -DNDEBUG
cmake --build --preset debug --target clean
```

The presets also write `compile_commands.json` (for clangd). For cross
compiling, pass a toolchain file: `cmake --preset release --toolchain <file>`.

**Using the library from CMake:**

```cmake
add_subdirectory(path/to/askhttp)
target_link_libraries(app PRIVATE askhttp::askhttp)
```

This brings in the include directories, C++20, Boost, OpenSSL and threads.
The options `ASKHTTP_BUILD_TESTS` and `ASKHTTP_BUILD_EXAMPLES` are ON only
when the library is built on its own. Without CMake, link
`libaskhttp.a -pthread -lssl -lcrypto`.

**Boost.JSON:** `json_impl.cpp` compiles the Boost.JSON implementation into
the library. If your application also links `libboost_json` or includes
`<boost/json/src.hpp>`, configure with `-DASKHTTP_COMPILE_BOOST_JSON=OFF`.
Otherwise the program has duplicate symbols.

## Usage

```cpp
#include "server.hpp"
#include "mcp.hpp"
#include "json_fields.hpp"

auto responses = std::make_shared<response_map>();

auto* rpc = json_rpc();
rpc->add("echo", [](const boost::json::value& p) -> boost::json::value
{
  return {{"text", get_required_field(p, "text")}};   // missing field -> -32602
});
add(*responses, "/api", rpc);

auto* m = mcp({.name = "my-server", .version = "1.0"});
m->add_tool({.name = "echo", .description = "Echoes the text."},
            rpc->handler("echo"));                // same handler as /api
add(*responses, "/mcp", m);

add(*responses, "/", file("www/index.html"));

server_control control;   // call control.stop() to shut down gracefully
start_server(tcp::endpoint(net::ip::make_address("127.0.0.1"), 8080),
             2 /* threads */, responses, &control);
```

Rules to follow:

- Register all providers before the server starts, and do not change them
  afterwards. Handlers run concurrently on server threads, so they must be
  thread-safe.
- `start_ssl_server()` takes a certificate chain file, a key file and the
  key password. Pass the password in at run time, for example from the
  environment. Do not compile it into the program.
- `start_server()` speaks plain HTTP. Use it only for development, on a
  trusted network, or behind a reverse proxy that terminates TLS.

The example in [`examples/mcp_server`](examples/mcp_server/README.md) shows
a complete program, including authentication, signal handling and CORS.

```sh
cmake --build --preset debug --target mcp_example
MCP_TOKEN=change-me ./build/debug/mcp_example 8080
```

For the simplest case, serving static files over plain HTTP, see
[`examples/static_site`](examples/static_site/README.md)
(`--target static_example`).

## Tests

```sh
ctest --preset debug                                          # unit and end-to-end tests
./build/debug/askhttp_tests --run_test=request_handling       # one suite
./build/debug/askhttp_tests --run_test="json/get_*" --log_level=all
```

| Suite | File | Covers |
|---|---|---|
| `uri` | `tests/test_uri.cpp` | URI and query parsing |
| `json` | `tests/test_json.cpp` | JSON helpers, providers, JSON-RPC |
| `request_handling` | `tests/test_request_handling.cpp` | Request handling: methods, status codes, CORS, auth |
| `server` | `tests/test_server.cpp` | End-to-end tests against a live server on 127.0.0.1 |
| `mcp_endpoint` | `tests/test_mcp_endpoint.cpp` | MCP protocol, legacy revisions, header checks |
| `file_upload` | `tests/test_file_upload.cpp` | Upload and download providers, file formats |
| `logging` | `tests/test_logging.cpp` | Log levels, sinks, escaping |

The end-to-end tests use port `ASKHTTP_TEST_PORT`, which defaults to 18089.

## Conventions

- **File header**, in this order: authors, SPDX license identifier, a
  paragraph that says what the file contains.

  ```cpp
  //
  // Authors: Selçuk İyikalender
  //          Claude (Anthropic)
  //
  // SPDX-License-Identifier: Unlicense
  //
  // What this file contains.
  //
  ```

  A file with a single author uses `Author:`. Use `#` comments in shell
  scripts, `CMakeLists.txt` and `.gitignore`, and `<!-- ... -->` in
  Markdown. JSON files (`CMakePresets.json`) have no header, because JSON
  has no comments. Upstream notices (`server.hpp`) stay first and
  unchanged.
- **Include guards:** `ASKHTTP_<FILENAME>_HPP`, closed with
  `#endif // ASKHTTP_<FILENAME>_HPP`.
- **File names** say what is inside (`json_escape.hpp`, not
  `conversion.hpp`). All library files stay flat in this directory.
- **Style:** `using` instead of `typedef`, no `extern` on function
  declarations, 2-space indentation, English identifiers, comments and log
  messages.
- **Tests:** `tests/test_<suite>.cpp` contains `BOOST_AUTO_TEST_SUITE(<suite>)`.
  A suite opens a namespace of the same name, so a suite must not be named
  after a global function of the library (`parse_uri`, `upload`, `mcp`,
  `log`, ...).

## License

Released into the public domain under the Unlicense (see
`LICENSE-Unlicense`).

Exception: `server.hpp` is based on the Boost.Beast examples and remains
under the Boost Software License 1.0 (see `LICENSE-BSL-1.0`).

See the header of each file for its authors.
