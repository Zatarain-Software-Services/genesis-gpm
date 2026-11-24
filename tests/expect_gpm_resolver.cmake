# End-to-end gpm resolver test. Builds a local "remote" — a plain git
# repository standing in for github.com plus a directory of tarballs
# standing in for codeload — then exercises add/install/update/--frozen
# against it via the GPM_GIT_BASE_URL and GPM_ARCHIVE_BASE_URL overrides.
# Required: -DPKG=<exe> -DWORK=<dir>

if(NOT DEFINED PKG OR NOT DEFINED WORK)
  message(FATAL_ERROR "expect_gpm_resolver.cmake requires PKG and WORK")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(REMOTES "${WORK}/remotes")
set(ARCHIVES "${WORK}/archives")
set(PROJECT "${WORK}/project")
file(MAKE_DIRECTORY "${REMOTES}/acme/widgets" "${REMOTES}/acme/gadgets"
              "${ARCHIVES}/acme/widgets/tar.gz" "${ARCHIVES}/acme/gadgets/tar.gz"
              "${PROJECT}")

function(git_in DIRECTORY)
  execute_process(COMMAND git -C "${DIRECTORY}" ${ARGN}
                  RESULT_VARIABLE git_result
                  OUTPUT_VARIABLE git_stdout
                  ERROR_VARIABLE git_stderr)
  if(NOT git_result EQUAL 0)
    message(FATAL_ERROR "git -C ${DIRECTORY} ${ARGN} failed: ${git_stderr}")
  endif()
endfunction()

function(write_package REPO_DIR NAME DEPS_JSON)
  file(WRITE "${REPO_DIR}/genesis.json"
    "{\n  \"name\": \"@acme/${NAME}\",\n  \"version\": \"1.0.0\",\n  \"entry\": \"index.genesis\",\n  \"dependencies\": ${DEPS_JSON}\n}\n")
  file(WRITE "${REPO_DIR}/index.genesis"
    "// ${NAME} package entry point\n")
endfunction()

# publish_package REPO_NAME DEPS_JSON OUT_SHA — creates a git "remote", commits
# it, and drops a codeload-style tarball named by the commit SHA into the
# archive tree, returning the SHA through OUT_SHA.
function(publish_package REPO_NAME DEPS_JSON OUT_SHA)
  set(repo_dir "${REMOTES}/acme/${REPO_NAME}")
  set(tar_dir "${WORK}/tarbuild/${REPO_NAME}")
  file(MAKE_DIRECTORY "${repo_dir}" "${tar_dir}"
                    "${ARCHIVES}/acme/${REPO_NAME}/tar.gz")
  write_package("${repo_dir}" "${REPO_NAME}" "${DEPS_JSON}")
  git_in("${repo_dir}" init -q)
  git_in("${repo_dir}" add genesis.json index.genesis)
  git_in("${repo_dir}" -c user.email=gpm@test -c user.name=gpm
         commit -qm "initial")
  execute_process(COMMAND git -C "${repo_dir}" rev-parse HEAD
                  OUTPUT_VARIABLE pkg_sha OUTPUT_STRIP_TRAILING_WHITESPACE)
  write_package("${tar_dir}" "${REPO_NAME}" "${DEPS_JSON}")
  # Archive name stays relative: GNU tar reads "C:/..." in -f as a remote
  # host, so WORKING_DIRECTORY keeps the path drive-letter-free everywhere.
  execute_process(COMMAND tar -czf "${REPO_NAME}-${pkg_sha}.tar.gz"
                        -C "${WORK}/tarbuild" "${REPO_NAME}"
                  WORKING_DIRECTORY "${WORK}")
  file(RENAME "${WORK}/${REPO_NAME}-${pkg_sha}.tar.gz"
       "${ARCHIVES}/acme/${REPO_NAME}/tar.gz/${pkg_sha}")
  set(${OUT_SHA} "${pkg_sha}" PARENT_SCOPE)
endfunction()

