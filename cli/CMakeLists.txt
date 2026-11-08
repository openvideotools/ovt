# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ovt command-line tool (chapter 12 §12.1, WI-1.500).
#   ovt_cli_lib          the front-end: CLI11 tree, global options, exit codes, output writers
#   ovt_gen_completions  build-time generator: walks the CLI11 tree and writes ovt.{bash,zsh,fish} plus
#                        completions_data.cpp into ${OVT_GENERATED_DIR}/completions (chapter 13 §13.2.3)
#   ovt_cli              the executable (output name ${OVT_CLI_NAME}); embeds the generated scripts

ovt_find_package(CLI11 SPDX "BSD-3-Clause" TARGETS CLI11::CLI11 HEADER_ONLY)
# Imported targets are directory-scoped, so the core's dependencies are looked up again here.
ovt_find_package(fmt SPDX "MIT" TARGETS fmt::fmt)
ovt_find_package(flatbuffers SPDX "Apache-2.0" TARGETS flatbuffers::flatbuffers NAMES FlatBuffers)
ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)

add_library(ovt_cli_lib STATIC src/app.cpp src/output/json_writer.cpp src/output/fb_writer.cpp
                               src/output/csv_writer.cpp src/output/text_writer.cpp)
target_include_directories(ovt_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${OVT_GENERATED_DIR}")
target_link_libraries(ovt_cli_lib PUBLIC ovt::core ${CLI11_TARGET} ${fmt_TARGET} ${flatbuffers_TARGET}
                      PRIVATE ovt::flags ovt_schemas ${nlohmann_json_TARGET})
target_compile_definitions(ovt_cli_lib PUBLIC "OVT_CLI_NAME_VALUE=\"${OVT_CLI_NAME}\""
                           PRIVATE "OVT_CLI_VERSION_VALUE=\"${PROJECT_VERSION}${OVT_VERSION_SUFFIX}\"")
add_dependencies(ovt_cli_lib ovt_version_header)
ovt_set_license(ovt_cli_lib "GPL-3.0-or-later")

# ---- Completion scripts (§12.2.15) ------------------------------------------------------------------------
set(_ovt_completions_dir "${OVT_GENERATED_DIR}/completions")
set(_ovt_completions_outputs
    "${_ovt_completions_dir}/${OVT_CLI_NAME}.bash" "${_ovt_completions_dir}/${OVT_CLI_NAME}.zsh"
    "${_ovt_completions_dir}/${OVT_CLI_NAME}.fish" "${_ovt_completions_dir}/completions_data.cpp")
set(OVT_HOST_GEN_COMPLETIONS "" CACHE FILEPATH "Host-built ovt_gen_completions for cross builds without an emulator")
if(OVT_HOST_GEN_COMPLETIONS)
  set(_ovt_gen "${OVT_HOST_GEN_COMPLETIONS}")
elseif(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  set(_ovt_gen "")
else()
  add_executable(ovt_gen_completions tools/gen_completions.cpp)
  target_link_libraries(ovt_gen_completions PRIVATE ovt_cli_lib ovt::flags)
  ovt_set_license(ovt_gen_completions "GPL-3.0-or-later")
  set(_ovt_gen ovt_gen_completions) # add_custom_command runs it through CMAKE_CROSSCOMPILING_EMULATOR if set
endif()
if(_ovt_gen)
  add_custom_command(
    OUTPUT ${_ovt_completions_outputs}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ovt_completions_dir}"
    COMMAND ${_ovt_gen} "${_ovt_completions_dir}" "${OVT_CLI_NAME}"
    DEPENDS ${_ovt_gen}
    COMMENT "Generating ${OVT_CLI_NAME} shell completion scripts"
    VERBATIM)
else()
  # Cross build without a way to run the generator: `ovt completion` prints an empty script.
  message(WARNING "Cross build: pass -DOVT_HOST_GEN_COMPLETIONS=<host ovt_gen_completions> to embed completions")
  file(
    WRITE "${_ovt_completions_dir}/completions_data.cpp"
    "#include \"completions.h\"\novt::cli::CompletionScripts ovt::cli::embedded_completions() noexcept { return {}; }\n"
  )
  set(_ovt_completions_outputs "${_ovt_completions_dir}/completions_data.cpp")
endif()
add_library(ovt_cli_completions OBJECT "${_ovt_completions_dir}/completions_data.cpp")
target_link_libraries(ovt_cli_completions PRIVATE ovt_cli_lib ovt::flags)
add_custom_target(ovt_completions DEPENDS ${_ovt_completions_outputs})

# ---- The executable -----------------------------------------------------------------------------------------
add_executable(ovt_cli src/main.cpp $<TARGET_OBJECTS:ovt_cli_completions>)
target_link_libraries(ovt_cli PRIVATE ovt_cli_lib ovt::flags)
set_target_properties(ovt_cli PROPERTIES OUTPUT_NAME "${OVT_CLI_NAME}")
ovt_set_license(ovt_cli "GPL-3.0-or-later")

if(OVT_BUILD_TESTS)
  ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
  add_executable(ovt_tests_cli tests/test_globals.cpp tests/test_writers.cpp $<TARGET_OBJECTS:ovt_cli_completions>)
  target_link_libraries(ovt_tests_cli PRIVATE ovt_cli_lib ovt::flags ovt_schemas ${nlohmann_json_TARGET}
                                              ${Catch2_TARGET})
  ovt_set_license(ovt_tests_cli "GPL-3.0-or-later")
  ovt_catch_discover_tests(ovt_tests_cli)

  # The real binary, for the acceptance lines that are about the process (exit status, stream contents).
  add_test(NAME cli_completion_zsh COMMAND ovt_cli completion zsh)
  set_tests_properties(cli_completion_zsh PROPERTIES PASS_REGULAR_EXPRESSION "^#compdef ${OVT_CLI_NAME}\n" LABELS
                                                     "fast;unit;cli")
  add_test(NAME cli_no_args COMMAND "${CMAKE_COMMAND}" -DOVT_EXE=$<TARGET_FILE:ovt_cli> -P
                                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_args.cmake")
  set_tests_properties(cli_no_args PROPERTIES LABELS "fast;unit;cli")
endif()

# ---- JSON Schemas of the -f json output (chapter 12 §12.6, WI-1.501) -----------------------------------
# docs/spec/schemas/<Table>.schema.json and ovt-cli-1.schema.json are generated from core/schema/*.fbs and
# committed. `ovt_json_schema` rewrites them; `ovt_json_schema_check` (and the fast CTest
# cli_json_schema_up_to_date) fails with a diff when an .fbs changed without regenerating.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND OVT_FLATC_EXECUTABLE)
  set(_ovt_jsonschema_cmd
      "${Python3_EXECUTABLE}"
      -I
      "${PROJECT_SOURCE_DIR}/tools/fbs_to_jsonschema.py"
      --flatc
      "${OVT_FLATC_EXECUTABLE}"
      --schema-dir
      "${PROJECT_SOURCE_DIR}/core/schema"
      --out
      "${PROJECT_SOURCE_DIR}/docs/spec/schemas")
  add_custom_target(ovt_json_schema COMMAND ${_ovt_jsonschema_cmd}
                    COMMENT "Regenerating docs/spec/schemas/*.schema.json from core/schema/*.fbs" VERBATIM)
  add_custom_target(ovt_json_schema_check COMMAND ${_ovt_jsonschema_cmd} --check
                    COMMENT "Checking docs/spec/schemas/*.schema.json against core/schema/*.fbs" VERBATIM)
  if(OVT_BUILD_TESTS)
    add_test(NAME cli_json_schema_up_to_date COMMAND ${_ovt_jsonschema_cmd} --check)
    set_tests_properties(cli_json_schema_up_to_date PROPERTIES LABELS "fast;cli;tools")
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pytest, jsonschema" RESULT_VARIABLE _ovt_no_pytest
                    OUTPUT_QUIET ERROR_QUIET)
    if(_ovt_no_pytest EQUAL 0)
      add_test(NAME cli_schema_validate COMMAND "${Python3_EXECUTABLE}" -m pytest -q -p no:cacheprovider -rs
                                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_schema_validate.py"
               WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
      set_tests_properties(
        cli_schema_validate PROPERTIES LABELS "fast;cli;tools" ENVIRONMENT
                                       "OVT_CLI=$<TARGET_FILE:ovt_cli>;OVT_FLATC=${OVT_FLATC_EXECUTABLE}")
    else()
      message(STATUS "pytest or jsonschema not found for ${Python3_EXECUTABLE}: cli_schema_validate not registered "
                     "(pip install -r tools/requirements.txt -r tools/requirements-test.txt)")
    endif()
    unset(_ovt_no_pytest)
  endif()
endif()
