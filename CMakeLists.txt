cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Core Network)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Network)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

find_package(OpenSSL REQUIRED)
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
find_library(AVCODEC_LIBRARY NAMES avcodec REQUIRED)
find_library(AVUTIL_LIBRARY NAMES avutil REQUIRED)
find_library(SWSCALE_LIBRARY NAMES swscale REQUIRED)
find_path(OPUS_INCLUDE_DIR opus_multistream.h PATH_SUFFIXES opus)
find_library(OPUS_LIBRARY NAMES opus libopus libopus-0)
if(NOT OPUS_LIBRARY OR NOT OPUS_INCLUDE_DIR)
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  # Opus 1.5.2 CPU detection targets one architecture. Use its portable decoder
  # for the combined Apple arm64/x86_64 target rather than cross-arch intrinsics.
  if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
    set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(receiver_opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG ddbe48383984d56acd9e1ab6a090c54ca6b735a6
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(receiver_opus)
  set_target_properties(opus PROPERTIES COMPILE_WARNING_AS_ERROR OFF XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS NO)
  target_compile_options(opus PRIVATE $<$<C_COMPILER_ID:AppleClang,Clang>:-Wno-vla>)
  set(OPUS_LIBRARY opus)
  set(OPUS_INCLUDE_DIR "${receiver_opus_SOURCE_DIR}/include")
endif()
include(CheckFunctionExists)
include(CheckCXXSourceCompiles)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/moonlight-common-c EXCLUDE_FROM_ALL)
target_compile_options(enet PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wno-unused-parameter> $<$<C_COMPILER_ID:MSVC>:/wd4100> $<$<C_COMPILER_ID:MSVC>:/wd5287>)
target_compile_definitions(moonlight-common-c PRIVATE OBS_RECEIVE_ONLY=1)
# Preserve upstream's portable socket-sized types and constant platform branches.
# OBS template diagnostics remain strict for this plugin's own C++ sources.
target_compile_options(moonlight-common-c PRIVATE $<$<C_COMPILER_ID:AppleClang,Clang>:-Wno-error=shorten-64-to-32> $<$<C_COMPILER_ID:AppleClang,Clang>:-Wno-error=unreachable-code>)
if(APPLE)
  target_compile_definitions(moonlight-common-c PRIVATE __APPLE_USE_RFC_3542=1)
  target_compile_definitions(enet PRIVATE __APPLE_USE_RFC_3542=1)
endif()
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src third_party/moonlight-6.2 ${FFMPEG_INCLUDE_DIR} ${OPUS_INCLUDE_DIR})
find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h)
if(SIMDE_INCLUDE_DIR)
  target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE ${SIMDE_INCLUDE_DIR})
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE moonlight-common-c OpenSSL::Crypto ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${OPUS_LIBRARY})
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/plugin-main.cpp src/receiver.cpp src/media.cpp
  third_party/moonlight-6.2/nvhttp.cpp third_party/moonlight-6.2/nvhttp.h
  third_party/moonlight-6.2/nvpairingmanager.cpp third_party/moonlight-6.2/nvpairingmanager.h
  third_party/moonlight-6.2/nvaddress.cpp third_party/moonlight-6.2/nvapp.cpp
  third_party/moonlight-6.2/identitymanager.cpp
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

option(RECEIVER_BUILD_TESTS "Build headless media and settings tests" ON)
if(RECEIVER_BUILD_TESTS)
  enable_testing()
  add_executable(receiver-tests tests/tests.cpp src/media.cpp
    third_party/moonlight-6.2/nvhttp.cpp third_party/moonlight-6.2/nvhttp.h
    third_party/moonlight-6.2/identitymanager.cpp third_party/moonlight-6.2/nvaddress.cpp third_party/moonlight-6.2/nvapp.cpp)
  set_target_properties(receiver-tests PROPERTIES AUTOMOC ON)
  target_include_directories(receiver-tests PRIVATE src third_party/moonlight-6.2 ${FFMPEG_INCLUDE_DIR} ${OPUS_INCLUDE_DIR})
  if(SIMDE_INCLUDE_DIR)
    target_include_directories(receiver-tests SYSTEM PRIVATE ${SIMDE_INCLUDE_DIR})
  endif()
  target_compile_definitions(receiver-tests PRIVATE TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(receiver-tests PRIVATE OBS::libobs Qt6::Core Qt6::Network moonlight-common-c OpenSSL::Crypto ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${OPUS_LIBRARY})
  add_test(NAME media-and-settings COMMAND receiver-tests)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME receive-only-audit COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/audit_receive_only.py)
  endif()
endif()
