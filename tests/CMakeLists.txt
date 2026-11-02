# a library sending its requests through ciphon, checked from c with libcurl, swift with URLSession and kotlin
# with OkHttp against a local server. the target is not called `net`, the jdk already loads a libnet of its own.
add_library(netdemo SHARED net/net.c)
target_include_directories(netdemo PUBLIC net)
target_link_libraries(netdemo PUBLIC ciphon)
target_compile_options(netdemo PRIVATE -Wall -Wextra -Wpedantic)
ciffi_generate(netdemo
  HEADERS net/net.h
  NAMESPACE net
  DEPENDS ciphon
  SWIFT
  KOTLIN_PACKAGE dev.ciko.ciphon.net)

find_program(PYTHON3 python3)
set(with_server ${CMAKE_CURRENT_SOURCE_DIR}/net/with-server.sh)
if(PYTHON3 AND TARGET ciphon_curl)
  add_executable(c-net c/net.c)
  target_link_libraries(c-net PRIVATE netdemo ciphon::curl)
  add_test(NAME c COMMAND ${with_server} $<TARGET_FILE:c-net>)
  add_executable(c-stream c/stream.c)
  target_link_libraries(c-stream PRIVATE ciphon::curl)
  add_test(NAME c-stream COMMAND ${with_server} $<TARGET_FILE:c-stream>)
  add_executable(c-options c/options.c)
  target_link_libraries(c-options PRIVATE ciphon::curl)
  target_compile_definitions(c-options PRIVATE ${ciphon_defines})
  add_test(NAME c-options COMMAND ${with_server} $<TARGET_FILE:c-options>)
endif()

if(PYTHON3 AND TARGET netdemo_swift)
  add_executable(swift-net swift/main.swift)
  set_target_properties(swift-net PROPERTIES Swift_LANGUAGE_VERSION 6)
  target_link_libraries(swift-net PRIVATE netdemo_swift)
  add_test(NAME swift COMMAND ${with_server} $<TARGET_FILE:swift-net>)
endif()

find_package(Java COMPONENTS Runtime)
get_target_property(net_jar netdemo CIFFI_KOTLIN_JAR)
get_target_property(ciphon_jar ciphon CIFFI_KOTLIN_JAR)
if(PYTHON3 AND net_jar AND ciphon_jar AND Java_JAVA_EXECUTABLE AND TARGET netdemo_jni)
  set(test_jar ${CMAKE_CURRENT_BINARY_DIR}/kotlin-net.jar)
  set(classpath ${net_jar} ${ciphon_jar} ${CIPHON_OKHTTP_CLASSPATH})
  string(REPLACE ";" ":" classpath "${classpath}")
  add_custom_command(
    OUTPUT ${test_jar}
    COMMAND ${CIFFI_KOTLINC} -nowarn -classpath ${classpath} ${CMAKE_CURRENT_SOURCE_DIR}/kotlin/Main.kt
      -include-runtime -d ${test_jar}
    DEPENDS kotlin/Main.kt ${net_jar} ${ciphon_jar}
    VERBATIM)
  add_custom_target(net_kotlin_test ALL DEPENDS ${test_jar} netdemo_jni)
  add_test(NAME kotlin COMMAND ${with_server} ${Java_JAVA_EXECUTABLE} --enable-native-access=ALL-UNNAMED
    -Djava.library.path=$<TARGET_FILE_DIR:netdemo_jni> -cp ${test_jar}:${classpath} MainKt)
endif()
