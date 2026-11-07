# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Golden tests of the oracle families (chapter 14 §14.4.0, WI-0.556).
#
#   ovt_add_golden_test(NAME <name> AREA <dir> ID <id> ORACLE <tool> FAMILY <family>
#                       [CORPUS <manifest-id>] [CORPUS_TAG <tag> [CORPUS_MANIFEST <file>]] [GOLDEN <file>] [TOLERANCES <file>] [SLOW] [UPDATABLE] [LABELS <label>...]
#                       CMD <program> [args...])
#
#   Registers the CTest test `golden_<name>`. The test runs ovt_golden_run, which runs CMD with stdout redirected to a
#   scratch file that is unique per NAME (so `ctest -L golden -j8` never collides), checks that the output is a
#   canonical document of FAMILY (syntax-elements, header-struct, block-grid, box-tree, sample-table, scores) and
#   compares it with the golden file tests/golden/<AREA>/<ID>.<ORACLE>.json (or GOLDEN) by the tolerant semantics of
#   core/tests/test_support/golden_compare.h: the golden is the oracle and a lower bound of what OVT reports.
#   A mismatch prints the differing JSON paths and a unified diff. OVT_UPDATE_GOLDEN=1 rewrites the golden with OVT's
#   own output, but only for a test registered UPDATABLE (an OVT-side golden) and never when CI is set; for any other
#   test the run is refused, because an oracle golden is the lower bound OVT is measured against.
#
#   CORPUS names a manifest entry (tests/corpora/manifest.json): the argument "@CORPUS@" in CMD is replaced by the
#   verified corpus file, and the test is skipped (exit 77) when it has not been fetched, unless OVT_REQUIRE_CORPORA=1.
#   Labels: golden, the speed label fast (SLOW: slow), needs_corpus when CORPUS is given, plus LABELS.
#   CORPUS_TAG names a corpus subset (for example `fast`, WI-0.040): when it is not fully in $OVT_CORPORA_DIR the test
#   is skipped with `corpus subset '<tag>' missing: run tools/fetch_corpora.py --tags <tag>`, or fails under
#   OVT_REQUIRE_CORPORA=1 (cmake/OvtCorpora.cmake). CORPUS_MANIFEST replaces tests/corpora/manifest.json (tests only).
#   TOLERANCES is a JSON object {"frames[].psnr_y": 0.001, "default": 0} (see CompareOptions).

