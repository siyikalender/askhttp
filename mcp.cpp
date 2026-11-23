//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Model Context Protocol server endpoint: request validation, the
// 2026-07-28 methods, the legacy (initialize based) revisions and tool
// execution. See mcp.hpp for the interface.
//

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <boost/json.hpp>

#include "mcp.hpp"
#include "json_rpc.hpp"
#include "log.hpp"

namespace
{

// Decodes standard Base64; std::nullopt if the input is not valid.
std::optional<std::string> base64_decode(std::string_view p_in)
{
  auto value_of = [](char c) -> int
  {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  if (p_in.size() % 4 != 0)
    return std::nullopt;

  std::string out;
  for (std::size_t i = 0; i < p_in.size(); i += 4)
  {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k)
    {
      char const c = p_in[i + k];
      if (c == '=' && i + 4 == p_in.size() && k >= 2)
      {
        v[k] = 0;
        ++pad;
        continue;
      }
      if (pad)
        return std::nullopt;   // data after padding
      v[k] = value_of(c);
      if (v[k] < 0)
        return std::nullopt;
    }
    std::uint32_t const n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out += static_cast<char>((n >> 16) & 0xff);
    if (pad < 2) out += static_cast<char>((n >> 8) & 0xff);
    if (pad < 1) out += static_cast<char>(n & 0xff);
  }
  return out;
}

// Header value as sent: plain, or "=?base64?...?=" (MCP value encoding).
std::optional<std::string> decode_mcp_header(const std::string& p_value)
{
  std::string_view const prefix = "=?base64?";
  std::string_view const suffix = "?=";
  std::string_view v = p_value;

  if (v.size() >= prefix.size() + suffix.size() &&
      v.substr(0, prefix.size()) == prefix &&
      v.substr(v.size() - suffix.size()) == suffix)
    return base64_decode(v.substr(prefix.size(), v.size() - prefix.size() - suffix.size()));

  return p_value;
}

bool valid_tool_name(const std::string& p_name)
{
  if (p_name.empty() || p_name.size() > 128)
    return false;
  return std::all_of(p_name.begin(), p_name.end(), [](char c)
  {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
  });
}

bool is_legacy_version(const std::string& p_version)
{
  for (auto const* v : mcp_legacy_versions)
    if (p_version == v)
      return true;
  return false;
}

response_data ok(const boost::json::value& id, boost::json::object result)
{
  return response_data{boost::json::serialize(detail::rpc_result(id, std::move(result))),
                       "application/json", {}, 200};
}

// An error response; without id when the request id is unknown.
response_data error(unsigned                                    status,
                    const std::optional<boost::json::value>&    id,
                    int                                         code,
                    const std::string&                          message,
                    const boost::json::value&                   data = nullptr)
{
  auto o = detail::rpc_error_response(id ? *id : boost::json::value(nullptr), code, message, data);
  if (!id)
  {
    boost::json::object without_id;
    without_id["jsonrpc"] = "2.0";
    without_id["error"]   = o["error"];
    o = without_id;
  }
  return response_data{boost::json::serialize(o), "application/json", {}, status};
}

} // namespace

// Result of run_tool: the tool result (content, structuredContent,
// isError) shared by all revisions, or a protocol error as an error
// response.
struct mcp_provider::tool_outcome
{
  std::optional<boost::json::object> result;
  std::optional<response_data>       failure;
  explicit operator bool() const { return result.has_value(); }
  boost::json::object& operator*() { return *result; }
  boost::json::object* operator->() { return &*result; }
  response_data& error() { return *failure; }
};

mcp_provider::mcp_provider(mcp_options p_options)
: options_(std::move(p_options))
{
  if (options_.name.empty() || options_.version.empty())
    throw std::invalid_argument("mcp: name and version are required");
}

void mcp_provider::add_tool(mcp_tool p_tool, rpc_handler p_handler)
{
  if (!valid_tool_name(p_tool.name))
    throw std::invalid_argument("mcp: invalid tool name '" + p_tool.name + "'");
  if (!p_handler)
    throw std::invalid_argument("mcp: handler is required for tool '" + p_tool.name + "'");
  for (auto const& t : tools_)
    if (t.def.name == p_tool.name)
      throw std::invalid_argument("mcp: duplicate tool '" + p_tool.name + "'");

  if (p_tool.input_schema.is_null())
  {
    boost::json::object schema;
    schema["type"] = "object";
    p_tool.input_schema = schema;
  }
  if (!p_tool.input_schema.is_object())
    throw std::invalid_argument("mcp: input_schema of '" + p_tool.name + "' must be an object");

  tools_.push_back({std::move(p_tool), std::move(p_handler)});
}

