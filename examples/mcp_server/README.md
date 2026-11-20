<!-- Author: Claude (Anthropic) -->
<!-- SPDX-License-Identifier: Unlicense -->

# Example: MCP server with libaskhttp

A small in-memory note store (`main.cpp`). The same three handlers are
served twice:

| Endpoint | Protocol | For |
|---|---|---|
| `POST /api` | JSON-RPC 2.0 | a web frontend |
| `POST /mcp` | MCP 2026-07-28 (and 2025-03-26 to 2025-11-25) | AI clients (Claude Code, llama.cpp web UI, MCP Inspector, ...) |

Tools: `add_note`, `list_notes` (read-only), `delete_note` (destructive).

## Build and run

From the library directory:

```sh
cmake --preset debug && cmake --build --preset debug --target mcp_example
MCP_TOKEN=change-me ./build/debug/mcp_example 8080
```

The server listens on `127.0.0.1` only. Without `MCP_TOKEN` it accepts
requests without authentication and logs a warning. Stop it with Ctrl+C:
requests in progress are finished first.

## Try it with curl

JSON-RPC (frontend endpoint):

```sh
curl -s http://127.0.0.1:8080/api \
  -H 'Authorization: Bearer change-me' \
  -d '{"jsonrpc":"2.0","id":1,"method":"add_note","params":{"text":"milk"}}'
# {"jsonrpc":"2.0","id":1,"result":{"id":1}}
```

MCP: every request carries the protocol version and the method both in
the body and in headers.

```sh
META='"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}'

# list the tools
curl -s http://127.0.0.1:8080/mcp \
  -H 'Authorization: Bearer change-me' \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/list' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{'"$META"'}}'

# call a tool (Mcp-Name must equal params.name)
curl -s http://127.0.0.1:8080/mcp \
  -H 'Authorization: Bearer change-me' \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' \
  -H 'Mcp-Name: list_notes' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_notes","arguments":{},'"$META"'}}'
```

## Connect Claude Code

```sh
claude mcp add --transport http notes http://127.0.0.1:8080/mcp \
  --header "Authorization: Bearer change-me"
```

Then ask, for example, "add a note: buy milk" or "which notes do I have?".
Claude Code asks for confirmation before calling a tool such as
`delete_note`.

The server speaks MCP 2026-07-28 and, for clients built on older SDKs,
also the revisions 2025-11-25, 2025-06-18 and 2025-03-26 (`initialize`
handshake, no sessions). Set `.legacy_versions = false` in `mcp_options`
to accept 2026-07-28 only.

## Browser-based clients (llama.cpp web UI and others)

A chat UI running in the browser calls the server from another origin
(e.g. the page is `http://127.0.0.1:8080`, the MCP server
`http://127.0.0.1:39280`). The browser then sends an `OPTIONS` preflight
first and only continues if the server allows that origin (CORS). List the
page's origin, exactly as the browser sends it in the `Origin` header
(scheme, host and port, no trailing slash):

```sh
MCP_ALLOWED_ORIGINS=http://127.0.0.1:8080 MCP_TOKEN=change-me \
  ./build/debug/mcp_example 39280
```

Several origins are separated by commas. Note that `http://localhost:8080`
and `http://127.0.0.1:8080` are different origins. A request from an
origin that is not listed is refused with `403`.

If the client cannot send an `Authorization` header, run without
`MCP_TOKEN` only while the server is bound to `127.0.0.1`.

llama-server can also forward MCP requests itself (`--webui-mcp-proxy`);
the browser then does not call the MCP server directly and CORS is not
involved.

## What the example shows

- one handler serving JSON-RPC and MCP (`rpc->handler("add_note")`)
- errors: `rpc_error` becomes a JSON-RPC error on `/api` and a tool error
  (`isError: true`) on `/mcp`; a missing field gives -32602 (`get_required_field()`)
- typed access with `find_field()` (`delete_note` requires an integer id)
- thread safety: handlers run concurrently, the store uses a mutex
- authentication with `authorizer`, `bearer_token()` and
  `constant_time_equals()`
- graceful shutdown with `server_control` from a signal handler
- cross-origin access (CORS) for browser clients with `allowed_origins`