set(OVT_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Directory of the golden files (tests/golden)")

add_executable(ovt_golden_run golden_run_main.cpp)
target_link_libraries(ovt_golden_run PRIVATE ovt_test_support ovt::flags ${nlohmann_json_TARGET})
ovt_set_license(ovt_golden_run "LGPL-2.1-or-later")

function(ovt_add_golden_test)
  cmake_parse_arguments(PARSE_ARGV 0 arg "SLOW;UPDATABLE"
                        "NAME;AREA;ID;ORACLE;FAMILY;CORPUS;CORPUS_TAG;CORPUS_MANIFEST;GOLDEN;TOLERANCES" "LABELS;CMD")
  foreach(required NAME FAMILY CMD)
    if(NOT arg_${required})
      message(FATAL_ERROR "ovt_add_golden_test: ${required} is required")
    endif()
  endforeach()
  if(NOT arg_GOLDEN)
    if(NOT arg_AREA OR NOT arg_ORACLE)
      message(FATAL_ERROR "ovt_add_golden_test(${arg_NAME}): give GOLDEN, or AREA and ORACLE (and ID or CORPUS)")
    endif()
    if(NOT arg_ID)
      set(arg_ID "${arg_CORPUS}")
    endif()
    if(NOT arg_ID)
      message(FATAL_ERROR "ovt_add_golden_test(${arg_NAME}): ID or CORPUS is required to name the golden file")
    endif()
    set(arg_GOLDEN "${OVT_GOLDEN_DIR}/${arg_AREA}/${arg_ID}.${arg_ORACLE}.json")
  endif()

  set(options --golden "${arg_GOLDEN}" --family "${arg_FAMILY}" --out
              "${CMAKE_CURRENT_BINARY_DIR}/golden_out/${arg_NAME}.json")
  if(arg_UPDATABLE)
    list(APPEND options --updatable)
  endif()
  if(arg_TOLERANCES)
    list(APPEND options --tolerances "${arg_TOLERANCES}")
  endif()
  if(arg_CORPUS)
    list(APPEND options --corpus "${arg_CORPUS}")
  endif()
  if(arg_CORPUS_TAG)
    if(arg_CORPUS_MANIFEST)
      ovt_corpora_index(corpus_index MANIFEST "${arg_CORPUS_MANIFEST}")
    else()
      ovt_corpora_index(corpus_index)
    endif()
    ovt_corpus_gate_command(
      gated
      TAG
      "${arg_CORPUS_TAG}"
      INDEX
      "${corpus_index}"
      COMMAND
      ovt_golden_run
      ${options}
      --
      ${arg_CMD})
    add_test(NAME golden_${arg_NAME} COMMAND ${gated})
    ovt_corpus_skip_properties(golden_${arg_NAME})
  else()
    add_test(NAME golden_${arg_NAME} COMMAND ovt_golden_run ${options} -- ${arg_CMD})
  endif()

  set(labels golden)
  if(arg_SLOW)
    list(APPEND labels slow)
  else()
    list(APPEND labels fast)
  endif()
  if(arg_CORPUS OR arg_CORPUS_TAG)
    list(APPEND labels needs_corpus)
  endif()
  list(APPEND labels ${arg_LABELS})
  set_tests_properties(golden_${arg_NAME} PROPERTIES LABELS "${labels}" SKIP_RETURN_CODE 77)
endfunction()

# ---- First golden test (WI-0.556 step 4): box-tree of a committed 2 KiB MP4 ----------------------------------
# No container parser exists yet; ovt_golden_box_tree walks the boxes itself and goes through the real
# FlatBuffer -> to_canonical() -> compare chain. The golden comes from an independent Python walker
# (isobmff/gen_min_mp4.py). Not part of the unit-test lane's corpus: it needs no download.
add_executable(ovt_golden_box_tree isobmff/box_tree_main.cpp)
target_link_libraries(ovt_golden_box_tree PRIVATE ovt_test_support ovt_schemas ovt::flags ${flatbuffers_TARGET})
ovt_set_license(ovt_golden_box_tree "LGPL-2.1-or-later")

ovt_add_golden_test(
  NAME
  isobmff_min_structural
  AREA
  isobmff
  ID
  ovt_min_structural
  ORACLE
  pybox
  FAMILY
  box-tree
  CMD
  $<TARGET_FILE:ovt_golden_box_tree>
  "${CMAKE_CURRENT_SOURCE_DIR}/isobmff/ovt_min_structural.mp4")

# The committed fixture and its golden are exactly what the generator writes.
if(Python3_Interpreter_FOUND)
  add_test(NAME golden_isobmff_fixture_up_to_date COMMAND "${Python3_EXECUTABLE}" -I
                                                          "${CMAKE_CURRENT_SOURCE_DIR}/isobmff/gen_min_mp4.py" --check)
  set_tests_properties(golden_isobmff_fixture_up_to_date PROPERTIES LABELS "fast;golden;tools")
endif()

# OVT_UPDATE_GOLDEN=1 must not rewrite an oracle golden: the run is refused (before anything is written).
add_test(
  NAME golden_update_refused_for_oracle_golden
  COMMAND
    ovt_golden_run --golden "${CMAKE_CURRENT_SOURCE_DIR}/isobmff/ovt_min_structural.pybox.json" --family box-tree --out
    "${CMAKE_CURRENT_BINARY_DIR}/golden_out/update_refused.json" -- $<TARGET_FILE:ovt_golden_box_tree>
    "${CMAKE_CURRENT_SOURCE_DIR}/isobmff/ovt_min_structural.mp4")
set_tests_properties(
  golden_update_refused_for_oracle_golden
  PROPERTIES ENVIRONMENT "OVT_UPDATE_GOLDEN=1" PASS_REGULAR_EXPRESSION
             "OVT_UPDATE_GOLDEN=1 refused: .* is an oracle golden" LABELS "fast;golden")

# OVT_UPDATE_GOLDEN=1 is also refused under CI for an UPDATABLE golden (the oracle-golden check does not reach this
# path). The test works on a build-tree copy of the golden, so the source file cannot be touched even if the refusal
# regresses.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/isobmff/ovt_min_structural.pybox.json"
               "${CMAKE_CURRENT_BINARY_DIR}/golden_out/update_refused_ci.golden.json" COPYONLY)
