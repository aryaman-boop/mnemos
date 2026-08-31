cmake_minimum_required(VERSION 3.20)
project(mnemos
    VERSION 0.1.0
    DESCRIPTION "A Redis-compatible server in modern C++ with a native MCP introspection layer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

option(MNEMOS_SANITIZE "Build with address+undefined sanitizers" OFF)
if(MNEMOS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------- core library
file(GLOB_RECURSE MNEMOS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/server/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/server/commands/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/persist/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/repl/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/client/*.cpp")

add_library(mnemos_core STATIC ${MNEMOS_SOURCES})
target_include_directories(mnemos_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ------------------------------------------------------------------- binaries
add_executable(mnemos-server src/main.cpp)
target_link_libraries(mnemos-server PRIVATE mnemos_core)

file(GLOB MNEMOS_MCP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/mcp/*.cpp")
if(MNEMOS_MCP_SOURCES)
    add_executable(mnemos-mcp ${MNEMOS_MCP_SOURCES})
    target_link_libraries(mnemos-mcp PRIVATE mnemos_core)
endif()

# ---------------------------------------------------------------------- tests
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
