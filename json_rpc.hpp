//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// JSON-RPC 2.0 building blocks (https://www.jsonrpc.org/specification).
//
// A method handler receives the "params" of the request and returns the
// "result". To report a failure it throws rpc_error; the client then
// receives an "error" object with that code and message:
//
//   [](const boost::json::value& params) -> boost::json::value
//   {
//     if (...) throw rpc_error(1001, "record not found");
//     return {{"deleted", 1}};
//   }
//
// Any other exception becomes -32603 "Internal error"; its text is logged
// but not sent to the client.
//

#ifndef ASKHTTP_JSON_RPC_HPP
#define ASKHTTP_JSON_RPC_HPP

#include <functional>
#include <stdexcept>
#include <string>

#include <boost/json.hpp>

namespace rpc_code
{
  // Defined by JSON-RPC 2.0.
  constexpr int parse_error      = -32700;
  constexpr int invalid_request  = -32600;
  constexpr int method_not_found = -32601;
  constexpr int invalid_params   = -32602;
  constexpr int internal_error   = -32603;

  // Application errors use codes outside -32768 .. -32000. This is the
  // default code of rpc_error.
  constexpr int application_error = 1;
}

class rpc_error : public std::runtime_error
{
public:
  explicit rpc_error(std::string         p_message,
                     int                 p_code = rpc_code::application_error,
                     boost::json::value  p_data = nullptr)
  : std::runtime_error(std::move(p_message))
  , code_(p_code)
  , data_(std::move(p_data))
  {}

  rpc_error(int p_code, std::string p_message, boost::json::value p_data = nullptr)
  : rpc_error(std::move(p_message), p_code, std::move(p_data))
  {}

  int                        code() const { return code_; }
  const boost::json::value&  data() const { return data_; }

private:
  int                 code_;
  boost::json::value  data_;
};

// Receives "params" (JSON null if absent), returns "result".
using rpc_handler = std::function<boost::json::value(const boost::json::value& params)>;

namespace detail
{

// {"jsonrpc":"2.0","id":<id>,"result":<result>}
inline boost::json::object rpc_result(const boost::json::value& p_id, boost::json::value p_result)
{
  boost::json::object o;
  o["jsonrpc"] = "2.0";
  o["id"]      = p_id;
  o["result"]  = std::move(p_result);
  return o;
}

// {"jsonrpc":"2.0","id":<id>,"error":{"code":..,"message":..[,"data":..]}}
// An id of JSON null is written as null (JSON-RPC: id unknown).
inline boost::json::object rpc_error_response(const boost::json::value&  p_id,
                                              int                        p_code,
                                              const std::string&         p_message,
                                              const boost::json::value&  p_data = nullptr)
{
  boost::json::object e;
  e["code"]    = p_code;
  e["message"] = p_message;
  if (!p_data.is_null())
    e["data"] = p_data;

  boost::json::object o;
  o["jsonrpc"] = "2.0";
  o["id"]      = p_id;
  o["error"]   = std::move(e);
  return o;
}

} // namespace detail

// ASKHTTP_JSON_RPC_HPP
#endif
