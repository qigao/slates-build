cmake_minimum_required(VERSION 3.27)

function(run_at directory)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${directory}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${ARGN} failed (${result})\n${stdout}\n${stderr}")
  endif()
  set(run_output "${stdout}" PARENT_SCOPE)
  set(run_error "${stderr}" PARENT_SCOPE)
endfunction()

function(reject regex)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(result EQUAL 0 OR NOT "${stdout}\n${stderr}" MATCHES "${regex}")
    message(FATAL_ERROR "${ARGN} should fail with '${regex}'\n${stdout}\n${stderr}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}/config" "${WORK}/apps/test" "${WORK}/modules/math"
  "${WORK}/modules/io" "${WORK}/modules/io/include" "${WORK}/modules/shared"
  "${WORK}/sdk/fixture-triplet/include" "${WORK}/sdk/fixture-triplet/debug/include")
string(TOLOWER "${HOST_ARCH}" arch)
if(arch MATCHES "^(amd64|x86_64)$")
  set(arch x64)
elseif(arch STREQUAL "aarch64")
  set(arch arm64)
endif()
string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" os)
if(os STREQUAL "darwin")
  set(os macos)
endif()
if(arch STREQUAL "x64")
  set(render_arch arm64)
  set(render_machine aarch64)
else()
  set(render_arch x64)
  set(render_machine x86_64)
endif()
if(MSVC_DRIVER)
  set(driver msvc)
  set(cxx "${CC}")
  get_filename_component(bindir "${CC}" DIRECTORY)
  find_program(ld NAMES link.exe HINTS "${bindir}" NO_DEFAULT_PATH REQUIRED)
elseif(COMPILER_ID STREQUAL "GNU")
  set(driver gcc)
  string(REGEX REPLACE "gcc([^/]*)$" "g++\\1" cxx "${CC}")
  set(ld "${cxx}")
else()
  set(driver clang)
  string(REGEX REPLACE "clang([^/]*)$" "clang++\\1" cxx "${CC}")
  set(ld "${cxx}")
endif()
if(NOT EXISTS "${cxx}")
  message(FATAL_ERROR "Could not derive C++ compiler from fixture C compiler")
endif()
file(TO_CMAKE_PATH "${CC}" cc)
file(TO_CMAKE_PATH "${AR}" ar)
file(TO_CMAKE_PATH "${cxx}" cxx)
file(TO_CMAKE_PATH "${ld}" ld)

file(WRITE "${WORK}/slate.json" [=[
{
  // Comments, relative includes and trailing commas are supported.
  "include": ["config/native.json", "config/tools.json", /* A comment after a trailing comma
                                      is treated as whitespace. */],
  "defaultPreset": "debug",
  "modules": ["modules/math", "modules/shared"],
  "moduleFiles": ["apps/test/module.json"],
  "env": ["SALTS_FIXTURE_CMAKE"],
  "presets": {
    "base": { "cStd": "c17", "cxxStd": "c++17", "styles": ["strict"] },
    "debug": { "inherits": "base", "config": "debug" },
    "release": { "inherits": "base", "config": "release" },
    "no-tests": { "inherits": "debug", "tests": false },
    "custom-templates": { "inherits": "debug", "templateDir": "custom templates" }
  },
  "styles": { "strict": { "options": { "warnings": "strict", "werror": true } } },
}
]=])
set(toolchain [=[
{
  "env": ["SALTS_FIXTURE_CC"],
  "platforms": {
    "native": {
      "os": "@os@", "arch": "@arch@",
      "toolchains": [
        { "id": "missing" },
        { "id": "native", "vcpkgTriplet": "fixture-triplet", "vcpkgRoot": "sdk" }
      ]
    },
    "render-macos": {
      "os": "macos", "arch": "@render_arch@",
      "toolchains": [{ "id": "render-macos", "vcpkgTriplet": "fixture-triplet", "vcpkgRoot": "sdk" }]
    },
    "render-linux": {
      "os": "linux", "arch": "@render_arch@",
      "toolchains": [{ "id": "render-linux", "vcpkgTriplet": "fixture-triplet", "vcpkgRoot": "sdk" }]
    }
  },
  "toolchains": {
    "missing": {
      "driver": "gcc", "cc": "fixture-nonexistent-compiler", "cxx": "@cxx@", "ar": "@ar@", "ld": "@ld@"
    },
    "native": {
      "driver": "@driver@", "cc": "${env:SALTS_FIXTURE_CC}", "cxx": "@cxx@", "ar": "@ar@", "ld": "@ld@"
    },
    "render-macos": {
      "driver": "clang", "cc": "@cc@", "cxx": "@cxx@", "ar": "@ar@", "ld": "@ld@",
      "triple": "@render_machine@-apple-macos"
    },
    "render-linux": {
      "driver": "clang", "cc": "@cc@", "cxx": "@cxx@", "ar": "@ar@", "ld": "@ld@",
      "triple": "@render_machine@-linux-gnu"
    }
  }
}
]=])
string(CONFIGURE "${toolchain}" toolchain @ONLY)
file(WRITE "${WORK}/config/native.json" "${toolchain}")
file(WRITE "${WORK}/config/tools.json" [=[
{"tools":{"copy_file":{"program":"${env:SALTS_FIXTURE_CMAKE}","args":["-E","copy"]}}}
]=])
set(fixture_cmake "${CMAKE_COMMAND}")
if(CMAKE_HOST_WIN32)
  string(REPLACE "/" "\\" fixture_cmake "${fixture_cmake}")
endif()
file(WRITE "${WORK}/.env"
  "SALTS_FIXTURE_CC=\"${cc}\"\nSALTS_FIXTURE_CMAKE=\"${fixture_cmake}\"\n")
file(WRITE "${WORK}/modules/io/io-targets.json" [=[
{
  "kind": "static",
  "visibility": ["math"],
  "ninjaRules": {"compile": "fixture_compile", "archive": "fixture_archive", "task": "fixture_task"},
  "sources": { "files": ["io.c"] },
  "public": {
    "includeDirs": ["$builddir/gen/io", "$vcpkg/include", "$vcpkgConfig/include"],
    "requiredIncludeDirs": ["include"]
  },
  "variants": [{
    "when": { "driver": ["clang"] },
    "ninjaRules": {"compile": "fixture_clang_compile"},
    "sources": { "objc": ["io.c"] },
    "private": { "defines": { "FIXTURE_OBJC": null } }
  }],
  "preTasks": [{
    "name": "number", "builtin": "mustache", "template": "number.h.mustache",
    "data": "number.json", "outputs": ["$builddir/staged/number.h"]
  }, {
    "name": "copy_number", "builtin": "copy",
    "inputs": ["$builddir/staged/number.h"],
    "outputs": ["$builddir/staged/number.h.copy"]
  }, {
    "name": "program_copy", "tool": "copy_file",
    "args": ["$builddir/staged/number.h.copy", "$builddir/gen/io/number.h"],
    "inputs": ["$builddir/staged/number.h.copy"],
    "outputs": ["$builddir/gen/io/number.h"]
  }]
}
]=])
file(WRITE "${WORK}/sdk/fixture-triplet/include/fixture_sdk.h" "#define FIXTURE_SDK 1\n")
file(WRITE "${WORK}/sdk/fixture-triplet/debug/include/fixture_sdk_debug.h" "#define FIXTURE_SDK_DEBUG 1\n")
file(WRITE "${WORK}/modules/io/include/io.h"
  "#include \"number.h\"\n#include \"fixture_sdk.h\"\n#include \"fixture_sdk_debug.h\"\nint io_value(void);\n")
file(WRITE "${WORK}/modules/io/number.h.mustache" "#define VALUE {{value}}\n")
file(WRITE "${WORK}/modules/io/number.json" "{\"value\": 17}\n")
file(WRITE "${WORK}/modules/io/io.c"
  "#include \"io.h\"\n#if defined(FIXTURE_OBJC) && !defined(__OBJC__)\n#error Expected Objective-C language\n#endif\nint io_value(void) { return VALUE; }\n")