std::optional<response_data> mcp_provider::respond(const request_context& ctx) const
{
  // DNS rebinding protection: a browser page from another site must not
  // reach a local MCP server.
  std::string const origin = ctx.header("Origin");
  if (!origin.empty() &&
      std::find(options_.allowed_origins.begin(), options_.allowed_origins.end(), origin)
        == options_.allowed_origins.end())
  {
    log_warning("mcp: origin not allowed: ", origin);
    return error(403, std::nullopt, rpc_code::invalid_request, "Origin not allowed");
  }

  boost::json::error_code ec;
  boost::json::value const root = boost::json::parse(ctx.body, ec);
  if (ec)
    return error(400, std::nullopt, rpc_code::parse_error, "Parse error");

  // One JSON-RPC message per POST; batches are not part of MCP.
  auto const* msg = root.if_object();
  auto const* ver = msg ? msg->if_contains("jsonrpc") : nullptr;
  auto const* met = msg ? msg->if_contains("method") : nullptr;
  if (!msg || !ver || !ver->is_string() || ver->as_string() != "2.0" ||
      !met || !met->is_string())
    return error(400, std::nullopt, rpc_code::invalid_request, "Invalid Request");

  std::string const method(met->as_string().data(), met->as_string().size());

  // A notification: accepted, no response body. (This revision defines
  // no client-to-server notifications over HTTP.)
  auto const* id = msg->if_contains("id");
  if (!id)
    return response_data{{}, {}, {}, 202};

  // MCP request ids are strings or integers, never null.
  if (!id->is_string() && !id->is_int64() && !id->is_uint64())
    return error(400, std::nullopt, rpc_code::invalid_request, "Invalid Request: id must be a string or an integer");

  // Revisions before 2026-07-28: the client starts with initialize, and
  // later requests carry the negotiated version in MCP-Protocol-Version
  // (no header: 2025-03-26 is assumed, as those revisions require). A
  // 2026-07-28 client always sends the version in params._meta, so a
  // request without the header but with _meta is a 2026-07-28 request
  // with a missing header, not a legacy one.
  std::string const version_header = ctx.header("MCP-Protocol-Version");
  if (options_.legacy_versions)
  {
    auto const* lparams = msg->if_contains("params");
    auto const* lmeta = lparams && lparams->is_object()
                          ? lparams->as_object().if_contains("_meta") : nullptr;
    bool const has_meta = lmeta && lmeta->is_object() &&
                          lmeta->as_object().if_contains("io.modelcontextprotocol/protocolVersion");
    if (method == "initialize" || is_legacy_version(version_header) ||
        (version_header.empty() && !has_meta))
      return respond_legacy(method, *id, lparams, version_header);
  }

  // Request metadata headers must be present and match the body.
  std::string const method_header  = ctx.header("Mcp-Method");
  if (version_header.empty())
    return error(400, *id, mcp_code::header_mismatch, "Header mismatch: MCP-Protocol-Version header is missing");
  if (method_header.empty())
    return error(400, *id, mcp_code::header_mismatch, "Header mismatch: Mcp-Method header is missing");
  if (method_header != method)
    return error(400, *id, mcp_code::header_mismatch,
                 "Header mismatch: Mcp-Method header value '" + method_header +
                 "' does not match body value '" + method + "'");

  // Every request carries its protocol version and client capabilities.
  auto const* params = msg->if_contains("params");
  auto const* meta   = params && params->is_object() ? params->as_object().if_contains("_meta") : nullptr;
  auto const* pver   = meta && meta->is_object() ? meta->as_object().if_contains("io.modelcontextprotocol/protocolVersion") : nullptr;
  auto const* pcap   = meta && meta->is_object() ? meta->as_object().if_contains("io.modelcontextprotocol/clientCapabilities") : nullptr;
  if (!pver || !pver->is_string() || !pcap || !pcap->is_object())
    return error(400, *id, rpc_code::invalid_params,
                 "Invalid params: _meta must contain io.modelcontextprotocol/protocolVersion and clientCapabilities");

  std::string const body_version(pver->as_string().data(), pver->as_string().size());
  if (body_version != version_header)
    return error(400, *id, mcp_code::header_mismatch,
                 "Header mismatch: MCP-Protocol-Version header value '" + version_header +
                 "' does not match body value '" + body_version + "'");

  if (body_version != protocol_version)
  {
    boost::json::object data;
    boost::json::array supported;
    supported.push_back(protocol_version);
    if (options_.legacy_versions)
      for (auto const* v : mcp_legacy_versions)
        supported.push_back(v);
    data["supported"] = supported;
    data["requested"] = body_version;
    return error(400, *id, mcp_code::unsupported_protocol_version, "Unsupported protocol version", data);
  }

  if (method == "server/discover")
    return ok(*id, discover());
  if (method == "tools/list")
    return ok(*id, list_tools());
  if (method == "tools/call")
    return call_tool(ctx, *id, params->as_object());

  return error(404, *id, rpc_code::method_not_found, "Method not found");
}