# gadgets: leaf package, no dependencies.
write_package("${REMOTES}/acme/gadgets" "gadgets" "{}")
git_in("${REMOTES}/acme/gadgets" init -q)
git_in("${REMOTES}/acme/gadgets" add genesis.json index.genesis)
git_in("${REMOTES}/acme/gadgets" -c user.email=gpm@test -c user.name=gpm
       commit -qm "initial")
execute_process(COMMAND git -C "${REMOTES}/acme/gadgets" rev-parse HEAD
                OUTPUT_VARIABLE GADGETS_SHA OUTPUT_STRIP_TRAILING_WHITESPACE)

# widgets: depends on gadgets to exercise lockfile flattening.
write_package("${REMOTES}/acme/widgets" "widgets"
              "{\"gadgets\": \"github:acme/gadgets\"}")
git_in("${REMOTES}/acme/widgets" init -q)
git_in("${REMOTES}/acme/widgets" add genesis.json index.genesis)
git_in("${REMOTES}/acme/widgets" -c user.email=gpm@test -c user.name=gpm
       commit -qm "initial")
execute_process(COMMAND git -C "${REMOTES}/acme/widgets" rev-parse HEAD
                OUTPUT_VARIABLE WIDGETS_SHA_V1 OUTPUT_STRIP_TRAILING_WHITESPACE)

# Publish tarballs named by commit SHA, mirroring codeload's layout. The
# archive top level is a single directory (stripped on unpack).
foreach(pkg widgets gadgets)
  if(pkg STREQUAL "widgets")
    set(pkg_sha "${WIDGETS_SHA_V1}")
    set(pkg_deps "{\"gadgets\": \"github:acme/gadgets\"}")
  else()
    set(pkg_sha "${GADGETS_SHA}")
    set(pkg_deps "{}")
  endif()
  file(MAKE_DIRECTORY "${WORK}/tarbuild/${pkg}")
  write_package("${WORK}/tarbuild/${pkg}" "${pkg}" "${pkg_deps}")
  execute_process(COMMAND tar -czf "${pkg}-${pkg_sha}.tar.gz"
                        -C "${WORK}/tarbuild" "${pkg}"
                  WORKING_DIRECTORY "${WORK}")
  # The archive URL ends in the commit SHA, so the payload must be a file
  # literally named by that SHA under the tar.gz directory.
  file(RENAME "${WORK}/${pkg}-${pkg_sha}.tar.gz"
       "${ARCHIVES}/acme/${pkg}/tar.gz/${pkg_sha}")
endforeach()

set(REGISTRY_DIR "${WORK}/registry")
file(MAKE_DIRECTORY "${REGISTRY_DIR}/packages")

function(run_pkg RESULT_VAR)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "GPM_GIT_BASE_URL=${REMOTES}"
            "GPM_ARCHIVE_BASE_URL=file://${ARCHIVES}"
            "GPM_REGISTRY_URL=file://${REGISTRY_DIR}"
            "GPM_KEYS_DIR=${WORK}/keys"
            "${PKG}" ${ARGN}
    WORKING_DIRECTORY "${PROJECT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text)
  set(${RESULT_VAR} "${result}" PARENT_SCOPE)
  set(last_stdout "${stdout_text}" PARENT_SCOPE)
  set(last_stderr "${stderr_text}" PARENT_SCOPE)
endfunction()

# Same as run_pkg but without GPM_REGISTRY_URL, for exercising the
# manifest "registry" field and the no-registry-configured error.
function(run_pkg_raw RESULT_VAR)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "GPM_GIT_BASE_URL=${REMOTES}"
            "GPM_ARCHIVE_BASE_URL=file://${ARCHIVES}"
            "GPM_KEYS_DIR=${WORK}/keys"
            "${PKG}" ${ARGN}
    WORKING_DIRECTORY "${PROJECT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text)
  set(${RESULT_VAR} "${result}" PARENT_SCOPE)
  set(last_stdout "${stdout_text}" PARENT_SCOPE)
  set(last_stderr "${stderr_text}" PARENT_SCOPE)