file(WRITE "${WORK}/modules/math/module.json" [=[
{
  "include": ["../io/io-targets.json", "../io/./io-targets.json"],
  "kind": "static",
  "sources": { "files": ["math.c"] },
  "private": { "deps": ["io"] }
}
]=])
file(WRITE "${WORK}/modules/math/math.c" "#include \"io.h\"\nint twice(void) { return io_value() + VALUE; }\n")
file(WRITE "${WORK}/modules/shared/module.json" [=[
{
  "kind": "shared",
  "sources": { "files": ["shared.c"] },
  "private": { "deps": ["math"] }
}
]=])
file(WRITE "${WORK}/modules/shared/shared.c" [=[
int twice(void);
#ifdef _WIN32
__declspec(dllexport)
#endif
int shared_value(void) { return twice(); }
]=])
file(WRITE "${WORK}/apps/test/module.json" [=[
{
  "kind": "executable",
  "ninjaRules": {"link": "fixture_link"},
  "test": {
    "name": "fixture.cpp", "timeout": 10,
    "program": "${env:SALTS_FIXTURE_CMAKE}",
    "workingDirectory": ".",
    "environment": ["SLATE_FIXTURE_TEST=literal;with=separators"],
    "labels": ["smoke"],
    "args": ["-E", "env", "$target", "escaped quote \" then // comma ,] /* literal */",]
  },
  "sources": { "files": ["main.cpp"] },
  "private": {
    "deps": ["shared"],
    "defines": { "FIXTURE_CMAKE_PATH": "\"${envPath:SALTS_FIXTURE_CMAKE}\"" }
  }
}
]=])
set(main [=[
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C"
#ifdef _WIN32
__declspec(dllimport)
#endif
int shared_value(void);
int main(int argc, char **argv) {
  const char *value = std::getenv("SLATE_FIXTURE_TEST");
  std::FILE *marker = std::fopen("test-cwd.txt", "r");
  if (!marker) return 2;
  std::fclose(marker);
  std::FILE *tool = std::fopen(FIXTURE_CMAKE_PATH, "rb");
  if (!tool) return 5;
  std::fclose(tool);
  if (!value || std::strcmp(value, "literal;with=separators")) return 3;
  if (argc != 2 || std::strcmp(argv[1], "escaped quote \" then // comma ,] /* literal */")) return 4;
  return shared_value() == @expected@ ? 0 : 1;
}
]=])
file(WRITE "${WORK}/apps/test/test-cwd.txt" "working directory marker\n")
set(expected 34)
string(CONFIGURE "${main}" first_main @ONLY)
file(WRITE "${WORK}/apps/test/main.cpp" "${first_main}")

if(DEFINED TEST_INSTALL_TREE AND
    NOT IS_ABSOLUTE "${TEST_INSTALL_BINDIR}" AND NOT IS_ABSOLUTE "${TEST_INSTALL_DATADIR}")
  string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef bundle_run)
  set(bundle_dir "${WORK}/out/bundle ${bundle_run}")
  set(staged "${bundle_dir}/original prefix")
  set(relocated "${bundle_dir}/relocated prefix")
  set(install_args --install "${TEST_INSTALL_TREE}" --prefix "${staged}")
  if(TEST_INSTALL_CONFIG)
    list(APPEND install_args --config "${TEST_INSTALL_CONFIG}")
  endif()
  run_at("${WORK}" "${CMAKE_COMMAND}" ${install_args})
  file(COPY "${staged}/" DESTINATION "${relocated}")
  set(bundle_template "${relocated}/${TEST_INSTALL_DATADIR}/slates/build.ninja.j2")
  file(READ "${bundle_template}" bundled_ninja)
  file(WRITE "${bundle_template}" "${bundled_ninja}\n# Rendered from the relocated bundle\n")
  get_filename_component(builder_name "${BUILDER}" NAME)
  set(bundle_out "${bundle_dir}/build")
  run_at("${WORK}" "${relocated}/${TEST_INSTALL_BINDIR}/${builder_name}" gen
    --target math --build-dir "${bundle_out}")
  file(READ "${bundle_out}/build.ninja" bundle_manifest)
  if(NOT bundle_manifest MATCHES "# Rendered from the relocated bundle")
    message(FATAL_ERROR "Installed Slate did not load its relocated template")
  endif()
  run_at("${bundle_out}" "${NINJA}")
  set(bundle_test_out "${bundle_dir}/tests")
  run_at("${WORK}" "${relocated}/${TEST_INSTALL_BINDIR}/${builder_name}" test
    --preset no-tests --target test --build-dir "${bundle_test_out}")
  if(NOT run_output MATCHES "PASS fixture[.]cpp" OR
      EXISTS "${bundle_test_out}/CTestTestfile.cmake")
    message(FATAL_ERROR "Relocated Slate did not build and run the test directly\n${run_output}")
  endif()
endif()

run_at("${WORK}/apps/test" "${BUILDER}" doctor --targets test)
if(NOT run_output MATCHES "skipped missing" OR NOT run_output MATCHES "vcpkg triplet: fixture-triplet")
  message(FATAL_ERROR "automatic toolchain selection not reported: ${run_output}")
endif()
string(REPEAT "{name}-%s" 500 diagnostic_payload)
set(bad_option "--unknown-${diagnostic_payload}")
execute_process(COMMAND "${BUILDER}" doctor "${bad_option}"
  WORKING_DIRECTORY "${WORK}"
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result STREQUAL "1" OR NOT stdout STREQUAL "" OR
    NOT stderr STREQUAL "slate: unknown option '${bad_option}'\n")
  message(FATAL_ERROR "unexpected diagnostic output (${result})\n${stdout}\n${stderr}")
endif()
run_at("${WORK}" "${BUILDER}" --help)
if(NOT run_output MATCHES "gen .*Generate a Ninja build" OR
    NOT run_output MATCHES "--targets")
  message(FATAL_ERROR "CLI help did not list the command and option descriptors")
endif()
run_at("${WORK}" "${BUILDER}" graph --format dot --help)
if(NOT run_output MATCHES "Usage: slate graph")
  message(FATAL_ERROR "CLI did not show graph help")
endif()
reject("--preset requires a value" "${BUILDER}" doctor --preset --tests)
file(MAKE_DIRECTORY "${WORK}/out/invalid-jsonc")
file(WRITE "${WORK}/out/invalid-jsonc/slate.json" "{ /* unterminated")
reject("unterminated block comment" "${BUILDER}" doctor
  --root "${WORK}/out/invalid-jsonc")
reject("unknown target 'not-present'" "${BUILDER}" doctor
  --targets test not-present --target math)
file(MAKE_DIRECTORY "${WORK}/other working directory")
file(WRITE "${WORK}/other working directory/.env" "SALTS_FIXTURE_CC=invalid\n")
run_at("${WORK}/other working directory" "${BUILDER}" doctor
  "--root=${WORK}/apps/../config/..")
if(NOT run_output MATCHES "environment: SALTS_FIXTURE_CC \\(.*\\.env:")
  message(FATAL_ERROR "CLI parsing changed which .env supplied the compiler:\n${run_output}")
endif()
reject("tool 'invalid' was not found" "${CMAKE_COMMAND}" -E env "SALTS_FIXTURE_CC=invalid"
  "${BUILDER}" doctor)
file(READ "${WORK}/.env" fixture_env)
file(WRITE "${WORK}/.env" "SALTS_FIXTURE_CMAKE=\"${CMAKE_COMMAND}\"\n")
reject("SALTS_FIXTURE_CC is required" "${CMAKE_COMMAND}" -E env --unset=SALTS_FIXTURE_CC
  "${BUILDER}" doctor)
file(WRITE "${WORK}/.env" "${fixture_env}")
set(io_manifest "${WORK}/modules/io/io-targets.json")
file(READ "${io_manifest}" io_contents)
string(REPLACE [=["requiredIncludeDirs": ["include"]]=]
  [=["requiredIncludeDirs": ["missing-include"]]=] missing_include "${io_contents}")
file(WRITE "${io_manifest}" "${missing_include}")
reject("requiredIncludeDirs: directory not found" "${BUILDER}" graph --target io)
file(WRITE "${io_manifest}" "${io_contents}")
reject("no usable toolchain" "${BUILDER}" doctor --compiler llvm-mingw)
reject("unknown target .*test" "${BUILDER}" gen --preset no-tests --targets test)
set(fixture_build_args --platform native --preset no-tests --tests
  --target test --build-dir "out/./debug/native/../native/native")
