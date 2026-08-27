cmake_minimum_required(VERSION 4.2 FATAL_ERROR)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/import-std.cmake)
set(CMAKE_CXX_EXTENSIONS OFF)

project(minimal-android-ndk VERSION 0.1 LANGUAGES CXX)

# The Makefile generators do not scan C++ modules, so a tree configured
# without Ninja fails later with missing module interfaces rather than here.
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR
    "minimal-android-ndk is built as C++23 modules, which needs the Ninja generator: "
    "configure with -G Ninja (found ${CMAKE_GENERATOR}).")
endif()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "Configure into a separate build directory.")
endif()

include(cmake/CPM.cmake)

# EXCLUDE_FROM_ALL keeps the dependency's own install rules out of this
# project's install: CMake ignores install() from a subdirectory added that
# way, which is the difference between installing a tool and installing a
# copy of somebody else's headers with it.
CPMAddPackage(
  NAME nlohmann_json
  GITHUB_REPOSITORY nlohmann/json
  VERSION 3.11.3
  EXCLUDE_FROM_ALL YES
  OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF"
)

set(mandk_modules
  src/posix.cc
  src/json.cc
  src/text.cc
  src/log.cc
  src/process.cc
  src/sha256.cc
  src/copy.cc
  src/search.cc
  src/layout.cc
  src/manifest.cc
  src/journal.cc
  src/git.cc
  src/aosp.cc
  src/pin.cc
  src/plan.cc
  src/toolchainfile.cc
  src/build.cc
  src/steps/sources.cc
  src/steps/headers.cc
  src/steps/stubs.cc
  src/steps/runtimes.cc
  src/steps/check.cc
  src/steps/apksigner.cc
  src/steps/framework.cc
  src/steps/apk.cc
  src/steps/thirdparty.cc
  src/steps/skia.cc
  src/steps/planned.cc
  src/steps/hashes.cc
)

add_executable(minimal-android-ndk src/main.cc)
target_sources(minimal-android-ndk PRIVATE FILE_SET CXX_MODULES FILES ${mandk_modules})
target_compile_features(minimal-android-ndk PRIVATE cxx_std_23)
target_link_libraries(minimal-android-ndk PRIVATE nlohmann_json::nlohmann_json)

install(TARGETS minimal-android-ndk RUNTIME DESTINATION bin)
install(FILES manifest/sources.json DESTINATION share/minimal-android-ndk)
