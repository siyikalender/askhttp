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

#include <string>
#include <string_view>

#include <boost/json.hpp>
#include <boost/optional.hpp>

/// Finds a member of a JSON object by path: "name", or "address.city" for
/// nested objects (the same path syntax property_tree used). Returns
/// nullptr if a part of the path is missing or is not an object.
const boost::json::value*
find_field(const boost::json::value&  p_data,
           std::string_view           p_path);

/// Text form of a JSON value.
///   null           -> empty optional
///   string         -> the string
///   number         -> its decimal text ("42", "-7", "0.1")
///   true / false   -> "true" / "false" (as property_tree gave them)
///   array / object -> its JSON text
boost::optional<std::string>
to_text(const boost::json::value& p_value);

/// Returns the raw value of an optional field (no escaping; apply
/// json_escape() if needed). A missing field and JSON null both give an
/// empty optional. (With property_tree, null arrived as the text "null".)
boost::optional<std::string>
get_field(const boost::json::value&  p_data,
          std::string_view           p_path);

/// Returns the value of a field that must be present and not null.
/// Throws rpc_error with code -32602 (Invalid params) otherwise, which a
/// JSON-RPC client receives as an error response.
///
/// Like get_field(), the value is returned raw; apply json_escape() if
/// needed.
std::string
get_required_field(const boost::json::value&  p_data,
                   std::string_view           p_path);

// ASKHTTP_JSON_FIELDS_HPP
#endif
