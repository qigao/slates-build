cmake_minimum_required(VERSION 3.27)

function(run)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${ARGN} failed (${result})\n${stdout}\n${stderr}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}/math" "${WORK}/app")
file(TO_CMAKE_PATH "${NVCC}" nvcc)
get_filename_component(cuda_root "${nvcc}/../.." ABSOLUTE)
string(TOLOWER "${HOST_ARCH}" arch)
if(arch MATCHES "^(amd64|x86_64)$")
  set(arch x64)
elseif(arch STREQUAL "aarch64")
  set(arch arm64)
endif()
if(CMAKE_HOST_WIN32)
  set(os windows)
  set(driver msvc)
  set(cc cl.exe)
  set(cxx cl.exe)
  set(ar lib.exe)
  set(ld link.exe)
  file(TO_CMAKE_PATH "${VSDEVCMD}" vsdevcmd)
  set(vsdevcmd_field "\"vsDevCmd\":\"${vsdevcmd}\",")
  set(cuda_lib "${cuda_root}/lib/x64/cudart_static.lib")
else()
  set(os linux)
  set(driver gcc)
  file(TO_CMAKE_PATH "${CC}" cc)
  string(REGEX REPLACE "gcc([^/]*)$" "g++\\1" cxx "${cc}")
  string(REGEX REPLACE "clang([^/]*)$" "clang++\\1" cxx "${cxx}")
  set(ar ar)
  set(ld "${cxx}")
  set(cuda_lib "${cuda_root}/lib64/libcudart_static.a")
endif()
set(project [=[
{
 "defaultPreset":"cuda","modules":["math","app"],
 "platforms":{"native":{"os":"@os@","arch":"@arch@","toolchains":[{"id":"native"}]}},
 "toolchains":{"native":{"driver":"@driver@","cc":"@cc@","cxx":"@cxx@","ar":"@ar@","ld":"@ld@"}},
 "presets":{"cuda":{"config":"release",@vsdevcmd_field@"compilePoolDepth":2,
   "cStd":"c11","cxxStd":"c++17","options":{"runtimeLib":"dynamic","cudaCompiler":"@nvcc@","cudaArchitecture":"52"}}}
}
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${WORK}/slate.json" "${project}")
set(math [=[
{"name":"cuda_math","kind":"static","sources":{"files":["a.cu","b.cu"]},
 "options":{"cudaRdc":true,"cudaDeviceLink":true},
 "private":{"libs":["@cuda_lib@"]}}
]=])
string(CONFIGURE "${math}" math @ONLY)
file(WRITE "${WORK}/math/module.json" "${math}")
file(WRITE "${WORK}/math/a.cu" [=[
__device__ int increment(int);
__global__ void answer_kernel(int *output) { *output = increment(41); }
extern "C" int cuda_answer(void) { return 42; }
]=])
file(WRITE "${WORK}/math/b.cu" "__device__ int increment(int value) { return value + 1; }\n")
file(WRITE "${WORK}/app/main.c" "int cuda_answer(void);\nint main(void) { return cuda_answer() != 42; }\n")
file(WRITE "${WORK}/app/module.json" [=[
{"name":"cuda_app","kind":"executable","sources":{"files":["main.c"]},
 "private":{"deps":["cuda_math"]},"test":{"name":"cuda.device-link"}}
]=])
run("${BUILDER}" doctor --preset cuda)
run("${BUILDER}" test --preset cuda)
run("${BUILDER}" build --preset cuda)

# An explicit bad compiler must fail at graph admission, with no alternate tool.
string(REPLACE "${nvcc}" "missing-nvcc-for-fixture" project "${project}")
file(WRITE "${WORK}/slate.json" "${project}")
execute_process(COMMAND "${BUILDER}" doctor --root "${WORK}" --preset cuda
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(result EQUAL 0 OR NOT "${stdout}\n${stderr}" MATCHES "CUDA compiler not found")
  message(FATAL_ERROR "Missing NVCC did not fail at graph admission: ${stdout}\n${stderr}")
endif()