endfunction()

function(require_ok WHAT)
  if(NOT "${${WHAT}}" STREQUAL "0")
    message(FATAL_ERROR "${WHAT} failed (exit ${${WHAT}}): ${last_stderr}")
  endif()
endfunction()

function(require_fail WHAT EXPECT_TEXT)
  if("${${WHAT}}" STREQUAL "0")
    message(FATAL_ERROR "${WHAT} unexpectedly succeeded: ${last_stdout}")
  endif()
  if(NOT "${last_stderr}" MATCHES "${EXPECT_TEXT}")
    message(FATAL_ERROR "${WHAT} stderr '${last_stderr}' did not match '${EXPECT_TEXT}'")
  endif()
endfunction()

# init + add: widgets and its transitive gadgets dependency must install.
run_pkg(INIT_RESULT init)
require_ok(INIT_RESULT)
run_pkg(ADD_RESULT add github:acme/widgets)
require_ok(ADD_RESULT)

foreach(installed widgets gadgets)
  if(NOT EXISTS "${PROJECT}/.genesis/packages/${installed}/index.genesis")
    message(FATAL_ERROR "${installed} was not installed under .genesis/packages")
  endif()
  if(NOT EXISTS "${PROJECT}/.genesis/packages/${installed}/.gpm-verified")
    message(FATAL_ERROR "${installed} is missing its verification marker")
  endif()
endforeach()

file(READ "${PROJECT}/genesis.lock.json" lockfile_text)
if(NOT lockfile_text MATCHES "${WIDGETS_SHA_V1}")
  message(FATAL_ERROR "lockfile does not pin widgets commit ${WIDGETS_SHA_V1}: ${lockfile_text}")
endif()
if(NOT lockfile_text MATCHES "${GADGETS_SHA}")
  message(FATAL_ERROR "lockfile does not pin gadgets commit ${GADGETS_SHA}: ${lockfile_text}")
endif()
if(NOT lockfile_text MATCHES "\"sha256\": \"[0-9a-f]+\"")
  message(FATAL_ERROR "lockfile is missing an archive sha256: ${lockfile_text}")
endif()

run_pkg(CHECK_RESULT check)
require_ok(CHECK_RESULT)

# Frozen install sees verified markers and touches nothing.
run_pkg(FROZEN_RESULT install --frozen)
require_ok(FROZEN_RESULT)
if(NOT last_stdout MATCHES "already installed")
  message(FATAL_ERROR "frozen install did not reuse verified packages: ${last_stdout}")
endif()

# Removing the installed tree forces a verified re-fetch from the lockfile.
file(REMOVE_RECURSE "${PROJECT}/.genesis/packages/widgets")
run_pkg(REINSTALL_RESULT install --frozen)
require_ok(REINSTALL_RESULT)
if(NOT EXISTS "${PROJECT}/.genesis/packages/widgets/index.genesis")
  message(FATAL_ERROR "install --frozen did not restore widgets")
endif()

# A tampered lockfile hash must fail verification before unpacking.
file(READ "${PROJECT}/genesis.lock.json" tampered_lock)
string(REGEX REPLACE "\"sha256\": \"[0-9a-f]+\""
                   "\"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\""
                   tampered_lock "${tampered_lock}")
file(WRITE "${PROJECT}/genesis.lock.json" "${tampered_lock}")
file(REMOVE_RECURSE "${PROJECT}/.genesis/packages/gadgets")
run_pkg(TAMPER_RESULT install)
require_fail(TAMPER_RESULT "SHA-256 mismatch")
if(EXISTS "${PROJECT}/.genesis/packages/gadgets/index.genesis")
  message(FATAL_ERROR "failed verification still unpacked gadgets")
endif()

# Second commit on widgets; update re-resolves and re-pins.
file(APPEND "${REMOTES}/acme/widgets/index.genesis" "// v2\n")
git_in("${REMOTES}/acme/widgets" add index.genesis)
git_in("${REMOTES}/acme/widgets" -c user.email=gpm@test -c user.name=gpm
       commit -qm "v2")
