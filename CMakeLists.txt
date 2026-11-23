cmake_minimum_required(VERSION 3.29)
project(ciphon VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})

option(CIPHON_BUILD_TESTS "build the ciphon tests" ${PROJECT_IS_TOP_LEVEL})
option(CIPHON_BINDINGS "generate the swift and kotlin bindings with ciffi" ON)
set(CIPHON_OKHTTP_CLASSPATH "" CACHE STRING "okhttp and okio jars, needed to build the kotlin client jar")
option(CIPHON_FETCH_OKHTTP "download pinned okhttp jars when CIPHON_OKHTTP_CLASSPATH is empty" ${CIPHON_BUILD_TESTS})

# the swift module needs swift enabled before ciffi_generate, consumers enable it first.
if(PROJECT_IS_TOP_LEVEL AND CIPHON_BINDINGS)
  include(CheckLanguage)
  check_language(Swift)
  if(CMAKE_Swift_COMPILER)
    enable_language(Swift)
  endif()
endif()

add_library(ciphon STATIC src/ciphon.c)
add_library(ciphon::ciphon ALIAS ciphon)
target_include_directories(ciphon PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
set_target_properties(ciphon PROPERTIES POSITION_INDEPENDENT_CODE ON)
# pread, open_memstream and strndup sit behind feature macros on glibc.
set(ciphon_defines $<IF:$<PLATFORM_ID:Darwin>,_DARWIN_C_SOURCE,_GNU_SOURCE>)
target_compile_definitions(ciphon PRIVATE ${ciphon_defines})
set(installed ciphon)

find_package(CURL QUIET)
if(CURL_FOUND)
  add_library(ciphon_curl STATIC src/curl.c)
  add_library(ciphon::curl ALIAS ciphon_curl)
  target_link_libraries(ciphon_curl PUBLIC ciphon PRIVATE CURL::libcurl)
  target_compile_definitions(ciphon_curl PRIVATE ${ciphon_defines})
  set_target_properties(ciphon_curl PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME curl)
  list(APPEND installed ciphon_curl)
endif()

if(CIPHON_BINDINGS)
  # a parent project may already provide ciffi, otherwise it is fetched,
  # from a checkout next to this one when there is.
  if(NOT COMMAND ciffi_generate)
    include(FetchContent)
    get_filename_component(local ${CMAKE_CURRENT_SOURCE_DIR}/../ciffi ABSOLUTE)
    if(NOT FETCHCONTENT_SOURCE_DIR_CIFFI AND EXISTS ${local}/CMakeLists.txt)
      set(FETCHCONTENT_SOURCE_DIR_CIFFI ${local})
    endif()
    FetchContent_Declare(ciffi GIT_REPOSITORY ssh://git@vexcited.com/ciko/ciffi.git GIT_TAG main)
    FetchContent_MakeAvailable(ciffi)
  endif()

  if(CIPHON_FETCH_OKHTTP AND NOT CIPHON_OKHTTP_CLASSPATH)
    # jvm builds fetch pinned okhttp jars from maven central, android apps get okhttp from gradle instead.
    set(maven https://repo1.maven.org/maven2)
    file(DOWNLOAD ${maven}/com/squareup/okhttp3/okhttp/4.12.0/okhttp-4.12.0.jar ${CMAKE_BINARY_DIR}/jars/okhttp-4.12.0.jar
      EXPECTED_HASH SHA256=b1050081b14bb7a3a7e55a4d3ef01b5dcfabc453b4573a4fc019767191d5f4e0)
    file(DOWNLOAD ${maven}/com/squareup/okio/okio-jvm/3.6.0/okio-jvm-3.6.0.jar ${CMAKE_BINARY_DIR}/jars/okio-jvm-3.6.0.jar
      EXPECTED_HASH SHA256=67543f0736fc422ae927ed0e504b98bc5e269fda0d3500579337cb713da28412)
    set(CIPHON_OKHTTP_CLASSPATH ${CMAKE_BINARY_DIR}/jars/okhttp-4.12.0.jar ${CMAKE_BINARY_DIR}/jars/okio-jvm-3.6.0.jar
      CACHE STRING "" FORCE)
  endif()

  # without okhttp there is no kotlin client to default to, only the bindings are generated.
  set(kotlin KOTLIN_PACKAGE dev.ciko.ciphon)
  if(CIPHON_OKHTTP_CLASSPATH)
    list(APPEND kotlin KOTLIN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/kotlin/CiphonOkHttp.kt
      KOTLIN_CLASSPATH ${CIPHON_OKHTTP_CLASSPATH} KOTLIN_DEFAULTS "ciphon_client=CiphonOkHttp()")
  endif()
  ciffi_generate(ciphon
    HEADERS include/ciphon/ciphon.h
    NAMESPACE ciphon
    SWIFT
    SWIFT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/swift/CiphonURLSession.swift
    SWIFT_DEFAULTS "ciphon_client=CiphonURLSession()"
    ${kotlin}
    NO_JNI)
endif()

# c consumers only need the headers and the static libraries, ciffi is not part of the install.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS ${installed} EXPORT ciphon-targets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ciphon-targets NAMESPACE ciphon:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ciphon)
configure_package_config_file(cmake/ciphon-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/ciphon-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ciphon)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ciphon-config-version.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ciphon-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/ciphon-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ciphon)

if(CIPHON_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
