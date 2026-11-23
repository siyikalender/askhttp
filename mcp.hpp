//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Model Context Protocol server endpoint, protocol revision 2026-07-28,
// Streamable HTTP transport (https://modelcontextprotocol.io/specification/2026-07-28).
//
// Only tools are offered: server/discover, tools/list and tools/call.
// Every response is a single JSON object (no SSE stream); the protocol is
// stateless, so there are no sessions.
//
// Clients of the earlier revisions 2025-11-25, 2025-06-18 and 2025-03-26
// (initialize handshake; e.g. the llama.cpp web UI and other clients built
// on older SDKs) are served too, unless mcp_options::legacy_versions is
// false. The server stays stateless for them: it sends no Mcp-Session-Id,
// which those revisions allow.
//
//   auto* rpc = json_rpc();                       // frontend endpoint
//   rpc->add("find_record", find_record);
//   add(*responses, "/api", rpc);
//
//   auto* m = mcp({.name = "example", .version = "1.0"});
//   m->add_tool({.name        = "find_record",
//                .description = "Finds a record by id.",
//                .input_schema = parse(R"({"type":"object",
//                                          "properties":{"id":{"type":"integer"}},
//                                          "required":["id"]})"),
//                .read_only   = true},
//               rpc->handler("find_record"));      // same handler as the frontend
//   add(*responses, "/mcp", m);
//
// A tool handler is an rpc_handler: it receives the call's "arguments" and
// returns a JSON value. That value is sent as text in content; a JSON object
// is also sent as structuredContent (the protocol allows only objects there). If the handler throws rpc_error, the model receives its
// message as a tool error (isError: true) and can correct its call; other
// exceptions are logged and reported as "Internal error".
//
// The implementation is in mcp.cpp.
//

#ifndef ASKHTTP_MCP_HPP
#define ASKHTTP_MCP_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

// The include order of provider.hpp, server.hpp and this header does not
// matter: none of them instantiates Boost.Asio templates any more.
#include "provider.hpp"
#include "json_rpc.hpp"

namespace mcp_code
{
  constexpr int header_mismatch              = -32020;
  constexpr int unsupported_protocol_version = -32022;
}

// Revisions before 2026-07-28 that are served when
// mcp_options::legacy_versions is set; the first is the latest.
inline constexpr const char* mcp_legacy_versions[] = {"2025-11-25", "2025-06-18", "2025-03-26"};

struct mcp_options
{
  // Server identity, sent as serverInfo (required).
  std::string name    = {};
  std::string version = {};

  // Optional guidance for the model on how to use this server.
  std::string instructions = {};

  // Values of the Origin header that are accepted, e.g.
  // "http://127.0.0.1:8080" (scheme, host and port, no trailing slash).
  // Browsers send Origin; MCP clients that are not browsers usually do not.
  // A request with an Origin that is not listed is refused with 403 (DNS
  // rebinding protection). Listed origins are also allowed cross-origin
  // access (CORS), so a web page served from them, such as a chat UI, can
  // call the server. Empty: every request that carries Origin is refused.
  std::vector<std::string> allowed_origins = {};

  // Caching hints for server/discover and tools/list.
  std::uint64_t ttl_ms = 300000;
  std::string   cache_scope = "private";   // "public" only if the result never depends on credentials

  // Also serve clients of the revisions before 2026-07-28 (see above).
  bool legacy_versions = true;
};

struct mcp_tool
{
  std::string         name          = {};   // 1-128 of A-Z a-z 0-9 _ - .
  std::string         title         = {};   // optional, for display
  std::string         description   = {};   // what the tool does, for the model
  boost::json::value  input_schema  = {};   // JSON Schema object; null: {"type":"object"}
  boost::json::value  output_schema = {};   // optional; null: none

  // Behaviour hints (annotations). Clients treat them as untrusted.
  std::optional<bool> read_only     = {};   // readOnlyHint
  std::optional<bool> destructive   = {};   // destructiveHint
  std::optional<bool> idempotent    = {};   // idempotentHint
  std::optional<bool> open_world    = {};   // openWorldHint
};

class mcp_provider : public response_provider
{
public:
  static constexpr const char* protocol_version = "2026-07-28";

  // Throws std::invalid_argument if name or version is empty.
  explicit mcp_provider(mcp_options p_options);

  // Registers a tool. Must only be called before the server is started.
  // Throws std::invalid_argument for an invalid or duplicate name, an
  // empty handler or an input_schema that is not an object.
  void add_tool(mcp_tool p_tool, rpc_handler p_handler);

  std::optional<response_data> respond(const request_context& ctx) const override;

  // Always empty: only respond() is meaningful for MCP.
  std::optional<std::string>
  content(const std::string& request_body, const query& request_query) const override;

  string_view mime_type() const override;

  types type() const override;

  // Browser pages listed in allowed_origins may call the server (CORS).
  bool cors_allowed(std::string_view p_origin) const override;

private:
  struct tool_entry
  {
    mcp_tool     def;
    rpc_handler  handler;
  };

  struct tool_outcome;

  boost::json::object server_meta() const;

  boost::json::object discover() const;

  boost::json::object list_tools() const;

  boost::json::array tool_array() const;

  response_data call_tool(const request_context&      ctx,
                          const boost::json::value&   id,
                          const boost::json::object&  params) const;

  tool_outcome run_tool(const boost::json::value&   id,
                        const boost::json::object&  params,
                        const std::string&          tool_name) const;

  response_data respond_legacy(const std::string&           method,
                               const boost::json::value&    id,
                               const boost::json::value*    params,
                               const std::string&           version_header) const;

  mcp_options               options_;
  std::vector<tool_entry>   tools_;
};

mcp_provider* mcp(mcp_options p_options);

// ASKHTTP_MCP_HPP
#endif
