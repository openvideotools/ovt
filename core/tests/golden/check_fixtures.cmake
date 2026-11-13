# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WI-0.093 acceptance tests 1 and 3: gen_fixtures run twice into two empty directories writes byte-identical
# files (determinism), and those files equal the checked-in ones (the "CI job regenerates and diffs" step).
#
#   cmake -DGEN=<gen_fixtures> -DWORK=<scratch dir> -DGOLDEN=<core/tests/golden> -P check_fixtures.cmake

file(REMOVE_RECURSE "${WORK}")
foreach(run IN ITEMS a b)
  execute_process(COMMAND "${GEN}" "${WORK}/${run}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gen_fixtures failed (${rc}) for run ${run}")
  endif()
endforeach()

file(GLOB names RELATIVE "${WORK}/a" "${WORK}/a/*")
list(LENGTH names count)
if(count LESS 8)
  message(FATAL_ERROR "gen_fixtures wrote only ${count} files, expected at least 8")
endif()
foreach(name IN LISTS names)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORK}/a/${name}" "${WORK}/b/${name}"
                  RESULT_VARIABLE differs)
  if(differs)
    message(FATAL_ERROR "${name}: two gen_fixtures runs differ (not deterministic)")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORK}/a/${name}" "${GOLDEN}/${name}"
                  RESULT_VARIABLE stale)
  if(stale)
    message(FATAL_ERROR "${name}: the checked-in file differs from the regenerated one. Regenerate with "
                        "gen_fixtures ${GOLDEN} and commit the result (never edit fixtures by hand).")
  endif()
endforeach()