add_test(
  NAME golden_update_refused_under_ci
  COMMAND
    ovt_golden_run --golden "${CMAKE_CURRENT_BINARY_DIR}/golden_out/update_refused_ci.golden.json" --family box-tree
    --out "${CMAKE_CURRENT_BINARY_DIR}/golden_out/update_refused_ci.json" --updatable --
    $<TARGET_FILE:ovt_golden_box_tree> "${CMAKE_CURRENT_SOURCE_DIR}/isobmff/ovt_min_structural.mp4")
set_tests_properties(
  golden_update_refused_under_ci PROPERTIES ENVIRONMENT "OVT_UPDATE_GOLDEN=1;CI=1" PASS_REGULAR_EXPRESSION
                                            "refused: CI is set" LABELS "fast;golden")

# ---- Corpus gate (WI-0.040): a missing corpus subset skips, OVT_REQUIRE_CORPORA=1 fails -------------------------
# The fixture manifest has two fetchable entries tagged `fast` and entries that are not fetchable; the cache
# directories are made empty at configure time.
set(_gate_dir "${CMAKE_CURRENT_BINARY_DIR}/corpus_gate")
set(_gate_manifest "${CMAKE_CURRENT_SOURCE_DIR}/corpus_gate/manifest.json")
file(REMOVE_RECURSE "${_gate_dir}")
file(MAKE_DIRECTORY "${_gate_dir}/empty" "${_gate_dir}/complete/gate_a" "${_gate_dir}/complete/gate_b"
     "${_gate_dir}/partial/gate_a")
file(WRITE "${_gate_dir}/complete/gate_a/.ok" "")
file(WRITE "${_gate_dir}/complete/gate_b/.ok" "")
file(WRITE "${_gate_dir}/partial/gate_a/.ok" "")
ovt_corpora_index(_gate_index MANIFEST "${_gate_manifest}")

# Acceptance 2: with nothing fetched the registered golden test is skipped.
ovt_add_golden_test(
  NAME
  corpus_gate_skips
  AREA
  isobmff
  ID
  ovt_min_structural
  ORACLE
  pybox
  FAMILY
  box-tree
  CORPUS_TAG
  fast
  CORPUS_MANIFEST
  "${_gate_manifest}"
  CMD
  $<TARGET_FILE:ovt_golden_box_tree>
  "${CMAKE_CURRENT_SOURCE_DIR}/isobmff/ovt_min_structural.mp4")
set_tests_properties(golden_corpus_gate_skips PROPERTIES ENVIRONMENT
                                                         "OVT_CORPORA_DIR=${_gate_dir}/empty;OVT_REQUIRE_CORPORA=0")

# The same, checked by message and exit code (a skipped test cannot assert its own message).
function(_ovt_gate_test name dir require)
  add_test(NAME corpus_gate_${name} COMMAND ${CMAKE_COMMAND} "-DOVT_CORPUS_INDEX=${_gate_index}" -DOVT_CORPUS_TAG=fast
                                            -P "${PROJECT_SOURCE_DIR}/cmake/OvtCorpusGate.cmake" -- ${ARGN})
  set_tests_properties(
    corpus_gate_${name} PROPERTIES ENVIRONMENT "OVT_CORPORA_DIR=${dir};OVT_REQUIRE_CORPORA=${require}" LABELS
                                   "fast;golden;corpus_gate")
endfunction()
_ovt_gate_test(message_when_empty "${_gate_dir}/empty" 0)
_ovt_gate_test(message_when_partial "${_gate_dir}/partial" 0)
set_tests_properties(
  corpus_gate_message_when_empty corpus_gate_message_when_partial
  PROPERTIES PASS_REGULAR_EXPRESSION "corpus subset 'fast' missing: run tools/fetch_corpora.py --tags fast")
_ovt_gate_test(fails_when_required "${_gate_dir}/empty" 1)
set_tests_properties(corpus_gate_fails_when_required PROPERTIES WILL_FAIL TRUE)
_ovt_gate_test(runs_command_when_complete "${_gate_dir}/complete" 1 ${CMAKE_COMMAND} -E echo gate-ran-the-command)
set_tests_properties(corpus_gate_runs_command_when_complete PROPERTIES PASS_REGULAR_EXPRESSION "gate-ran-the-command")
# A failing wrapped command must fail the gate (a gate that never runs its command would pass vacuously).
_ovt_gate_test(propagates_command_failure "${_gate_dir}/complete" 1 ${CMAKE_COMMAND} -E cat "${_gate_dir}/no_such_file")
set_tests_properties(corpus_gate_propagates_command_failure PROPERTIES WILL_FAIL TRUE)
