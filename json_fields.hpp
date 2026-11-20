//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Access to the fields of a parsed JSON request (e.g. the "params" a
// JSON-RPC handler receives), by path: "name" or "address.city".
//
//   find_field(params, "id")            the JSON value, or nullptr
//   get_field(params, "note")           its text, or an empty optional
//   get_required_field(params, "text")  its text; throws rpc_error -32602
//                                       if it is missing or null
//
// Values are returned raw (not escaped); json_escape() escapes them for
// JSON text.
//

#ifndef ASKHTTP_JSON_FIELDS_HPP
#define ASKHTTP_JSON_FIELDS_HPP

#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/json.hpp>
#include <boost/optional.hpp>

#include "json_writer.hpp"
#include "json_rpc.hpp"

/// Finds a member of a JSON object by path: "name", or "address.city" for
/// nested objects (the same path syntax property_tree used). Returns
/// nullptr if a part of the path is missing or is not an object.
inline const boost::json::value*
find_field(const boost::json::value&  p_data,
           std::string_view           p_path)
{
  const boost::json::value* current = &p_data;

  while (true)
  {
    auto const dot  = p_path.find('.');
    auto const key  = p_path.substr(0, dot);
    auto const* obj = current->if_object();

    if (!obj)
      return nullptr;

    current = obj->if_contains(key);

    if (!current || dot == std::string_view::npos)
      return current;

    p_path.remove_prefix(dot + 1);
  }
}

/// Text form of a JSON value.
///   null           -> empty optional
///   string         -> the string
///   number         -> its decimal text ("42", "-7", "0.1")
///   true / false   -> "true" / "false" (as property_tree gave them)
///   array / object -> its JSON text
inline boost::optional<std::string>
to_text(const boost::json::value& p_value)
{
  if (p_value.is_null())
    return boost::optional<std::string>();

  if (p_value.is_string())
  {
    auto const& s = p_value.as_string();
    return std::string(s.data(), s.size());
  }

  if (p_value.is_int64())
    return std::to_string(p_value.as_int64());

  if (p_value.is_uint64())
    return std::to_string(p_value.as_uint64());

  if (p_value.is_double())
  {
    std::string text;
    text << p_value.as_double();   // shortest round-trip form (json_writer.hpp)
    return text;
  }

  if (p_value.is_bool())
    return std::string(p_value.as_bool() ? "true" : "false");

  return boost::json::serialize(p_value);
}

/// Returns the raw value of an optional field (no escaping; apply
/// json_escape() if needed). A missing field and JSON null both give an
/// empty optional. (With property_tree, null arrived as the text "null".)
inline boost::optional<std::string>
get_field(const boost::json::value&  p_data,
          std::string_view           p_path)
{
  auto const* v = find_field(p_data, p_path);

  if (!v)
    return boost::optional<std::string>();

  return to_text(*v);
}

/// Returns the value of a field that must be present and not null.
/// Throws rpc_error with code -32602 (Invalid params) otherwise, which a
/// JSON-RPC client receives as an error response.
///
/// Like get_field(), the value is returned raw; apply json_escape() if
/// needed.
inline std::string
get_required_field(const boost::json::value&  p_data,
                   std::string_view           p_path)
{
  auto result = get_field(p_data, p_path);

  if (!result)
  {
    throw rpc_error(rpc_code::invalid_params, "missing required field: " + std::string(p_path));
  }

  return *result;
}

// ASKHTTP_JSON_FIELDS_HPP
#endif