execute_process(COMMAND git -C "${REMOTES}/acme/widgets" rev-parse HEAD
                OUTPUT_VARIABLE WIDGETS_SHA_V2 OUTPUT_STRIP_TRAILING_WHITESPACE)
file(MAKE_DIRECTORY "${WORK}/tarbuild/widgets-v2")
write_package("${WORK}/tarbuild/widgets-v2" "widgets"
              "{\"gadgets\": \"github:acme/gadgets\"}")
file(APPEND "${WORK}/tarbuild/widgets-v2/index.genesis" "// v2\n")
execute_process(COMMAND tar -czf "widgets-v2.tar.gz"
                      -C "${WORK}/tarbuild" "widgets-v2"
                WORKING_DIRECTORY "${WORK}")
# The archive top-level directory name is irrelevant (strip-components=1),
# so reuse the widgets path for the new commit's archive.
file(RENAME "${WORK}/widgets-v2.tar.gz"
     "${ARCHIVES}/acme/widgets/tar.gz/${WIDGETS_SHA_V2}")

run_pkg(UPDATE_RESULT update widgets)
require_ok(UPDATE_RESULT)
file(READ "${PROJECT}/genesis.lock.json" updated_lock)
if(NOT updated_lock MATCHES "${WIDGETS_SHA_V2}")
  message(FATAL_ERROR "update did not re-pin widgets to ${WIDGETS_SHA_V2}: ${updated_lock}")
endif()
if(EXISTS "${PROJECT}/.genesis/packages/widgets/.staging-widgets")
  message(FATAL_ERROR "staging directory was left behind")
endif()

# The tamper test above left the lockfile corrupted, so drop it and the
# installed tree: a plain install must re-resolve every manifest dependency,
# including flattened transitives.
file(REMOVE "${PROJECT}/genesis.lock.json")
file(REMOVE_RECURSE "${PROJECT}/.genesis/packages")
run_pkg(REBUILD_RESULT install)
require_ok(REBUILD_RESULT)
foreach(installed widgets gadgets)
  if(NOT EXISTS "${PROJECT}/.genesis/packages/${installed}/index.genesis")
    message(FATAL_ERROR "rebuild did not reinstall ${installed}")
  endif()
endforeach()
file(READ "${PROJECT}/genesis.lock.json" rebuilt_lock)
if(NOT rebuilt_lock MATCHES "${WIDGETS_SHA_V2}" OR
   NOT rebuilt_lock MATCHES "${GADGETS_SHA}")
  message(FATAL_ERROR "rebuild did not re-pin expected commits: ${rebuilt_lock}")
endif()

# Adding under an alias the manifest already declares must fail before
# anything is recorded, even when the source differs.
run_pkg(ALIAS_RESULT add github:acme/gadgets --alias widgets)
require_fail(ALIAS_RESULT "already declares dependency")

# Transitive alias conflict: toolbox locks 'shared' to gadgets while crate
# wants the same alias pointed at widgets.
publish_package("toolbox" "{\"shared\": \"github:acme/gadgets\"}" TOOLBOX_SHA)
publish_package("crate" "{\"shared\": \"github:acme/widgets\"}" CRATE_SHA)
run_pkg(TOOLBOX_RESULT add github:acme/toolbox)
require_ok(TOOLBOX_RESULT)
if(NOT EXISTS "${PROJECT}/.genesis/packages/shared/index.genesis")
  message(FATAL_ERROR "toolbox's transitive 'shared' dependency was not installed")
endif()
# 'shared' is locked to gadgets but is not a manifest dependency, so this
# exercises the lockfile-level conflict path directly.
run_pkg(SHARED_RESULT add github:acme/widgets --alias shared)
require_fail(SHARED_RESULT "already locked to a different source")
run_pkg(CRATE_RESULT add github:acme/crate)
require_fail(CRATE_RESULT "already locked to a different source")
file(READ "${PROJECT}/genesis.json" manifest_text)
if(manifest_text MATCHES "\"crate\"")
  message(FATAL_ERROR "failed add still recorded crate in genesis.json")
