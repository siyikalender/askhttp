//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Test runner. The Boost.Test implementation is compiled here (single-header
// variant), so no Boost.Test library has to be linked. The other test files
// include <boost/test/unit_test.hpp> only.
//
//   ./askhttp_tests                          all tests
//   ./askhttp_tests --run_test=uri           one suite
//   ./askhttp_tests --run_test=json/get_*    matching cases
//   ./askhttp_tests --list_content           list suites and cases
//

#define BOOST_TEST_MODULE askhttp_tests
#include <boost/test/included/unit_test.hpp>
