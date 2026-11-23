//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Implementation of the JSON field accessors (see json_fields.hpp).
//

#include "json_fields.hpp"
#include "json_rpc.hpp"
#include "json_writer.hpp"

const boost::json::value*
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

boost::optional<std::string>
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

boost::optional<std::string>
get_field(const boost::json::value&  p_data,
          std::string_view           p_path)
{
  auto const* v = find_field(p_data, p_path);

  if (!v)
    return boost::optional<std::string>();

  return to_text(*v);
}

std::string
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