std::optional<std::string>
mcp_provider::content(const std::string&, const query&) const
{
  return std::nullopt;   // only respond() is meaningful for MCP
}

string_view mcp_provider::mime_type() const
{
  return "application/json";
}

response_provider::types mcp_provider::type() const
{
  return types::mcp;
}

bool mcp_provider::cors_allowed(std::string_view p_origin) const
{
  return std::find(options_.allowed_origins.begin(), options_.allowed_origins.end(), p_origin)
           != options_.allowed_origins.end();
}

boost::json::object mcp_provider::server_meta() const
{
  boost::json::object info;
  info["name"]    = options_.name;
  info["version"] = options_.version;
  boost::json::object meta;
  meta["io.modelcontextprotocol/serverInfo"] = info;
  return meta;
}

boost::json::object mcp_provider::discover() const
{
  boost::json::object r;
  r["resultType"] = "complete";
  boost::json::array versions;
  versions.push_back(protocol_version);
  r["supportedVersions"] = versions;
  boost::json::object capabilities;
  capabilities["tools"] = boost::json::object();
  r["capabilities"] = capabilities;
  if (!options_.instructions.empty())
    r["instructions"] = options_.instructions;
  r["ttlMs"]      = options_.ttl_ms;
  r["cacheScope"] = options_.cache_scope;
  r["_meta"]      = server_meta();
  return r;
}

boost::json::object mcp_provider::list_tools() const
{
  boost::json::object r;
  r["resultType"] = "complete";
  r["tools"]      = tool_array();
  r["ttlMs"]      = options_.ttl_ms;
  r["cacheScope"] = options_.cache_scope;
  r["_meta"]      = server_meta();
  return r;
}

boost::json::array mcp_provider::tool_array() const
{
  boost::json::array tools;
  for (auto const& t : tools_)   // registration order: deterministic
  {
    boost::json::object o;
    o["name"] = t.def.name;
    if (!t.def.title.empty())
      o["title"] = t.def.title;
    o["description"] = t.def.description;
    o["inputSchema"] = t.def.input_schema;
    if (!t.def.output_schema.is_null())
      o["outputSchema"] = t.def.output_schema;

    boost::json::object a;
    if (t.def.read_only)   a["readOnlyHint"]    = *t.def.read_only;
    if (t.def.destructive) a["destructiveHint"] = *t.def.destructive;
    if (t.def.idempotent)  a["idempotentHint"]  = *t.def.idempotent;
    if (t.def.open_world)  a["openWorldHint"]   = *t.def.open_world;
    if (!a.empty())
      o["annotations"] = a;

    tools.push_back(o);
  }
  return tools;
}

response_data mcp_provider::call_tool(const request_context&      ctx,
                                      const boost::json::value&   id,
                                      const boost::json::object&  params) const
{
  auto const* name = params.if_contains("name");
  if (!name || !name->is_string())
    return error(200, id, rpc_code::invalid_params, "Invalid params: name is required");

  std::string const tool_name(name->as_string().data(), name->as_string().size());

  // Mcp-Name must be present and equal params.name (after decoding).
  std::string const name_header = ctx.header("Mcp-Name");
  if (name_header.empty())
    return error(400, id, mcp_code::header_mismatch, "Header mismatch: Mcp-Name header is missing");
  auto const decoded = decode_mcp_header(name_header);
  if (!decoded || *decoded != tool_name)
    return error(400, id, mcp_code::header_mismatch,
                 "Header mismatch: Mcp-Name header does not match body value '" + tool_name + "'");

  auto r = run_tool(id, params, tool_name);
  if (!r)
    return std::move(r.error());
  (*r)["resultType"] = "complete";
  (*r)["_meta"] = server_meta();
  return ok(id, std::move(*r));
}

