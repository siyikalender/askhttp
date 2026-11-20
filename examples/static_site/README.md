<!-- Author: Claude (Anthropic) -->
<!-- SPDX-License-Identifier: Unlicense -->

# Example: static site with libaskhttp

The simplest use of the library (`main.cpp`): a plain HTTP server that
serves an HTML page, a stylesheet and a script from `www/` with `file()`
providers in a `response_map`.

| Path | File | Content-Type |
|---|---|---|
| `/`, `/index.html` | `www/index.html` | `text/html` |
| `/style.css` | `www/style.css` | `text/css` |
| `/app.js` | `www/app.js` | `application/javascript` |

When the page loads, the script sets the status line to "JavaScript
loaded"; the button shows a greeting with the current time.

## Build and run

From the library directory:

```sh
cmake --preset debug && cmake --build --preset debug --target static_example
./build/debug/static_example 8080
```

Then open <http://127.0.0.1:8080/> in a browser.

Usage: `static_example [port] [www_dir]`. The port defaults to 8080. The
default `www_dir` is the absolute path of `examples/static_site/www`, set
by CMake, so the program works from any working directory. Pass a second
argument to serve another directory.

The server listens on `127.0.0.1` only. Stop it with Ctrl+C: requests in
progress are finished first.

## Try it with curl

```sh
curl -i http://127.0.0.1:8080/style.css   # 200, Content-Type: text/css
curl -I http://127.0.0.1:8080/            # HEAD: headers only
curl -i http://127.0.0.1:8080/nope        # 404
```

## Notes

- Only the registered paths are served; anything else gets `404`. There is
  no directory listing and no prefix matching, so no path traversal.
- Files are read on every request, so edits show up without a restart.
- Plain HTTP is for development only. Use `start_ssl_server()` or a
  reverse proxy that terminates TLS in production.