endif()
file(READ "${PROJECT}/genesis.lock.json" lock_text)
if(lock_text MATCHES "\"crate\"")
  message(FATAL_ERROR "failed add still recorded crate in the lockfile")
endif()

# A package whose dependencies are already locked to the same sources must
# deduplicate cleanly instead of refetching or conflicting.
publish_package("widgetpack"
  "{\"gadgets\": \"github:acme/gadgets\", \"widgets\": \"github:acme/widgets\"}"
  WIDGETPACK_SHA)
run_pkg(WIDGETPACK_RESULT add github:acme/widgetpack)
require_ok(WIDGETPACK_RESULT)
run_pkg(FROZEN_AGAIN_RESULT install --frozen)
require_ok(FROZEN_AGAIN_RESULT)

# A manifest whose sources disagree with the lockfile pins is stale and must
# fail --frozen even though every alias is present.
file(READ "${PROJECT}/genesis.json" manifest_backup)
string(REPLACE "\"github:acme/widgets\"" "\"github:acme/gadgets\""
               manifest_tampered "${manifest_backup}")
file(WRITE "${PROJECT}/genesis.json" "${manifest_tampered}")
run_pkg(STALE_RESULT install --frozen)
require_fail(STALE_RESULT "missing or stale")
file(WRITE "${PROJECT}/genesis.json" "${manifest_backup}")

# update of an alias that is not a manifest dependency fails cleanly.
run_pkg(UPDATE_GHOST_RESULT update ghost)
require_fail(UPDATE_GHOST_RESULT "No dependency named 'ghost'")

# An unknown remote fails resolution cleanly.
run_pkg(GHOST_RESULT add github:acme/ghost)
require_fail(GHOST_RESULT "gpm:")

# --- registry client (docs/registry.md) ------------------------------------
# The registry is a static JSON tree fetched over file:// here; entries
# point at the same local git "remote". widgets gets v1/v2 tags so a
# registry version can select an older ref explicitly.
git_in("${REMOTES}/acme/widgets" tag v1 "${WIDGETS_SHA_V1}")
git_in("${REMOTES}/acme/widgets" tag v2 "${WIDGETS_SHA_V2}")
publish_package("gizmo" "{}" GIZMO_SHA)

file(WRITE "${REGISTRY_DIR}/index.json"
  "{\n  \"registry\": \"test\",\n  \"packages\": [\n"
  "    { \"alias\": \"gizmo\", \"name\": \"@acme/gizmo\", \"description\": \"A gizmo\", \"latest\": \"1.0.0\" },\n"
  "    { \"alias\": \"widgets-legacy\", \"name\": \"@acme/widgets\", \"description\": \"Widgets archive\", \"latest\": \"2.0.0\" }\n"
  "  ]\n}\n")
file(WRITE "${REGISTRY_DIR}/packages/gizmo.json"
  "{\n  \"alias\": \"gizmo\",\n  \"name\": \"@acme/gizmo\",\n"
  "  \"description\": \"A gizmo\",\n  \"latest\": \"1.0.0\",\n"
  "  \"versions\": [\n"
  "    { \"version\": \"1.0.0\", \"source\": \"github:acme/gizmo\" }\n"
  "  ]\n}\n")
file(WRITE "${REGISTRY_DIR}/packages/widgets-legacy.json"
  "{\n  \"alias\": \"widgets-legacy\",\n  \"name\": \"@acme/widgets\",\n"
  "  \"description\": \"Widgets archive\",\n  \"latest\": \"2.0.0\",\n"
  "  \"versions\": [\n"
  "    { \"version\": \"1.0.0\", \"source\": \"github:acme/widgets@v1\" },\n"
  "    { \"version\": \"2.0.0\", \"source\": \"github:acme/widgets@v2\" }\n"
  "  ]\n}\n")