run_at("${WORK}" "${BUILDER}" build ${fixture_build_args})
set(out "${WORK}/out/debug/native/native")
file(READ "${out}/build.ninja" manifest)
set(fixture_compile fixture_compile)
if(driver STREQUAL "clang")
  set(fixture_compile fixture_clang_compile)
endif()
foreach(rule IN ITEMS "${fixture_compile}" fixture_archive fixture_task fixture_link)
  if(NOT manifest MATCHES "rule ${rule}\n" OR
      NOT manifest MATCHES "build [^\n]+: ${rule}( |\n)")
    message(FATAL_ERROR "module Ninja rule '${rule}' was not generated and used")
  endif()
endforeach()
if(manifest MATCHES "CTestTestfile[.]cmake")
  message(FATAL_ERROR "Slate generation still references a CTest manifest")
endif()
run_at("${WORK}" "${BUILDER}" test ${fixture_build_args} --list --label smoke)
if(NOT run_output STREQUAL "fixture.cpp\n")
  message(FATAL_ERROR "Slate did not list the declared smoke test\n${run_output}")
endif()
run_at("${WORK}" "${BUILDER}" test ${fixture_build_args} --label smoke)
if(NOT run_output MATCHES "PASS fixture[.]cpp")
  message(FATAL_ERROR "Slate did not report the passing smoke test\n${run_output}")
endif()
reject("no matching host-runnable tests" "${BUILDER}" test ${fixture_build_args} --label missing)
file(WRITE "${WORK}/apps/test/main.cpp" "int main() { return 7; }\n")
reject("FAIL fixture[.]cpp [(]exit 7[)]" "${BUILDER}" test ${fixture_build_args})
file(WRITE "${WORK}/apps/test/main.cpp" "${first_main}")
set(test_module "${WORK}/apps/test/module.json")
file(READ "${test_module}" smoke_module)
string(REPLACE [=["timeout": 10]=] [=["timeout": 1]=] slow_module "${smoke_module}")
string(REPLACE [=["-E", "env", "$target", "escaped quote \" then // comma ,] /* literal */",]=]
  [=["-E", "sleep", "3"]=] slow_module "${slow_module}")
file(WRITE "${test_module}" "${slow_module}")
reject("FAIL fixture[.]cpp [(]timed out after 1 s[)]" "${BUILDER}" test ${fixture_build_args})
file(WRITE "${test_module}" "${smoke_module}")
file(READ "${out}/compile_commands.json" database)
string(JSON commands LENGTH "${database}")
if(NOT commands EQUAL 4)
  message(FATAL_ERROR "expected four compilation database entries")
endif()
file(SHA256 "${out}/build.ninja" before_tool)
run_at("${WORK}/apps/test" "${BUILDER}" tool query --build-dir out/debug/native/native
  -- module/test)
if(NOT run_output MATCHES "module/test:" OR NOT run_output MATCHES "input: phony")
  message(FATAL_ERROR "Ninja query did not receive the selected target\n${run_output}")
endif()
run_at("${WORK}" "${BUILDER}" tool compdb --preset debug --platform native
  -- "${fixture_compile}")
string(JSON entries ERROR_VARIABLE parse_error LENGTH "${run_output}")
if(parse_error OR entries EQUAL 0)
  message(FATAL_ERROR "Ninja compdb did not produce raw JSON\n${run_output}")
endif()
run_at("${WORK}" "${CMAKE_COMMAND}" -E env "SALTS_FIXTURE_CC=invalid"
  "${BUILDER}" tool targets --build-dir "${out}" -- all)
if(NOT run_output MATCHES "module/test: phony")
  message(FATAL_ERROR "Ninja tool could not inspect an existing graph\n${run_output}")
endif()
file(SHA256 "${out}/build.ninja" after_tool)
if(NOT before_tool STREQUAL after_tool)
  message(FATAL_ERROR "Ninja tool unexpectedly rewrote the manifest")