// Runs a tool and builds the result (content, structuredContent,
// isError) shared by all revisions; a protocol error as an error response.
mcp_provider::tool_outcome mcp_provider::run_tool(const boost::json::value&   id,
                                                  const boost::json::object&  params,
                                                  const std::string&          tool_name) const
{
  auto const it = std::find_if(tools_.begin(), tools_.end(),
                               [&](const tool_entry& t) { return t.def.name == tool_name; });
  if (it == tools_.end())
    return {std::nullopt, error(200, id, rpc_code::invalid_params, "Unknown tool: " + tool_name)};

  static const boost::json::value no_arguments = boost::json::object();
  auto const* arguments = params.if_contains("arguments");
  if (arguments && !arguments->is_object())
    return {std::nullopt, error(200, id, rpc_code::invalid_params, "Invalid params: arguments must be an object")};

  boost::json::object r;
  boost::json::array content;
  boost::json::object text;
  text["type"] = "text";

  try
  {
    boost::json::value const value = it->handler(arguments ? *arguments : no_arguments);

    // A string result is the text itself; anything else is sent as JSON
    // text. structuredContent must be a JSON object in every revision, so
    // only objects are also sent there: an array or a number would make
    // clients reject the whole result (e.g. the TypeScript SDK: "expected
    // record"). The text content always carries the full value.
    if (value.is_string())
      text["text"] = value;
    else
      text["text"] = boost::json::serialize(value);
    if (value.is_object())
      r["structuredContent"] = value;
    r["isError"] = false;
  }
  catch (rpc_error const& e)
  {
    // Tool execution error: the model sees the message and may retry.
    log_debug("mcp tool '", tool_name, "': ", e.what());
    text["text"] = std::string(e.what());
    r["isError"] = true;
  }
  catch (std::exception const& e)
  {
    log_error("mcp tool '", tool_name, "' failed: ", e.what());
    text["text"] = "Internal error";
    r["isError"] = true;
  }

  content.push_back(text);
  r["content"] = content;
  return {std::move(r), std::nullopt};
}

// Revisions 2025-03-26 to 2025-11-25: initialize, ping, tools/list and
// tools/call. JSON-RPC errors are sent with HTTP 200, as those clients
// treat any other status as a transport failure.
response_data mcp_provider::respond_legacy(const std::string&           method,
                                           const boost::json::value&    id,
                                           const boost::json::value*    params,
                                           const std::string&           version_header) const
{
  if (method == "initialize")
  {
    // Version negotiation: the requested version if supported, otherwise
    // the latest supported one (the client then decides).
    std::string requested;
    if (params && params->is_object())
      if (auto const* v = params->as_object().if_contains("protocolVersion"); v && v->is_string())
        requested.assign(v->as_string().data(), v->as_string().size());

    boost::json::object r;
    r["protocolVersion"] = is_legacy_version(requested) ? requested : std::string(mcp_legacy_versions[0]);
    boost::json::object capabilities;
    capabilities["tools"] = boost::json::object();
    r["capabilities"] = capabilities;
    boost::json::object info;
    info["name"]    = options_.name;
    info["version"] = options_.version;
    r["serverInfo"] = info;
    if (!options_.instructions.empty())
      r["instructions"] = options_.instructions;
    return ok(id, std::move(r));
  }

  // A version header that is present must name a supported revision.
  if (!version_header.empty() && !is_legacy_version(version_header))
    return error(400, id, rpc_code::invalid_request,
                 "Unsupported protocol version: " + version_header);

  if (method == "ping")
    return ok(id, boost::json::object());

  if (method == "tools/list")
  {
    boost::json::object r;
    r["tools"] = tool_array();
    return ok(id, std::move(r));
  }

  if (method == "tools/call")
  {
    if (!params || !params->is_object())
      return error(200, id, rpc_code::invalid_params, "Invalid params: name is required");
    auto const* name = params->as_object().if_contains("name");
    if (!name || !name->is_string())
      return error(200, id, rpc_code::invalid_params, "Invalid params: name is required");

    auto r = run_tool(id, params->as_object(),
                      std::string(name->as_string().data(), name->as_string().size()));
    if (!r)
      return std::move(r.error());
    return ok(id, std::move(*r));
  }

  return error(200, id, rpc_code::method_not_found, "Method not found");
}

mcp_provider* mcp(mcp_options p_options)
{
  return new mcp_provider(std::move(p_options));
}
