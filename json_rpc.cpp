//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of rpc_error and the JSON-RPC response builders (see
// json_rpc.hpp).
//

#include <utility>

#include "json_rpc.hpp"

rpc_error::rpc_error(std::string         p_message,
                     int                 p_code,
                     boost::json::value  p_data)
: std::runtime_error(std::move(p_message))
, code_(p_code)
, data_(std::move(p_data))
{}

rpc_error::rpc_error(int p_code, std::string p_message, boost::json::value p_data)
: rpc_error(std::move(p_message), p_code, std::move(p_data))
{}

int rpc_error::code() const
{
  return code_;
}

const boost::json::value& rpc_error::data() const
{
  return data_;
}

namespace detail
{

boost::json::object rpc_result(const boost::json::value& p_id, boost::json::value p_result)
{
  boost::json::object o;
  o["jsonrpc"] = "2.0";
  o["id"]      = p_id;
  o["result"]  = std::move(p_result);
  return o;
}

boost::json::object rpc_error_response(const boost::json::value&  p_id,
                                       int                        p_code,
                                       const std::string&         p_message,
                                       const boost::json::value&  p_data)
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