endif()
file(WRITE "${WORK}/apps/test/main.cpp" "#error intentional build failure\n")
execute_process(COMMAND "${NINJA}" WORKING_DIRECTORY "${out}"
  RESULT_VARIABLE ninja_failure OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND "${BUILDER}" build ${fixture_build_args}
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
file(WRITE "${WORK}/apps/test/main.cpp" "${first_main}")
if(ninja_failure EQUAL 0 OR NOT result STREQUAL ninja_failure OR
    NOT "${stdout}\n${stderr}" MATCHES "intentional build failure")
  message(FATAL_ERROR "build did not propagate the Ninja failure (${result})\n${stdout}\n${stderr}")
endif()
file(WRITE "${WORK}/modules/io/number.json" "{\"value\": 18}\n")
set(expected 36)
string(CONFIGURE "${main}" second_main @ONLY)
file(WRITE "${WORK}/apps/test/main.cpp" "${second_main}")
run_at("${WORK}" "${BUILDER}" build ${fixture_build_args})
run_at("${WORK}" "${BUILDER}" test ${fixture_build_args})
file(TIMESTAMP "${out}/build.ninja" before "%s")
run_at("${out}" "${NINJA}" -d explain)
file(TIMESTAMP "${out}/build.ninja" after "%s")
if(NOT before STREQUAL after OR run_output MATCHES "preset:" OR
   NOT run_output MATCHES "no work to do")
  message(FATAL_ERROR "unchanged inputs caused regeneration or rebuilding\n${run_output}\n${run_error}")
endif()
file(APPEND "${WORK}/slate.json" "\n// Exercise regeneration from the root manifest.\n")
run_at("${out}" "${NINJA}")
if(NOT run_output MATCHES "preset: no-tests")
  message(FATAL_ERROR "Ninja did not regenerate after slate.json changed\n${run_output}")
endif()
file(READ "${TEMPLATE}" bundled_template)
file(MAKE_DIRECTORY "${WORK}/build/templates")
set(override "${WORK}/build/templates/build.ninja.j2")
foreach(marker "template override added" "template override updated")
  file(WRITE "${override}" "# ${marker}\n${bundled_template}")
  run_at("${out}" "${NINJA}")
  file(READ "${out}/build.ninja" manifest)
  if(NOT manifest MATCHES "^# ${marker}")
    message(FATAL_ERROR "Ninja did not regenerate after changing the template")
  endif()
endforeach()
file(REMOVE "${override}")
run_at("${out}" "${NINJA}")
file(READ "${out}/build.ninja" manifest)
if(manifest MATCHES "^# template override")
  message(FATAL_ERROR "Ninja did not fall back to the bundled template")
endif()
if(MSVC_DRIVER)
  set(link_template link_msvc.json.j2)
  set(link_flag /INCREMENTAL:NO)
  set(compile_template compile_msvc.json.j2)
  set(compile_flag /DFIXTURE_OVERRIDE=1)
  set(archive_template archive_msvc.json.j2)
  set(archive_default /nologo)
  set(archive_flag /NOLOGO)
elseif(os STREQUAL "windows")
  set(link_template link_mingw.json.j2)
elseif(os STREQUAL "macos")
  set(link_template link_macho.json.j2)
else()
  set(link_template link_elf.json.j2)
endif()
if(NOT MSVC_DRIVER)
  set(link_flag -v)
  set(compile_template compile_gnu.json.j2)
  set(compile_flag -DFIXTURE_OVERRIDE=1)
  set(archive_template archive_gnu.json.j2)
  set(archive_default rcs)
  set(archive_flag -rcs)
endif()
get_filename_component(template_dir "${TEMPLATE}" DIRECTORY)
if(CMAKE_HOST_WIN32)
  set(argument_quote "\"")
else()
  set(argument_quote "'")
endif()
foreach(kind IN ITEMS link compile archive)
  set(filename "${${kind}_template}")
  set(flag "${${kind}_flag}")
  set(quoted_flag "${argument_quote}${flag}${argument_quote}")
  file(READ "${template_dir}/${filename}" bundled_arguments)
  set(argument_override "${WORK}/build/templates/${filename}")
  if(kind STREQUAL "archive")
    string(REPLACE "\"${archive_default}\"" "\"${flag}\"" arguments "${bundled_arguments}")
  else()
    string(SUBSTRING "${bundled_arguments}" 1 -1 argument_tail)
    set(arguments "[\"${flag}\",${argument_tail}")
  endif()
  file(WRITE "${argument_override}" "${arguments}")
  run_at("${out}" "${NINJA}")
  file(READ "${out}/build.ninja" manifest)
  string(FIND "${manifest}" "${quoted_flag}" flag_offset)
  if(flag_offset EQUAL -1)
    message(FATAL_ERROR "Ninja did not regenerate after adding ${filename}")
  endif()
  file(WRITE "${argument_override}" "{}")
  reject("expected an array" "${BUILDER}" gen --targets test --build-dir "${out}")
  file(REMOVE "${argument_override}")
  run_at("${out}" "${NINJA}")
  file(READ "${out}/build.ninja" manifest)
  string(FIND "${manifest}" "${quoted_flag}" flag_offset)
  if(NOT flag_offset EQUAL -1)
    message(FATAL_ERROR "Ninja kept options from removed override ${filename}")
  endif()
endforeach()
run_at("${WORK}" "${BUILDER}" test ${fixture_build_args})

set(custom_out "${WORK}/out/custom-templates")
set(ninja_override "${WORK}/custom templates/build.ninja.j2")
set(artifact_override "${WORK}/custom templates/artifact-names.json.j2")
file(MAKE_DIRECTORY "${WORK}/custom templates")
file(REMOVE "${artifact_override}")
file(READ "${TEMPLATE}" custom_ninja)
file(WRITE "${ninja_override}" "${custom_ninja}\n# custom Ninja override\n")
set(custom_build_args --preset custom-templates --target test --build-dir "${custom_out}")
run_at("${WORK}" "${BUILDER}" gen ${custom_build_args})
file(READ "${custom_out}/build.ninja" manifest)
if(NOT manifest MATCHES "# custom Ninja override")
  message(FATAL_ERROR "templateDir did not select the Ninja override")
endif()
file(REMOVE "${ninja_override}")
run_at("${custom_out}" "${NINJA}")
file(READ "${custom_out}/build.ninja" manifest)
if(manifest MATCHES "# custom Ninja override")
  message(FATAL_ERROR "Ninja did not fall back to the bundled build template")
endif()
run_at("${WORK}" "${BUILDER}" test ${custom_build_args})

file(READ "${template_dir}/artifact-names.json.j2" artifact_template)
set(exe_suffix ".custom")
if(os STREQUAL "windows")
  set(exe_suffix ".custom.exe")
endif()
set(object_suffix ".custom.o")
if(MSVC_DRIVER)
  set(object_suffix ".custom.obj")
endif()
string(REGEX REPLACE "\"executableSuffix\"[^\n]*"
  "\"executableSuffix\": \"${exe_suffix}\"," artifact_template "${artifact_template}")
string(REGEX REPLACE "\"objectSuffix\"[^\n]*"
  "\"objectSuffix\": \"${object_suffix}\"," artifact_template "${artifact_template}")
string(REPLACE "\".rsp\"" "\".custom.rsp\"" artifact_template "${artifact_template}")
file(WRITE "${artifact_override}" "${artifact_template}")
run_at("${custom_out}" "${NINJA}")
file(READ "${custom_out}/build.ninja" manifest)
string(FIND "${manifest}" "/bin/test${exe_suffix}" custom_exe)
string(FIND "${manifest}" "0${object_suffix}.custom.rsp" custom_response)
if(custom_exe EQUAL -1 OR custom_response EQUAL -1)
  message(FATAL_ERROR "artifact naming override did not regenerate all paths")
endif()
run_at("${WORK}" "${BUILDER}" test ${custom_build_args})
file(REMOVE "${artifact_override}")
run_at("${custom_out}" "${NINJA}")
file(READ "${custom_out}/build.ninja" manifest)
string(FIND "${manifest}" "/bin/test${exe_suffix}" custom_exe)
if(NOT custom_exe EQUAL -1)
  message(FATAL_ERROR "Ninja did not restore default artifact names")
endif()

run_at("${WORK}" "${BUILDER}" graph --target test)
if(NOT run_output MATCHES [=["math" -> "io" \[label="private"\]]=])
  message(FATAL_ERROR "private module dependency was missing from graph")
endif()
if(NOT run_output MATCHES [=[  subgraph "cluster_modules"]=] OR
   NOT run_output MATCHES [=[    subgraph "cluster_modules/math"]=])
  message(FATAL_ERROR "graph did not group targets in nested module folders")
endif()
run_at("${WORK}" "${BUILDER}" graph --format json --target test)
string(JSON graph_target GET "${run_output}" targets 0 name)
string(JSON graph_folder GET "${run_output}" targets 0 folder)
string(JSON graph_manifest GET "${run_output}" targets 0 manifest)
string(JSON graph_source GET "${run_output}" targets 0 sources 0)
string(REGEX REPLACE "/module[.]json$" "/main.cpp" expected_graph_source "${graph_manifest}")
string(JSON graph_runnable GET "${run_output}" targets 0 runnable)
string(JSON graph_test GET "${run_output}" targets 0 test)
string(JSON graph_dep GET "${run_output}" targets 0 privateDeps 0)
if(NOT graph_target STREQUAL "test" OR NOT graph_folder STREQUAL "apps/test" OR
    NOT graph_source STREQUAL expected_graph_source OR
    NOT graph_runnable OR NOT graph_test OR NOT graph_dep STREQUAL "shared")
  message(FATAL_ERROR "JSON graph did not describe the executable test target\n${run_output}")
endif()
reject("not a host-runnable executable" "${BUILDER}" run --target math)
reject("run requires exactly one --target" "${BUILDER}" run)
file(READ "${WORK}/modules/math/module.json" math_module)
string(JSON hidden_math SET "${math_module}" visibility "[]")
file(WRITE "${WORK}/modules/math/module.json" "${hidden_math}")
reject("is not visible" "${BUILDER}" gen --targets test)
string(JSON cyclic_math SET "${math_module}" private deps [=[["math"]]=])
file(WRITE "${WORK}/modules/math/module.json" "${cyclic_math}")
reject("dependency cycle" "${BUILDER}" gen --targets test)
file(WRITE "${WORK}/modules/math/module.json" "${math_module}")

# Add a child manifest to an existing build and expand its file group.
set(checks "${WORK}/modules/math/checks")
file(MAKE_DIRECTORY "${checks}/directory_case.c" "${checks}/nested")
file(REMOVE "${checks}/new_case.c")
set(group_out "${WORK}/out/file-group")
run_at("${WORK}" "${BUILDER}" gen --build-dir "${group_out}")
string(JSON math_with_empty_module SET "${math_module}" modules [=[[""]]=])
file(WRITE "${WORK}/modules/math/module.json" "${math_with_empty_module}")
reject("modules contains an empty directory path" "${BUILDER}" graph)
string(JSON math_with_checks SET "${math_module}" modules [=[["checks"]]=])
file(REMOVE "${checks}/module.json")
file(WRITE "${WORK}/modules/math/module.json" "${math_with_checks}")
reject("must be a module manifest file in the repository" "${BUILDER}" graph)
file(WRITE "${checks}/module.json" [=[{"include":["../module.json"]}]=])
reject("module include cycle" "${BUILDER}" graph)
file(WRITE "${checks}/module.json" [=[
{
  "modules": ["nested"],
  "forEach": [{
    "files": ["seed_case.c", "*_case.c"],
    "exclude": ["excluded_*.c"],
    "namePrefix": "fixture_",
    "target": {
      "kind": "executable",
      "test": { "namePrefix": "fixture.pattern.", "timeout": 10 }
    }
  }],
  "targets": {
    "release_only": {
      "kind": "executable",
      "when": { "config": "release" },
      "sources": { "files": ["release_only.c"] }
    }
  }
}
]=])
file(WRITE "${checks}/nested/module.json" [=[{"kind":"header_only"}]=])
file(WRITE "${checks}/seed_case.c" "int main(void) { return 0; }\n")
file(WRITE "${checks}/excluded_case.c" "#error This fixture is excluded\n")
string(JSON math_with_duplicate SET "${math_with_checks}" include 2
  [=["checks/./module.json"]=])
file(WRITE "${WORK}/modules/math/module.json" "${math_with_duplicate}")
run_at("${WORK}" "${BUILDER}" graph --target fixture_seed_case)
file(WRITE "${WORK}/modules/math/module.json" "${math_with_checks}")
reject("unknown target .*release_only" "${BUILDER}" gen --target release_only)
run_at("${group_out}" "${NINJA}")
file(READ "${group_out}/build.ninja" manifest)
if(NOT manifest MATCHES "module/fixture_seed_case" OR
   NOT manifest MATCHES "module/nested")
  message(FATAL_ERROR "Ninja did not regenerate with the nested module directories")
endif()
set(group_test_args --build-dir "${group_out}" --filter fixture.pattern.)
run_at("${WORK}" "${BUILDER}" test ${group_test_args})
if(NOT run_output MATCHES "fixture[.]pattern[.]fixture_seed_case" OR
   NOT run_output MATCHES "1 passed, 0 failed")
  message(FATAL_ERROR "expected one generated test\n${run_output}")
endif()
file(WRITE "${checks}/new_case.c" "int main(void) { return 0; }\n")
run_at("${group_out}" "${NINJA}")
file(READ "${group_out}/build.ninja" manifest)
if(NOT manifest MATCHES "module/fixture_new_case")
  message(FATAL_ERROR "Ninja did not regenerate after adding a matching file")
endif()
run_at("${WORK}" "${BUILDER}" test ${group_test_args})
if(NOT run_output MATCHES "fixture[.]pattern[.]fixture_new_case" OR
   NOT run_output MATCHES "2 passed, 0 failed")
  message(FATAL_ERROR "Ninja did not pick up a new matching file\n${run_output}")
endif()
file(REMOVE "${checks}/new_case.c")
run_at("${group_out}" "${NINJA}")
file(READ "${group_out}/build.ninja" manifest)
if(manifest MATCHES "fixture_new_case")
  message(FATAL_ERROR "Ninja did not regenerate after removing a matching file")
endif()
run_at("${WORK}" "${BUILDER}" test ${group_test_args})
if(run_output MATCHES "fixture[.]pattern[.]fixture_new_case" OR
   NOT run_output MATCHES "1 passed, 0 failed")
  message(FATAL_ERROR "Ninja kept the test for a removed matching file\n${run_output}")
endif()

# A module can have more actions than Jinja's default node workspace.
file(MAKE_DIRECTORY "${WORK}/apps/scale")
set(sources "")
foreach(i RANGE 1 1100)
  set(source "sample_${i}.c")
  file(WRITE "${WORK}/apps/scale/${source}" "int sample(void);\n")
  list(APPEND sources "\"${source}\"")
endforeach()
list(JOIN sources "," file_list)
file(WRITE "${WORK}/apps/scale/module.json"
  "{\"kind\":\"static\",\"sources\":{\"files\":[${file_list}]}}")
string(JSON math_with_scale SET "${math_with_checks}" include 2
  [=["../../apps/scale/module.json"]=])
file(WRITE "${WORK}/modules/math/module.json" "${math_with_scale}")
set(scale_out "${WORK}/out/template-scale")
run_at("${WORK}" "${BUILDER}" gen --targets scale --build-dir "${scale_out}")
file(WRITE "${WORK}/modules/math/module.json" "${math_with_checks}")
run_at("${scale_out}" "${NINJA}" -t targets all)
file(READ "${scale_out}/compile_commands.json" database)
string(JSON edge_count LENGTH "${database}")
if(NOT edge_count EQUAL 1100)
  message(FATAL_ERROR "large fixture did not generate all 1100 compilation edges")
endif()

# Exercise the generated options for foreign targets without invoking a linker.
if(driver STREQUAL "clang")
  if(render_arch STREQUAL "x64")
    set(apple_arch x86_64)
  else()
    set(apple_arch arm64)
  endif()
  foreach(platform IN ITEMS macos linux)
    set(render_out "${WORK}/out/render-${platform}-triple")
    run_at("${WORK}" "${BUILDER}" gen --platform "render-${platform}"
      --targets shared --build-dir "${render_out}")
    file(READ "${render_out}/build.ninja" manifest)
    string(REGEX MATCH "build [^\n]+: link [^\n]*\n  run = [^\n]*\n  response = [^\n]*\n  argv = [^\n]*"
      link_edge "${manifest}")
    if(platform STREQUAL "macos")
      set(expected_options "\"-arch\" \"${apple_arch}\"" "\"-dynamiclib\""
        "\"-Wl,-install_name,@rpath/libshared.dylib\""
        "\"-Wl,-rpath,@loader_path/../lib\"")
    else()
      set(expected_options "\"-shared\"" "\"-Wl,-soname,libshared.so\""
        "\"-Wl,-rpath,$$ORIGIN:$$ORIGIN/../lib\"")
    endif()
    foreach(option IN LISTS expected_options)
      string(REPLACE "\"" "${argument_quote}" option "${option}")
      string(FIND "${link_edge}" "${option}" option_offset)
      if(option_offset EQUAL -1)
        message(FATAL_ERROR "${platform} response is missing ${option}")
      endif()
    endforeach()
  endforeach()
  string(JSON wrong_target REMOVE "${toolchain}" toolchains render-linux triple)
  file(WRITE "${WORK}/config/native.json" "${wrong_target}")
  reject("reports target .* for linux/${render_arch}" "${BUILDER}" doctor
    --platform render-linux)
  file(WRITE "${WORK}/config/native.json" "${toolchain}")
endif()

string(JSON native_candidate GET "${toolchain}" platforms native)
string(JSON two_native SET "${toolchain}" platforms native-alternate "${native_candidate}")
file(WRITE "${WORK}/config/native.json" "${two_native}")
run_at("${WORK}" "${BUILDER}" doctor --platform native-alternate)
if(NOT run_output MATCHES "target: native-alternate / native")
  message(FATAL_ERROR "the explicit native platform was not selected")
endif()
reject("more than one platform matches" "${BUILDER}" doctor)
file(WRITE "${WORK}/config/native.json" "${toolchain}")

# A tool change must reach Ninja's locked identity check through _env.
set(identity_root "${WORK}/out/identity-fixture")
set(identity_out "${identity_root}/build-output")
file(MAKE_DIRECTORY "${identity_root}/app" "${identity_root}/tools")
get_filename_component(ar_name "${AR}" NAME)
set(identity_ar "tools/${ar_name}")
file(COPY_FILE "${AR}" "${identity_root}/${identity_ar}")
file(CHMOD "${identity_root}/${identity_ar}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(REMOVE "${identity_out}/toolchains.json")
string(JSON identity_tools SET "${toolchain}" toolchains native ar "\"${identity_ar}\"")
file(WRITE "${identity_root}/tools.json" "${identity_tools}")
file(WRITE "${identity_root}/.env" "SALTS_FIXTURE_CC=\"${cc}\"\n")
set(identity_config [=[
{"include":["tools.json"],"defaultPreset":"debug",
 "presets":{"debug":{"config":"debug"}}}
]=])
file(WRITE "${identity_root}/app/targets.json" [=[{"kind":"header_only"}]=])
file(WRITE "${identity_root}/module.json" "This file has no declaration in slate.json.\n")
foreach(undeclared IN ITEMS apps modules third_party)
  file(MAKE_DIRECTORY "${identity_root}/${undeclared}")
  file(WRITE "${identity_root}/${undeclared}/module.json" "This file has no declaration in slate.json.\n")
endforeach()
string(JSON identity_files SET "${identity_config}" moduleFiles [=[["app/targets.json"]]=])
file(WRITE "${identity_root}/slate.json" "${identity_files}")
run_at("${identity_root}" "${BUILDER}" graph --target app)
file(WRITE "${identity_root}/apps/duplicate.json"
  [=[{"kind":"header_only","name":"app","when":{"config":"other"}}]=])
string(JSON duplicate_name SET "${identity_files}" moduleFiles 1 [=["apps/duplicate.json"]=])
file(WRITE "${identity_root}/slate.json" "${duplicate_name}")
reject("duplicate target name 'app'" "${BUILDER}" graph --root "${identity_root}" --target app)
file(WRITE "${identity_root}/slate.json" "${identity_files}")
run_at("${identity_root}" "${BUILDER}" gen --targets app --build-dir "${identity_out}")
run_at("${identity_out}" "${NINJA}")
run_at("${identity_root}" "${CMAKE_COMMAND}" -E sleep 1.1)
file(TOUCH "${identity_root}/${identity_ar}")
reject("toolchain identity changed" "${NINJA}" -C "${identity_out}")

# Exercise install from a fresh graph, including generated headers and runtime dependencies.
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef install_run)
set(install_root "${WORK}/out/install fixture ${install_run}")
set(install_out "${install_root}/output")
file(MAKE_DIRECTORY "${install_root}/apps/demo/assets" "${install_root}/modules/helpers/include/api"
  "${install_root}/modules/runtime" "${install_root}/modules/other" "${install_root}/tools")
file(WRITE "${install_root}/tools.json" "${toolchain}")
file(WRITE "${install_root}/.env" "SALTS_FIXTURE_CC=\"${cc}\"\n")
file(WRITE "${install_root}/slate.json" [=[
{"include":["tools.json"],"defaultPreset":"debug",
 "presets":{"debug":{"config":"debug","options":{"debugInfo":"full"}}},
 "moduleFiles":["apps/demo/module.json","modules/runtime/module.json",
                "modules/helpers/module.json","modules/other/module.json","tools/module.json"]}
]=])
file(WRITE "${install_root}/apps/demo/module.json" [=[
{
  "kind": "executable",
  "sources": {"files":["main.c"]},
  "private": {"deps":["runtime"]},
  "install": {"assets":[{"from":"assets/*.txt","to":"share/demo"}]},
  "targets": {
    "test_helper": {"kind":"executable","testOnly":true,
             "sources":{"files":["unrelated.c"]}}
  }
}
]=])
file(WRITE "${install_root}/apps/demo/main.c" [=[
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
__declspec(dllimport)
#endif
int runtime_value(void);
int main(int argc, char **argv) {
  if (runtime_value() != 42) return 1;
  if (argc == 1) return 0;
  if (argc != 2 || strcmp(argv[1], "--run flag")) return 7;
  puts("ran selected target");
  return 0;
}
]=])
file(WRITE "${install_root}/apps/demo/unrelated.c" "int main(void) { return 0; }\n")
file(WRITE "${install_root}/apps/demo/assets/readme.txt" "runtime asset\n")
file(WRITE "${install_root}/modules/runtime/module.json" [=[
{
  "kind": "shared",
  "sources": {"files":["runtime.c"]},
  "runtimeBins": ["runtime.dat"]
}
]=])
file(WRITE "${install_root}/modules/runtime/runtime.c" [=[
#ifdef _WIN32
__declspec(dllexport)
#endif
int runtime_value(void) { return 42; }
]=])
file(WRITE "${install_root}/modules/runtime/runtime.dat" "extra runtime file\n")
file(WRITE "${install_root}/modules/helpers/module.json" [=[
{
  "kind": "static",
  "sources": {"files":["helpers.c"]},
  "public": {"includeDirs":["include","$builddir/gen"],"defines":{"HELPERS_ABI":"3"}},
  "private": {"includeDirs":["private"]},
  "options": {"cStd":"c17"},
  "variants": [{"when":{"config":"debug"},"options":{"cStd":"c11"}}],
  "install": {"headers":[
    {"from":"include","to":"include"},
    {"from":"$builddir/gen/*.h","to":"include"}
  ], "exportTarget":true,"version":"1.2.3"},
  "preTasks": [{
    "name":"generate", "builtin":"copy",
    "inputs":["generated.h.in"], "outputs":["$builddir/gen/generated.h"]
  }]
}
]=])
file(WRITE "${install_root}/modules/helpers/helpers.c" "int helper(void) { return 1; }\n")
file(WRITE "${install_root}/modules/helpers/local.h" "int helper(void);\n")
file(WRITE "${install_root}/modules/helpers/include/api/helpers.h" "int helper(void);\n")
file(WRITE "${install_root}/modules/helpers/private/impl.hpp" "int helper(void);\n")
file(WRITE "${install_root}/modules/helpers/generated.h.in" "#define GENERATED 1\n")
file(WRITE "${install_root}/modules/other/module.json" [=[
{"kind":"executable","sources":{"files":["unused.c"]}}
]=])
file(WRITE "${install_root}/modules/other/unused.c" "#error target selection failed\n")
file(WRITE "${install_root}/tools/module.json" [=[
{"kind":"executable","toolchainRole":"host","sources":{"files":["unused.c"]}}
]=])
file(WRITE "${install_root}/tools/unused.c" "#error host tool was built\n")

run_at("${install_root}" "${BUILDER}" graph --format json --target helpers
  --build-dir "${install_out}")
string(JSON graph_headers GET "${run_output}" targets 0 headers)
string(JSON graph_header_count LENGTH "${graph_headers}")
string(JSON graph_task_input GET "${run_output}" targets 0 taskInputs 0)
string(JSON graph_task_output GET "${run_output}" targets 0 taskOutputs 0)
if(NOT graph_header_count EQUAL 4 OR NOT graph_headers MATCHES "/local[.]h\"" OR
    NOT graph_headers MATCHES "/include/api/helpers[.]h\"" OR
    NOT graph_headers MATCHES "/private/impl[.]hpp\"" OR
    NOT graph_headers MATCHES "/gen/generated[.]h\"")
  message(FATAL_ERROR "JSON graph did not describe the module headers\n${graph_headers}")
endif()
if(NOT graph_task_input MATCHES "/generated[.]h[.]in$" OR
    NOT graph_task_output MATCHES "/gen/generated[.]h$")
  message(FATAL_ERROR "JSON graph did not describe the module task files\n${run_output}")
endif()

set(install_args --build-dir "${install_out}/" --tests --targets
  demo runtime helpers test_helper)
run_at("${install_root}" "${CMAKE_COMMAND}" -E env "SLATE_CONSOLE=1" "NO_COLOR=1"
  "${BUILDER}" run --target demo --build-dir "run output"
  -- "--run flag")
if(NOT run_output MATCHES "ran selected target")
  message(FATAL_ERROR "run did not pass the program arguments\n${run_output}")
endif()
if(NOT run_output MATCHES "\\[Slate\\] Run demo" OR
   NOT run_output MATCHES "\\[Slate\\] Run succeeded \\(exit 0\\)")
  message(FATAL_ERROR "run did not report its console status\n${run_output}")
endif()
execute_process(COMMAND "${BUILDER}" run --target demo --build-dir "run output" -- bad
  WORKING_DIRECTORY "${install_root}" RESULT_VARIABLE program_result OUTPUT_QUIET)
if(NOT program_result EQUAL 7)
  message(FATAL_ERROR "run did not propagate the executable exit code (${program_result})")
endif()
set(runtime_stage "${install_root}/runtime stage")
run_at("${install_root}/apps/demo" "${BUILDER}" install ${install_args}
  --components runtime --prefix "runtime stage/")
set(exe "")
set(runtime_dir lib)
if(os STREQUAL "windows")
  set(exe ".exe")
  set(runtime_dir bin)
endif()
run_at("${runtime_stage}/bin" "${runtime_stage}/bin/demo${exe}")
if(NOT EXISTS "${runtime_stage}/share/demo/readme.txt" OR
    NOT EXISTS "${runtime_stage}/${runtime_dir}/runtime.dat" OR
    EXISTS "${runtime_stage}/bin/test_helper${exe}" OR
    EXISTS "${runtime_stage}/include" OR EXISTS "${install_out}/gen/generated.h" OR
    EXISTS "${runtime_stage}/symbols")
  message(FATAL_ERROR "runtime installation did not respect its component and target set")
endif()

set(dev_stage "${install_root}/dev stage")
run_at("${install_root}" "${BUILDER}" install ${install_args}
  --components dev --prefix "dev stage")
if(MSVC_DRIVER)
  set(archive helpers.lib)
  set(import runtime.lib)
else()
  set(archive libhelpers.a)
  set(import libruntime.dll.a)
endif()
if(NOT EXISTS "${dev_stage}/lib/${archive}" OR
    NOT EXISTS "${dev_stage}/include/api/helpers.h" OR
    NOT EXISTS "${dev_stage}/include/generated.h" OR
    EXISTS "${dev_stage}/bin" OR EXISTS "${dev_stage}/share/demo" OR
    (os STREQUAL "windows" AND NOT EXISTS "${dev_stage}/lib/${import}"))
  message(FATAL_ERROR "dev installation is missing files or contains runtime files")
endif()

run_at("${install_out}" "${NINJA}" install)
set(default_stage "${install_out}/stage")
if(NOT EXISTS "${default_stage}/include/generated.h" OR
    (MSVC_DRIVER AND NOT EXISTS "${default_stage}/symbols/demo.exe.pdb"))
  message(FATAL_ERROR "Ninja install did not install all components")
endif()
run_at("${default_stage}/bin" "${default_stage}/bin/demo${exe}")
run_at("${install_out}" "${NINJA}" -t clean)
run_at("${default_stage}/bin" "${default_stage}/bin/demo${exe}")
file(REMOVE "${default_stage}/share/demo/readme.txt")
run_at("${install_out}" "${NINJA}" install)
if(NOT EXISTS "${default_stage}/share/demo/readme.txt")
  message(FATAL_ERROR "Ninja install did not restore a removed installed file")
endif()

# The installed manifest must suffice to build a downstream project after relocation.
set(relocated "${install_root}/relocated sdk")
file(RENAME "${dev_stage}" "${relocated}")
set(sdk_manifest "${relocated}/share/slate/helpers/pkg.json")
if(NOT EXISTS "${sdk_manifest}" OR
    NOT EXISTS "${relocated}/lib/pkgconfig/helpers.pc")
  message(FATAL_ERROR "install dev did not export SDK metadata")
endif()
file(READ "${sdk_manifest}" sdk_json)
string(JSON sdk_version GET "${sdk_json}" version)
if(NOT sdk_version STREQUAL "1.2.3" OR sdk_json MATCHES "install fixture")
  message(FATAL_ERROR "SDK manifest is not relocatable or lost its version")
endif()
file(READ "${install_out}/compile_commands.json" sdk_compile)
if(NOT sdk_compile MATCHES "std[:=]c11")
  message(FATAL_ERROR "matching variant did not apply its C standard")
endif()
set(consumer "${install_root}/consumer")
file(MAKE_DIRECTORY "${consumer}/deps/helpers" "${consumer}/app")
file(COPY "${install_root}/.env" "${install_root}/tools.json" DESTINATION "${consumer}")
file(WRITE "${consumer}/slate.json" [=[
{"include":["tools.json"],"defaultPreset":"debug",
 "presets":{"debug":{"config":"debug"}},
 "moduleFiles":["app/module.json","deps/helpers/module.json"]}
]=])
file(WRITE "${consumer}/deps/helpers/module.json" [=[
{"kind":"external","package":{
  "provider":"salts","file":"../../../relocated sdk/share/slate/helpers/pkg.json"}}
]=])
file(WRITE "${consumer}/app/module.json" [=[
{"kind":"executable","sources":{"files":["main.c"]},"private":{"deps":["helpers"]}}
]=])
file(WRITE "${consumer}/app/main.c" [=[
#include "api/helpers.h"
#include "generated.h"
int main(void) { return helper() == 1 && GENERATED == 1 && HELPERS_ABI == 3 ? 0 : 1; }
]=])
run_at("${consumer}" "${BUILDER}" build --target app --build-dir sdk-build)
run_at("${consumer}/sdk-build/bin" "${consumer}/sdk-build/bin/app${exe}")

find_program(pkgconf NAMES pkgconf)
if(pkgconf)
  run_at("${relocated}" "${CMAKE_COMMAND}" -E env
    "PKG_CONFIG_LIBDIR=${relocated}/lib/pkgconfig" "PKG_CONFIG_PATH="
    "${pkgconf}" --modversion helpers)
  if(NOT run_output MATCHES "^1[.]2[.]3")
    message(FATAL_ERROR "pkgconf did not read the installed .pc metadata")
  endif()
endif()

# The vcpkg provider uses only the selected triplet/configuration, including runtime files from .list.
set(tree "${consumer}/sdk/fixture-triplet")
file(MAKE_DIRECTORY "${tree}/debug/lib/pkgconfig" "${tree}/lib/pkgconfig"
  "${tree}/debug/${runtime_dir}" "${consumer}/sdk/vcpkg/info")
file(COPY "${relocated}/include" DESTINATION "${tree}")
file(COPY "${relocated}/lib/${archive}" DESTINATION "${tree}/debug/lib")
if(os STREQUAL "windows")
  set(runtime_file runtime.dll)
elseif(os STREQUAL "macos")
  set(runtime_file libruntime.dylib)
else()
  set(runtime_file libruntime.so)
endif()
file(COPY "${runtime_stage}/${runtime_dir}/${runtime_file}"
  DESTINATION "${tree}/debug/${runtime_dir}")
file(WRITE "${consumer}/sdk/vcpkg/info/helpers_1.2.3_fixture-triplet.list"
  "fixture-triplet/include/api/helpers.h\nfixture-triplet/debug/lib/${archive}\n")
file(WRITE "${consumer}/sdk/vcpkg/info/runtime-support_1_fixture-triplet.list"
  "fixture-triplet/debug/${runtime_dir}/${runtime_file}\n")
file(WRITE "${consumer}/sdk/vcpkg/status"
  "Package: helpers\nDepends: runtime-support, host-tool:other-triplet\nArchitecture: fixture-triplet\nStatus: install ok installed\n")
file(WRITE "${consumer}/deps/helpers/module.json" [=[
{"kind":"prebuilt","package":{"provider":"vcpkg","name":"helpers"},
 "public":{"defines":{"HELPERS_ABI":"3"}}}
]=])
run_at("${consumer}" "${BUILDER}" build --target app --build-dir list-build)
run_at("${consumer}/list-build/bin" "${consumer}/list-build/bin/app${exe}")
if(NOT EXISTS "${consumer}/list-build/${runtime_dir}/${runtime_file}")
  message(FATAL_ERROR "vcpkg .list runtime library was not staged")
endif()

if(pkgconf)
  file(WRITE "${tree}/debug/lib/pkgconfig/helpers.pc" [=[
prefix=${pcfiledir}/../../..
Name: helpers
Description: helpers fixture
Version: 1.2.3
Cflags: "-I${prefix}/include" -DHELPERS_ABI=3
Libs: "-L${prefix}/debug/lib" -lhelpers
]=])
  file(WRITE "${tree}/lib/pkgconfig/helpers.pc" [=[
Name: helpers
Description: wrong configuration
Version: 0.0.0
Cflags: -DHELPERS_ABI=0
Libs: -lmissing_release_library
]=])
  file(WRITE "${consumer}/deps/helpers/module.json" [=[
{"kind":"external","package":{"provider":"vcpkg","name":"helpers","static":true}}
]=])
  run_at("${consumer}" "${BUILDER}" build --target app --build-dir pc-build)
  run_at("${consumer}/pc-build/bin" "${consumer}/pc-build/bin/app${exe}")
  file(READ "${consumer}/pc-build/build.ninja" pc_ninja)
  if(NOT pc_ninja MATCHES "(-L|/LIBPATH:).*debug/lib")
    message(FATAL_ERROR "the linker template did not emit the vcpkg libDirs")
  endif()
endif()

file(MAKE_DIRECTORY "${consumer}/deps/legacy")
file(WRITE "${consumer}/deps/legacy/module.json" [=[
{"kind":"static","sources":{"files":["legacy.cpp"]},"options":{"cxxStd":"c++14"}}
]=])
file(WRITE "${consumer}/deps/legacy/legacy.cpp" "int legacy() { return 0; }\n")
file(READ "${consumer}/slate.json" consumer_presets)
string(JSON consumer_presets SET "${consumer_presets}" moduleFiles 2
  [=["deps/legacy/module.json"]=])
file(WRITE "${consumer}/slate.json" "${consumer_presets}")
file(WRITE "${consumer}/app/module.json" [=[
{
 "kind":"executable","sources":{"files":["main.cpp"]},"private":{"deps":["legacy"]},
 "options":{"cxxStd":"c++17"},
 "variants":[{"when":{"config":"strict"},"options":{"abiCritical":true}}]
}
]=])
file(WRITE "${consumer}/app/main.cpp" "int main() { return 0; }\n")
run_at("${consumer}" "${BUILDER}" gen --target app --build-dir abi-build)
file(READ "${consumer}/abi-build/compile_commands.json" abi_compile)
if(NOT abi_compile MATCHES "std[:=]c[+][+]14" OR NOT abi_compile MATCHES "std[:=]c[+][+]17")
  message(FATAL_ERROR "module C++ standard override did not reach both compilers")
endif()
string(JSON consumer_presets SET "${consumer_presets}" presets strict [[{"config":"strict"}]])
file(WRITE "${consumer}/slate.json" "${consumer_presets}")
reject("incompatible cxxStd" "${BUILDER}" gen --root "${consumer}"
  --target app --preset strict --build-dir abi-strict)

reject("unknown install component" "${BUILDER}" install --components unknown)
file(WRITE "${install_root}/modules/other/module.json" [=[
{
  "kind":"header_only",
  "install":{"headers":[{"from":"unused.c","to":"../escape"}]}
}
]=])
reject("install destination escapes the prefix" "${BUILDER}" install
  --root "${install_root}" --target other)

# Two projects, including a nested root, share one library manifest tree.
set(workspace "${WORK}/library workspace")
set(shared_libs "${workspace}/libs")
set(outer_project "${workspace}/app")
set(inner_project "${outer_project}/tools/app")
file(MAKE_DIRECTORY "${shared_libs}/include")
file(WRITE "${shared_libs}/shared.json" [[{"include":["http.json"]}]])
set(shared_manifest [=[
{
  "name":"shared_http", "kind":"static", "sources":{"files":["http.c"]},
  "public":{"requiredIncludeDirs":["include"]},
  "variants":[
    {"when":{"config":"debug"},"private":{"defines":{"VALUE":"41"}}},
    {"when":{"config":"release"},"private":{"defines":{"VALUE":"42"}}}
  ]
}
]=])
file(WRITE "${shared_libs}/http.json" "${shared_manifest}")
file(WRITE "${shared_libs}/include/http.h" "int shared_http(void);\n")
file(WRITE "${shared_libs}/http.c" "#include <http.h>\nint shared_http(void) { return VALUE; }\n")
foreach(project IN ITEMS "${outer_project}" "${inner_project}")
  file(MAKE_DIRECTORY "${project}/config" "${project}/src")
  file(COPY "${WORK}/config/native.json" DESTINATION "${project}/config")
  file(COPY "${WORK}/.env" DESTINATION "${project}")
  file(RELATIVE_PATH library "${project}" "${shared_libs}/shared.json")
  if(project STREQUAL outer_project)
    set(configuration debug)
    set(expected 41)
  else()
    set(configuration release)
    set(expected 42)
  endif()
  file(WRITE "${project}/config/libraries.json" "{\"libraries\":[\"../${library}\"]}")
  file(WRITE "${project}/slate.json" "{
    \"include\":[\"config/native.json\",\"config/libraries.json\"],
    \"libraries\":[\"${library}\"],\"modules\":[\"src\"],
    \"defaultPreset\":\"${configuration}\",
    \"presets\":{\"${configuration}\":{\"config\":\"${configuration}\"}}}")
  file(WRITE "${project}/src/module.json" [=[
    {"name":"app","kind":"executable","sources":{"files":["main.c"]},
     "private":{"deps":["shared_http"]}}
  ]=])
  file(WRITE "${project}/src/main.c"
    "#include <http.h>\nint main(void) { return shared_http() != ${expected}; }\n")
  run_at("${project}/src" "${BUILDER}" build --target app --build-dir build)
  run_at("${project}/build/bin" "${project}/build/bin/app${exe}")
endforeach()
run_at("${inner_project}" "${BUILDER}" graph --root ../.. --format json)
string(JSON explicit_preset GET "${run_output}" preset)
string(JSON explicit_inputs GET "${run_output}" inputs)
string(FIND "${explicit_inputs}" "/library workspace/libs/http.json" library_input)
if(NOT explicit_preset STREQUAL "debug" OR library_input EQUAL -1)
  message(FATAL_ERROR "explicit root or shared library input is missing from the graph")
endif()
string(REPLACE "\"42\"" "\"43\"" shared_manifest "${shared_manifest}")
file(WRITE "${shared_libs}/http.json" "${shared_manifest}")
file(WRITE "${inner_project}/src/main.c"
  "#include <http.h>\nint main(void) { return shared_http() != 43; }\n")
run_at("${inner_project}/build" "${NINJA}")
run_at("${inner_project}/build/bin" "${inner_project}/build/bin/app${exe}")
file(WRITE "${shared_libs}/shared.json" [[{"include":["shared.json"]}]])
reject("module include cycle" "${BUILDER}" graph --root "${inner_project}")

# External suites retain actual helper kinds and refer to artifacts before build.
file(WRITE "${shared_libs}/shared.json" [[{"include":["http.json"]}]])
file(WRITE "${outer_project}/src/runner.c" [=[
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) return 2;
  const char *setting = getenv("SLATE_ENV");
  if (!setting || strcmp(setting, "local")) return 4;
  FILE *file = fopen(argv[1], "rb");
  if (!file) return 3;
  if (fclose(file)) return 5;
  if (argc == 3) return strcmp(argv[2], "qualification argument with spaces") ? 9 : 7;
  return 0;
}
]=])
file(WRITE "${outer_project}/src/fixture.c" "int main(void) { return 0; }\n")
file(WRITE "${outer_project}/src/module.json" [=[
{"targets":{
 "runner":{"kind":"executable","sources":{"files":["runner.c"]}},
 "fixture":{"kind":"executable","sources":{"files":["fixture.c"]}},
 "suite":{"kind":"header_only","test":{"tool":"runner","args":["${artifact:fixture}"],"environment":["SLATE_ENV=local"],"labels":["external-runner"]}},
 "program_suite":{"kind":"header_only","test":{"program":"${artifact:fixture}"}}
}}
]=])
file(READ "${outer_project}/slate.json" runner_project)
string(JSON runner_project SET "${runner_project}" presets debug testEnvironment [[ ["SLATE_ENV=global"] ]])
file(WRITE "${outer_project}/slate.json" "${runner_project}")
run_at("${outer_project}" "${BUILDER}" test --target suite --list --build-dir runners)
if(NOT run_output MATCHES "suite")
  message(FATAL_ERROR "header-only suite is not registered")
