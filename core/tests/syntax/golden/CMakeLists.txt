# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Header-parser golden suite (chapter 14 §14.4.1, §14.4.2, WI-1.572): one CTest per committed oracle golden
# `tests/golden/<codec>/<id>.<oracle>.json.gz` (oracle trace_headers or gstparse), named
# `syntax_golden_<codec>_<oracle>_<id>` (`ctest -R syntax_golden`). The goldens and the number of pictures each covers come from
# tests/golden/h26x-index.json (WI-0.567). A test whose corpus file has not been fetched is skipped (exit 77), and
# fails under OVT_REQUIRE_CORPORA=1.

add_library(ovt_syntax_golden STATIC syntax_golden.cpp h264_derived.cpp)
target_link_libraries(ovt_syntax_golden PUBLIC ovt_core_objects ovt_test_support ovt::flags ${nlohmann_json_TARGET})
ovt_set_license(ovt_syntax_golden "LGPL-2.1-or-later")

add_executable(ovt_syntax_golden_run syntax_golden_main.cpp)
target_link_libraries(ovt_syntax_golden_run PRIVATE ovt_syntax_golden)
ovt_set_license(ovt_syntax_golden_run "LGPL-2.1-or-later")

set(_sg_golden_dir "${PROJECT_SOURCE_DIR}/tests/golden")
set(_sg_normalize "${PROJECT_SOURCE_DIR}/tests/oracles/normalize")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sg_golden_dir}/h26x-index.json")

# Manifest tags per corpus id (`<id>|<tag>,...`); an id the index does not list (a generated syn_* stream) counts as
# fast: it is skipped when it is absent, like every other corpus.
ovt_corpora_index(_sg_corpora_index)
file(STRINGS "${_sg_corpora_index}" _sg_index_lines)

# ovt_add_syntax_golden_tests(<codec> <oracle> <index.json>): one test per <oracle> golden of <codec> in the index.
function(ovt_add_syntax_golden_tests codec oracle index_file)
  file(READ "${index_file}" text)
  string(JSON entries GET "${text}" entries)
  string(JSON count LENGTH "${entries}")
  math(EXPR last "${count} - 1")
  foreach(i RANGE ${last})
    string(JSON id MEMBER "${entries}" ${i})
    string(
      JSON
      file
      ERROR_VARIABLE
      no_golden
      GET
      "${entries}"
      "${id}"
      goldens
      ${oracle}
      file)
    if(no_golden OR NOT file MATCHES "^${codec}/")
      continue()
    endif()
    string(JSON pictures GET "${entries}" "${id}" goldens ${oracle} pictures)
    set(labels "golden;needs_corpus;syntax_golden")
    set(line ${_sg_index_lines})
    list(FILTER line INCLUDE REGEX "^${id}\\|")
    if(line AND NOT line MATCHES "[|,]fast(,|$)")
      list(APPEND labels slow)
    else()
      list(APPEND labels fast)
    endif()
    set(name syntax_golden_${codec}_${oracle}_${id})
    add_test(
      NAME ${name}
      COMMAND
        ovt_syntax_golden_run --codec ${codec} --oracle ${oracle} --golden "${_sg_golden_dir}/${file}" --names
        "${_sg_normalize}/cbs_name_map.json" --ignore "${_sg_normalize}/trace_headers_ignore.txt" --map
        "${CMAKE_CURRENT_SOURCE_DIR}/gst_map.json" --pictures ${pictures} --corpus ${id})
    set_tests_properties(${name} PROPERTIES LABELS "${labels}" SKIP_RETURN_CODE 77)
  endforeach()
endfunction()

foreach(oracle trace_headers gstparse)
  ovt_add_syntax_golden_tests(h264 ${oracle} "${_sg_golden_dir}/h26x-index.json")
endforeach()
