//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Minimal MCP server built on libaskhttp: an in-memory note store.
//
// The same three handlers are offered twice:
//   POST /api   JSON-RPC 2.0, for a web frontend
//   POST /mcp   Model Context Protocol (2026-07-28; 2025-03-26 to 2025-11-25
//               for clients on older SDKs), for AI clients
//
// Build and run (see README.md):
//   cmake --preset debug && cmake --build --preset debug --target mcp_example
//   MCP_ALLOWED_ORIGINS=http://127.0.0.1:8080 MCP_TOKEN=change-me ./build/debug/mcp_example 39280
//
// Stop with Ctrl+C or SIGTERM: requests in progress are finished first.
//

#include <csignal>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "server.hpp"
#include "mcp.hpp"
#include "json_fields.hpp"

namespace
{

// stop() only sets an atomic flag, so it may be called from a signal handler.
server_control control;

void on_signal(int)
{
  control.stop();
}

// Parses a JSON literal written in this file (a JSON Schema). Throws at
// start-up if it is malformed.
boost::json::value json(const char* text)
{
  return boost::json::parse(text);
}

// Notes kept in memory. Handlers run on several server threads at once,
// so every access is serialized by the mutex.
class note_store
{
public:
  // params: {"text": "..."}  ->  {"id": 1}
  boost::json::value add(const boost::json::value& params)
  {
    std::string text = get_required_field(params, "text");   // missing: -32602
    if (text.empty())
      throw rpc_error("text must not be empty");          // tool error for MCP

    std::lock_guard<std::mutex> lock(mutex_);
    long long const id = ++last_id_;
    notes_[id] = std::move(text);

    boost::json::object result;
    result["id"] = id;
    return result;
  }

  // params: none  ->  [{"id": 1, "text": "..."}, ...]
  boost::json::value list(const boost::json::value&)
  {
    std::lock_guard<std::mutex> lock(mutex_);

    boost::json::array result;
    for (auto const& [id, text] : notes_)
    {
      boost::json::object note;
      note["id"]   = id;
      note["text"] = text;
      result.push_back(note);
    }
    return result;
  }

  // params: {"id": 1}  ->  {"deleted": 1} (0: no such note)
  boost::json::value remove(const boost::json::value& params)
  {
    // Typed access: the id must be a JSON integer, not text.
    auto const* id = find_field(params, "id");
    if (!id || !id->is_int64())
      throw rpc_error(rpc_code::invalid_params, "id must be an integer");

    std::lock_guard<std::mutex> lock(mutex_);

    boost::json::object result;
    result["deleted"] = static_cast<long long>(notes_.erase(id->as_int64()));
    return result;
  }

private:
  std::mutex                     mutex_;
  long long                      last_id_ = 0;
  std::map<long long, std::string> notes_;
};

} // namespace

int main(int argc, char** argv)
{
  unsigned short const port = static_cast<unsigned short>(argc > 1 ? std::atoi(argv[1]) : 8080);

  note_store notes;
  auto responses = std::make_shared<response_map>();

  // ---- JSON-RPC for the frontend -------------------------------------------
  auto* rpc = json_rpc();
  rpc->add("add_note",    [&](const boost::json::value& p) { return notes.add(p); });
  rpc->add("list_notes",  [&](const boost::json::value& p) { return notes.list(p); });
  rpc->add("delete_note", [&](const boost::json::value& p) { return notes.remove(p); });
  add(*responses, "/api", rpc);

  // ---- the same handlers as MCP tools --------------------------------------
  // Browser-based clients (e.g. the llama.cpp web UI) send an Origin header;
  // their origin must be listed in MCP_ALLOWED_ORIGINS, comma-separated:
  //   MCP_ALLOWED_ORIGINS=http://127.0.0.1:8080
  std::vector<std::string> origins;
  if (const char* list = std::getenv("MCP_ALLOWED_ORIGINS"))
  {
    std::string_view rest = list;
    while (!rest.empty())
    {
      auto const comma = rest.find(',');
      auto item = rest.substr(0, comma);
      while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
      while (!item.empty() && (item.back() == ' ' || item.back() == '/')) item.remove_suffix(1);
      if (!item.empty())
        origins.emplace_back(item);
      rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
  }

  auto* m = mcp({.name            = "notes-example",
                 .version         = "1.0.0",
                 .instructions    = "A small note store. Call list_notes before "
                                    "delete_note to find the id of a note.",
                 .allowed_origins = origins});

  m->add_tool({.name         = "add_note",
               .title        = "Add note",
               .description  = "Stores a note and returns its id.",
               .input_schema = json(R"({"type":"object",
                                        "properties":{"text":{"type":"string","description":"Note text"}},
                                        "required":["text"],
                                        "additionalProperties":false})")},
              rpc->handler("add_note"));

  m->add_tool({.name         = "list_notes",
               .title        = "List notes",
               .description  = "Returns all notes with their ids.",
               .input_schema = json(R"({"type":"object","additionalProperties":false})"),
               .read_only    = true},
              rpc->handler("list_notes"));

  m->add_tool({.name         = "delete_note",
               .title        = "Delete note",
               .description  = "Deletes the note with the given id. Cannot be undone.",
               .input_schema = json(R"({"type":"object",
                                        "properties":{"id":{"type":"integer","description":"Note id"}},
                                        "required":["id"],
                                        "additionalProperties":false})"),
               .destructive  = true,
               .idempotent   = true},
              rpc->handler("delete_note"));

  add(*responses, "/mcp", m);

  // ---- authentication: a bearer token from the environment ------------------
  authorizer auth;
  if (const char* token = std::getenv("MCP_TOKEN"); token && *token)
  {
    auth = [expected = std::string(token)](const request_info& r)
    {
      auto const t = r.bearer_token();
      return t && constant_time_equals(*t, expected) ? auth_result::allow
                                                     : auth_result::unauthorized;
    };
  }
  else
  {
    log_warning("MCP_TOKEN is not set: requests are NOT authenticated");
  }

  std::signal(SIGINT,  on_signal);
  std::signal(SIGTERM, on_signal);

  // Bound to 127.0.0.1 only, as the MCP specification recommends for local
  // servers. Use another address to reach it from the local network.
  log_info("listening on http://127.0.0.1:", port, "  (/api: JSON-RPC, /mcp: MCP)");

  try
  {
    start_server(tcp::endpoint(net::ip::make_address("127.0.0.1"), port),
                 2, responses, &control, auth);
  }
  catch (std::exception const& e)
  {
    log_error("server: ", e.what());   // e.g. "bind: Address already in use"
    return 1;
  }

  log_info("stopped");
  return 0;
}