endif()
run_at("${outer_project}" "${BUILDER}" test --target suite --build-dir runners)
run_at("${outer_project}" "${BUILDER}" test --target program_suite --build-dir runners)
run_at("${outer_project}" "${BUILDER}" test --target suite --build-dir runners
  --accept-exit 7 -- "qualification argument with spaces")
reject("exit 7" "${BUILDER}" test --root "${outer_project}" --target suite
  --build-dir runners -- "qualification argument with spaces")
reject("exactly one matching test" "${BUILDER}" test --root "${outer_project}"
  --build-dir runners -- "qualification argument with spaces")
reject("nonnegative process exit code" "${BUILDER}" test --root "${outer_project}"
  --target suite --accept-exit 7invalid)
reject("not a host-runnable executable" "${BUILDER}" run --root "${outer_project}" --target suite --tests)

# A first build must admit a declared library output and link after its producer.
if(MSVC_DRIVER)
  set(generated_library "shared_http.lib")
else()
  set(generated_library "libshared_http.a")
endif()
set(generated_modules [=[
{"targets":{
 "generated":{"kind":"prebuilt",
   "public":{"includeDirs":["@shared_libs@/include"],"libs":["$builddir/generated/@generated_library@"]},
   "preTasks":[{"name":"archive","builtin":"copy",
     "inputs":["@outer_project@/build/lib/@generated_library@"],
     "outputs":["$builddir/generated/@generated_library@"]}]},
 "linked":{"kind":"executable","sources":{"files":["main.c"]},
   "private":{"deps":["generated"]},"test":{"name":"generated-library"}}
}}
]=])
string(CONFIGURE "${generated_modules}" generated_modules @ONLY)
file(WRITE "${outer_project}/src/module.json" "${generated_modules}")
run_at("${outer_project}" "${BUILDER}" test --target linked --build-dir generated-first)
