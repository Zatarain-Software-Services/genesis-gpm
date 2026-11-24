cmake_minimum_required(VERSION 3.16)
project(genesis-gpm C)

# genesis-gpm: the standalone package manager for Genesis source
# packages. Extracted from the Genesis monorepo (tools/gpm); see
# docs/package-manager.md for the manifest/lockfile format and
# docs/registry.md for the registry client.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CTest)

# Vendored tweetnacl.c carries the known-benign carry-shift UB (fine on
# two's-complement targets, UB only by the letter of the standard), so
# the shift sanitizer is disabled for this file only.
set_source_files_properties(src/vendor/tweetnacl/tweetnacl.c PROPERTIES
  COMPILE_OPTIONS "$<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-error=sign-compare>;$<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-error=unterminated-string-initialization>;$<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-fno-sanitize=shift>")

add_executable(gpm
  src/gpm.c
  src/json.c
  src/manifest.c
  src/registry.c
  src/resolver.c
  src/signing.c
  src/signature.c
  src/vendor/tweetnacl/tweetnacl.c)
target_include_directories(gpm PRIVATE src)
if(MSVC)
  target_compile_options(gpm PRIVATE /W4)
else()
  target_compile_options(gpm PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  enable_testing()
  # gpm scenarios run in per-test scratch directories under the
  # build tree; fixtures live in tests/gpm_fixtures/.
  set(_GPM_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpm_fixtures)
  foreach(_gpm_scenario
      init check_valid check_malformed check_missing_manifest
      check_invalid_manifest check_invalid_name check_invalid_lockfile
      check_no_lockfile check_trust_fields check_bad_trust_keyid
      keygen_sign_verify remove remove_missing_alias tree)
    set(_gpm_fixture "")
    if(_gpm_scenario STREQUAL "check_valid" OR _gpm_scenario STREQUAL "remove")
      set(_gpm_fixture "${_GPM_FIXTURES}/valid")
    elseif(_gpm_scenario STREQUAL "check_malformed")
      set(_gpm_fixture "${_GPM_FIXTURES}/malformed")
    elseif(_gpm_scenario STREQUAL "check_invalid_manifest")
      set(_gpm_fixture "${_GPM_FIXTURES}/invalid_manifest")
    elseif(_gpm_scenario STREQUAL "check_invalid_name")
      set(_gpm_fixture "${_GPM_FIXTURES}/invalid_name")
    elseif(_gpm_scenario STREQUAL "check_invalid_lockfile")
      set(_gpm_fixture "${_GPM_FIXTURES}/invalid_lockfile")
    elseif(_gpm_scenario STREQUAL "check_trust_fields")
      set(_gpm_fixture "${_GPM_FIXTURES}/trust_fields")
    elseif(_gpm_scenario STREQUAL "check_bad_trust_keyid")
      set(_gpm_fixture "${_GPM_FIXTURES}/bad_trust_keyid")
    elseif(_gpm_scenario STREQUAL "check_no_lockfile" OR
           _gpm_scenario STREQUAL "remove_missing_alias")
      set(_gpm_fixture "${_GPM_FIXTURES}/manifest_only")
    elseif(_gpm_scenario STREQUAL "tree")
      set(_gpm_fixture "${_GPM_FIXTURES}/tree")
    endif()
    add_test(NAME gpm_${_gpm_scenario}
      COMMAND ${CMAKE_COMMAND}
        -DPKG=$<TARGET_FILE:gpm>
        -DSCENARIO=${_gpm_scenario}
        -DWORK=${CMAKE_BINARY_DIR}/gpm-tests/${_gpm_scenario}
        -DFIXTURE=${_gpm_fixture}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_gpm.cmake)
  endforeach()
  # Resolver end-to-end: local git repo + file:// archive stand in for
  # github.com/codeload via the GPM_*_BASE_URL overrides; the registry
  # scenarios use a file:// registry tree via GPM_REGISTRY_URL. Requires
  # git, curl, and tar on PATH (git-for-windows ships all three on Windows).
  add_test(NAME gpm_resolver_end_to_end
    COMMAND ${CMAKE_COMMAND}
      -DPKG=$<TARGET_FILE:gpm>
      -DWORK=${CMAKE_BINARY_DIR}/gpm-tests/resolver_end_to_end
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_gpm_resolver.cmake)
endif()