# search and info read index.json / packages/<alias>.json.
run_pkg(REG_SEARCH_RESULT search giz)
require_ok(REG_SEARCH_RESULT)
if(NOT last_stdout MATCHES "gizmo.*@acme/gizmo.*A gizmo")
  message(FATAL_ERROR "search output unexpected: ${last_stdout}")
endif()
run_pkg(REG_INFO_RESULT info gizmo)
require_ok(REG_INFO_RESULT)
if(NOT last_stdout MATCHES "@acme/gizmo" OR
   NOT last_stdout MATCHES "1.0.0.*github:acme/gizmo")
  message(FATAL_ERROR "info output unexpected: ${last_stdout}")
endif()

# Bare-alias add resolves through GPM_REGISTRY_URL and records the
# resolved github: source in the manifest.
run_pkg(REG_ADD_RESULT add gizmo)
require_ok(REG_ADD_RESULT)
if(NOT EXISTS "${PROJECT}/.genesis/packages/gizmo/index.genesis")
  message(FATAL_ERROR "registry add did not install gizmo")
endif()
file(READ "${PROJECT}/genesis.json" reg_manifest)
if(NOT reg_manifest MATCHES "\"gizmo\": \"github:acme/gizmo\"")
  message(FATAL_ERROR "manifest did not record resolved source: ${reg_manifest}")
endif()
file(READ "${PROJECT}/genesis.lock.json" reg_lock)
if(NOT reg_lock MATCHES "${GIZMO_SHA}")
  message(FATAL_ERROR "lockfile does not pin gizmo commit: ${reg_lock}")
endif()

# --version selects a non-latest entry; an unknown version fails with the
# available list.
run_pkg(REG_VER_RESULT add widgets-legacy --version 1.0.0)
require_ok(REG_VER_RESULT)
file(READ "${PROJECT}/genesis.lock.json" ver_lock)
if(NOT ver_lock MATCHES "${WIDGETS_SHA_V1}")
  message(FATAL_ERROR "--version 1.0.0 did not pin widgets v1: ${ver_lock}")
endif()
# Resolution runs before the duplicate-alias check, so an unknown version
# fails with the available list even for an already-installed alias.
run_pkg(REG_BADVER_RESULT add widgets-legacy --version 9.9.9)
require_fail(REG_BADVER_RESULT "no version '9.9.9'")

# Without any registry source configured, a bare alias fails cleanly.
# (The manifest has no "registry" field and run_pkg_raw drops the env.)
run_pkg_raw(REG_NOCFG_RESULT add nothere)
require_fail(REG_NOCFG_RESULT "No registry configured")

# The manifest "registry" field works with no env var or flag.
file(READ "${PROJECT}/genesis.json" reg_manifest2)
string(REPLACE "\"entry\": \"index.genesis\""
  "\"entry\": \"index.genesis\",\n  \"registry\": \"file://${REGISTRY_DIR}\""
  reg_manifest2 "${reg_manifest2}")
file(WRITE "${PROJECT}/genesis.json" "${reg_manifest2}")
run_pkg_raw(REG_MANIFEST_RESULT info gizmo)
require_ok(REG_MANIFEST_RESULT)
if(NOT last_stdout MATCHES "@acme/gizmo")
  message(FATAL_ERROR "manifest registry field not honored: ${last_stdout}")
endif()
run_pkg_raw(REG_MANIFEST_CHECK check)
require_ok(REG_MANIFEST_CHECK)
# --registry flag overrides everything for a single invocation.
run_pkg_raw(REG_FLAG_RESULT info gizmo --registry "file://${REGISTRY_DIR}")
require_ok(REG_FLAG_RESULT)

# Restore the manifest without the registry field for the signing tests.
file(READ "${PROJECT}/genesis.json" reg_manifest3)
string(REPLACE ",\n  \"registry\": \"file://${REGISTRY_DIR}\"" ""
               reg_manifest3 "${reg_manifest3}")
file(WRITE "${PROJECT}/genesis.json" "${reg_manifest3}")

