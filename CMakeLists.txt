#
# Author: Claude (Anthropic)
#
# SPDX-License-Identifier: Unlicense
#
# CMake build for libaskhttp, its tests and the examples.
#   cmake --preset debug && cmake --build --preset debug && ctest --preset debug
#
cmake_minimum_required(VERSION 3.21)
project(libaskhttp LANGUAGES CXX)

get_property(_askhttp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _askhttp_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ASKHTTP_BUILD_TESTS "Build the Boost.Test runner" ${PROJECT_IS_TOP_LEVEL})
option(ASKHTTP_BUILD_EXAMPLES "Build the examples (examples/)" ${PROJECT_IS_TOP_LEVEL})
option(ASKHTTP_COMPILE_BOOST_JSON "Compile Boost.JSON into the library (json_impl.cpp)" ON)
set(ASKHTTP_HALUJ_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../framework/haluj/include"
  CACHE PATH "haluj include directory")
set(ASKHTTP_EXTRA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
  CACHE STRING "Additional include directories")
set(ASKHTTP_EXTRA_LIBRARY_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../lib"
  CACHE STRING "Additional library directories (tests, example)")

if(NOT EXISTS "${ASKHTTP_HALUJ_INCLUDE_DIR}/haluj/pair_lookup.hpp")
  message(FATAL_ERROR
    "haluj not found in '${ASKHTTP_HALUJ_INCLUDE_DIR}': set ASKHTTP_HALUJ_INCLUDE_DIR")
endif()

set(_askhttp_extra_include_dirs "")
foreach(_dir IN LISTS ASKHTTP_EXTRA_INCLUDE_DIRS)
  if(IS_DIRECTORY "${_dir}")
    list(APPEND _askhttp_extra_include_dirs "${_dir}")
  endif()
endforeach()

set(_askhttp_extra_library_dirs "")
foreach(_dir IN LISTS ASKHTTP_EXTRA_LIBRARY_DIRS)
  if(IS_DIRECTORY "${_dir}")
    list(APPEND _askhttp_extra_library_dirs "${_dir}")
  endif()
endforeach()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost 1.75 CONFIG QUIET)
if(NOT TARGET Boost::headers)
  find_path(ASKHTTP_BOOST_INCLUDE_DIR boost/version.hpp HINTS ${_askhttp_extra_include_dirs})
  if(NOT ASKHTTP_BOOST_INCLUDE_DIR)
    message(FATAL_ERROR
      "Boost >= 1.75 not found: set CMAKE_PREFIX_PATH or ASKHTTP_EXTRA_INCLUDE_DIRS")
  endif()
  add_library(Boost::headers INTERFACE IMPORTED)
  target_include_directories(Boost::headers INTERFACE "${ASKHTTP_BOOST_INCLUDE_DIR}")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)

# -Wall on every target, DEBUG in debug builds.
function(askhttp_common_options tgt)
  target_compile_options(${tgt} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall>)
  target_compile_definitions(${tgt} PRIVATE $<$<CONFIG:Debug>:DEBUG>)
endfunction()

add_library(askhttp STATIC
  json_escape.cpp
  parse_uri.cpp
  file_format.hpp
  json_escape.hpp
  json_fields.hpp
  json_rpc.hpp
  json_writer.hpp
  log.hpp
  mcp.hpp
  mime_type.hpp
  parse_uri.hpp
  provider.hpp
  query.hpp
  read_stream.hpp
  server.hpp
  string_view.hpp
)
if(ASKHTTP_COMPILE_BOOST_JSON)
  target_sources(askhttp PRIVATE json_impl.cpp)
endif()
add_library(askhttp::askhttp ALIAS askhttp)
target_compile_features(askhttp PUBLIC cxx_std_20)
target_include_directories(askhttp PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${ASKHTTP_HALUJ_INCLUDE_DIR}"
  ${_askhttp_extra_include_dirs}
)
target_link_libraries(askhttp PUBLIC
  Boost::headers
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
)
askhttp_common_options(askhttp)

if(ASKHTTP_BUILD_TESTS)
  enable_testing()
  add_executable(askhttp_tests
    tests/main.cpp
    tests/test_file_upload.cpp
    tests/test_json.cpp
    tests/test_logging.cpp
    tests/test_mcp_endpoint.cpp
    tests/test_request_handling.cpp
    tests/test_server.cpp
    tests/test_uri.cpp
  )
  target_link_libraries(askhttp_tests PRIVATE askhttp::askhttp)
  target_link_directories(askhttp_tests PRIVATE ${_askhttp_extra_library_dirs})
  askhttp_common_options(askhttp_tests)
  # One CTest entry: the end-to-end suites share ASKHTTP_TEST_PORT, so they
  # must not run in parallel under "ctest -j".
  add_test(NAME askhttp_tests COMMAND askhttp_tests)
endif()

if(ASKHTTP_BUILD_EXAMPLES)
  add_executable(mcp_example examples/mcp_server/main.cpp)
  target_link_libraries(mcp_example PRIVATE askhttp::askhttp)
  target_link_directories(mcp_example PRIVATE ${_askhttp_extra_library_dirs})
  askhttp_common_options(mcp_example)

  add_executable(static_example examples/static_site/main.cpp)
  target_link_libraries(static_example PRIVATE askhttp::askhttp)
  target_link_directories(static_example PRIVATE ${_askhttp_extra_library_dirs})
  target_compile_definitions(static_example PRIVATE
    STATIC_EXAMPLE_WWW_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/static_site/www")
  askhttp_common_options(static_example)
endif()
