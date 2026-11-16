// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Test names that broke cmake/OvtCatchAddTests.cmake: it split the Catch2 XML into a CMake list, which
// splits at every ";" in a name and stops splitting after an unbalanced "[", so such test cases (and,
// after an unbalanced "[", every later one) were never registered with ctest. check_catch_discovery.cmake
// runs the discovery script on this executable and requires every test case below to come out once, with
// a test spec that selects exactly that test case.

#include <catch2/catch_test_macros.hpp>

TEST_CASE("discovery: an unbalanced [ opens a bracket", "[discovery][fast]") {
  SUCCEED();
}

TEST_CASE("discovery: a name with ; a semicolon", "[discovery][fast]") {
  SUCCEED();
}

TEST_CASE("discovery: a lone ] closes nothing", "[discovery][fast]") {
  SUCCEED();
}

TEST_CASE("discovery: [begin, end) as in the JobSpec test", "[discovery][fast]") {
  SUCCEED();
}

TEST_CASE("discovery: all of , [ ] \\ * ; & < > \" ' at once", "[discovery][fast]") {
  SUCCEED();
}

TEST_CASE("discovery: a tag with a semicolon", "[discovery][fast][semi;colon]") {
  SUCCEED();
}

TEST_CASE("discovery: the test case after the tricky ones", "[discovery][fast]") {
  SUCCEED();
}