# --- signed packages (docs/signing.md) ------------------------------------
# A real keypair comes from `gpm keygen`; `gpm sign` produces the detached
# .sig published next to the archive, which is exactly the
# "<archive-url>.sig" sidecar the resolver downloads.
publish_package("signedpkg" "{}" SIGNEDPKG_SHA)

run_pkg(KEYGEN_RESULT keygen)
require_ok(KEYGEN_RESULT)
string(REGEX MATCH "keyId: +([A-Za-z0-9_-]+)" _ "${last_stdout}")
set(pkg_key_id "${CMAKE_MATCH_1}")
string(REGEX MATCH "publicKey: +([A-Za-z0-9_-]+)" _ "${last_stdout}")
set(pkg_public_key "${CMAKE_MATCH_1}")
string(REGEX MATCH "privateKey: +([^ \n]+)" _ "${last_stdout}")
set(pkg_key_file "${CMAKE_MATCH_1}")
if(NOT pkg_key_id OR NOT pkg_public_key OR NOT pkg_key_file)
  message(FATAL_ERROR "keygen output unparsable: ${last_stdout}")
endif()

set(signed_archive "${ARCHIVES}/acme/signedpkg/tar.gz/${SIGNEDPKG_SHA}")
run_pkg(SIGN_RESULT sign --key "${pkg_key_file}" --alias signedpkg
        --version 1.0.0 "${signed_archive}")
require_ok(SIGN_RESULT)
if(NOT EXISTS "${signed_archive}.sig")
  message(FATAL_ERROR "gpm sign did not produce the archive sidecar")
endif()

# Trust the key in the project manifest; required=true makes unsigned
# packages fail from here on.
file(READ "${PROJECT}/genesis.json" signed_manifest)
if(NOT signed_manifest MATCHES "\"entry\": \"index.genesis\"")
  message(FATAL_ERROR "unexpected manifest layout: ${signed_manifest}")
endif()
string(REPLACE "\"entry\": \"index.genesis\""
  "\"entry\": \"index.genesis\",\n  \"trustedPackageKeys\": {\n    \"${pkg_key_id}\": \"${pkg_public_key}\"\n  },\n  \"signedPackagesRequired\": true"
  signed_manifest "${signed_manifest}")
file(WRITE "${PROJECT}/genesis.json" "${signed_manifest}")

run_pkg(ADDSIGNED_RESULT add github:acme/signedpkg)
require_ok(ADDSIGNED_RESULT)
if(NOT EXISTS "${PROJECT}/.genesis/packages/signedpkg/index.genesis")
  message(FATAL_ERROR "signed package was not installed")
endif()
file(READ "${PROJECT}/genesis.lock.json" signed_lock)
if(NOT signed_lock MATCHES "GENESIS-SIG-1")
  message(FATAL_ERROR "lockfile did not record the signature: ${signed_lock}")
endif()
if(NOT signed_lock MATCHES "trustedPackageKeys")
  message(FATAL_ERROR "lockfile did not pin trustedPackageKeys: ${signed_lock}")
endif()

# Frozen reinstall verifies the lockfile-pinned signature before fetch.
file(REMOVE_RECURSE "${PROJECT}/.genesis/packages/signedpkg")
run_pkg(SIGNED_REINSTALL_RESULT install --frozen)
require_ok(SIGNED_REINSTALL_RESULT)

# signedPackagesRequired now rejects unsigned packages.
publish_package("unsignedpkg" "{}" UNSIGNEDPKG_SHA)
run_pkg(ADDU_RESULT add github:acme/unsignedpkg)
require_fail(ADDU_RESULT "signedPackagesRequired")

# A signature from a key outside trustedPackageKeys must be rejected even
# though a signature is present.
run_pkg(KEYGEN2_RESULT keygen)
require_ok(KEYGEN2_RESULT)
string(REGEX MATCH "privateKey: +([^ \n]+)" _ "${last_stdout}")
set(other_key_file "${CMAKE_MATCH_1}")
run_pkg(SIGN2_RESULT sign --key "${other_key_file}" --alias signedpkg
        --version 1.0.0 "${signed_archive}")
