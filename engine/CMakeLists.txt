cmake_minimum_required(VERSION 3.27)
project(pysapitts)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

find_package(Python3 3.11 REQUIRED COMPONENTS Development.Embed)

include(FetchContent)

FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG master
)
FetchContent_MakeAvailable(fmt)

set_target_properties(fmt PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

configure_file(
    resource.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/resource.rc
)

configure_file(
    pysapittsengine.rgs
    ${CMAKE_CURRENT_BINARY_DIR}/pysapittsengine.rgs
)

set(MIDL_OUTPUT
    pysapittsengine.h pysapittsengine_i.c pysapittsengine.tlb
)

add_custom_command(
    OUTPUT ${MIDL_OUTPUT}
    COMMAND midl /h pysapittsengine.h ${CMAKE_SOURCE_DIR}/pysapittsengine.idl
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS pysapittsengine.idl
    VERBATIM
)

add_custom_target(midl
    DEPENDS ${MIDL_OUTPUT}
)

# pysapittsengine.dll
add_library(pysapittsengine SHARED
    dllmain.cpp
    engine.cpp
    engine.h
    pycpp.cpp
    pycpp.h
    slog.h
    exports.def
    ${CMAKE_CURRENT_BINARY_DIR}/resource.rc
    ${CMAKE_CURRENT_BINARY_DIR}/pysapittsengine_i.c
)

target_include_directories(pysapittsengine PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(pysapittsengine PRIVATE
    fmt::fmt
    Python3::Python
)

set_target_properties(pysapittsengine PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# speak.exe
add_executable(speak speak.cpp)
target_link_libraries(speak PRIVATE fmt::fmt)
set_target_properties(speak PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# regvoice.exe
add_executable(regvoice
    regvoice.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/pysapittsengine_i.c
)
target_link_libraries(regvoice PRIVATE fmt::fmt)
target_include_directories(regvoice PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)
set_target_properties(regvoice PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
