cmake_minimum_required(VERSION 3.16)
project(aulos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# When Aulos is consumed via add_subdirectory(), tests/examples default to OFF
# so a host project does not pay for them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AULOS_IS_TOP_LEVEL ON)
else()
    set(AULOS_IS_TOP_LEVEL OFF)
endif()

option(AULOS_BUILD_TESTS    "Build the Aulos test suite"            ${AULOS_IS_TOP_LEVEL})
option(AULOS_BUILD_EXAMPLES "Build the standalone C example"        ${AULOS_IS_TOP_LEVEL})
option(AULOS_SHARED         "Also build a shared library for C#/Unity and other FFI hosts" OFF)
option(AULOS_INSTALL        "Generate install rules"                ${AULOS_IS_TOP_LEVEL})

set(AULOS_SOURCES src/aulos.cpp src/miniaudio_impl.c)

# ---- static library (the normal way to embed Aulos) ----------------------
add_library(aulos STATIC ${AULOS_SOURCES})
add_library(aulos::aulos ALIAS aulos)

target_include_directories(aulos
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern
)

if(MSVC)
    target_compile_options(aulos PRIVATE /W4)
else()
    target_compile_options(aulos PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(aulos PUBLIC Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(aulos PUBLIC dl m)
endif()
if(APPLE)
    target_link_libraries(aulos PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

# ---- shared library for FFI hosts (C#, Unity, Python, ...) ---------------
# Produces aulos.dll / libaulos.so / libaulos.dylib. For Unity, drop it into
# Assets/Plugins next to bindings/unity/Aulos.cs.
if(AULOS_SHARED)
    add_library(aulos_shared SHARED ${AULOS_SOURCES})
    add_library(aulos::shared ALIAS aulos_shared)
    set_target_properties(aulos_shared PROPERTIES
        OUTPUT_NAME aulos
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET default)
    target_compile_definitions(aulos_shared PRIVATE AULOS_BUILD_SHARED)
    target_include_directories(aulos_shared
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern)
    target_link_libraries(aulos_shared PUBLIC Threads::Threads)
    if(UNIX AND NOT APPLE)
        target_link_libraries(aulos_shared PUBLIC dl m)
    endif()
    if(APPLE)
        target_link_libraries(aulos_shared PUBLIC
            "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    endif()
endif()

# ---- standalone example (no engine involved) ----------------------------
if(AULOS_BUILD_EXAMPLES)
    add_executable(hello_aulos examples/hello_aulos.c)
    target_link_libraries(hello_aulos PRIVATE aulos)
endif()

# ---- tests, benchmark, offline demo -------------------------------------
if(AULOS_BUILD_TESTS)
    enable_testing()

    add_executable(test_aulos tests/test_aulos.cpp)
    target_link_libraries(test_aulos PRIVATE aulos)
    add_test(NAME aulos
             COMMAND test_aulos
                     ${CMAKE_CURRENT_SOURCE_DIR}/examples/test_bank.json
                     ${CMAKE_CURRENT_SOURCE_DIR}/assets)

    add_executable(bench_aulos tests/bench_aulos.cpp)
    target_link_libraries(bench_aulos PRIVATE aulos)

    add_executable(aulos_demo tools/aulos_demo.cpp)
    target_link_libraries(aulos_demo PRIVATE aulos)

    if(AULOS_BUILD_EXAMPLES)
        add_test(NAME aulos_hello
                 COMMAND hello_aulos
                         ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello_bank.json
                         ${CMAKE_CURRENT_SOURCE_DIR}/assets
                         ${CMAKE_CURRENT_BINARY_DIR}/hello.wav)
    endif()
endif()

# ---- install -------------------------------------------------------------
if(AULOS_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS aulos EXPORT aulosTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(AULOS_SHARED)
        install(TARGETS aulos_shared EXPORT aulosTargets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    install(FILES include/aulos.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT aulosTargets NAMESPACE aulos::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aulos)
endif()