require_ok(SIGN2_RESULT)
file(REMOVE_RECURSE "${PROJECT}/.genesis/packages/signedpkg")
run_pkg(UPDATE_BAD_RESULT update signedpkg)
require_fail(UPDATE_BAD_RESULT "not listed in trustedPackageKeys")

# --- signed registry documents (docs/registry.md trust model) --------------
# packages/<alias>.json.sig and index.json.sig sign the raw document bytes
# with the same GENESIS-SIG-1 envelope; gpm sign-file produces them.
# Verification is advisory: absent sidecars and unpinned signers are
# ignored, but a pinned signature that fails is a hard error.
run_pkg(SIGNFILE_RESULT sign-file --key "${pkg_key_file}"
        "${REGISTRY_DIR}/packages/gizmo.json")
require_ok(SIGNFILE_RESULT)
if(NOT EXISTS "${REGISTRY_DIR}/packages/gizmo.json.sig")
  message(FATAL_ERROR "gpm sign-file did not produce the sidecar")
endif()
run_pkg(SIGNFILE_INDEX sign-file --key "${pkg_key_file}"
        "${REGISTRY_DIR}/index.json")
require_ok(SIGNFILE_INDEX)
run_pkg(VERIFYFILE_RESULT verify-file --key "${pkg_key_file}"
        "${REGISTRY_DIR}/packages/gizmo.json")
require_ok(VERIFYFILE_RESULT)

# Signed docs verify cleanly through info (package doc) and search (index).
run_pkg(REG_SIG_INFO info gizmo)
require_ok(REG_SIG_INFO)
if(NOT last_stdout MATCHES "@acme/gizmo")
  message(FATAL_ERROR "signed registry info failed: ${last_stdout}")
endif()
run_pkg(REG_SIG_SEARCH search giz)
require_ok(REG_SIG_SEARCH)

# A signature from an unpinned key is ignored, not an error.
run_pkg(SIGNFILE_OTHER sign-file --key "${other_key_file}"
        "${REGISTRY_DIR}/packages/gizmo.json"
        "${REGISTRY_DIR}/packages/gizmo.json.sig")
require_ok(SIGNFILE_OTHER)
run_pkg(REG_UNPINNED_INFO info gizmo)
require_ok(REG_UNPINNED_INFO)

# Restore the pinned signature, then prove tampering is caught: the doc
# bytes are covered exactly, so any edit fails verification.
run_pkg(SIGNFILE_RESTORE sign-file --key "${pkg_key_file}"
        "${REGISTRY_DIR}/packages/gizmo.json"
        "${REGISTRY_DIR}/packages/gizmo.json.sig")
require_ok(SIGNFILE_RESTORE)
file(READ "${REGISTRY_DIR}/packages/gizmo.json" gizmo_doc)
file(WRITE "${REGISTRY_DIR}/packages/gizmo.json" "${gizmo_doc} ")
run_pkg(REG_TAMPER_INFO info gizmo)
require_fail(REG_TAMPER_INFO "signature verification failed")
# The add path verifies too: resolution fails before version selection.
file(READ "${REGISTRY_DIR}/packages/widgets-legacy.json" wl_doc)
file(WRITE "${REGISTRY_DIR}/packages/widgets-legacy.json" "${wl_doc}")
run_pkg(SIGNFILE_WL sign-file --key "${pkg_key_file}"
        "${REGISTRY_DIR}/packages/widgets-legacy.json"
        "${REGISTRY_DIR}/packages/widgets-legacy.json.sig")
require_ok(SIGNFILE_WL)
file(APPEND "${REGISTRY_DIR}/packages/widgets-legacy.json" " ")
run_pkg(REG_TAMPER_ADD add widgets-legacy --version 1.0.0)
require_fail(REG_TAMPER_ADD "signature verification failed")

message(STATUS "gpm resolver end-to-end test passed")
