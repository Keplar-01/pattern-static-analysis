cmake_minimum_required(VERSION 3.16)

project(static_analyzer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Some distro LLVM packages reference this imported target even if zstd package
# config is unavailable. Provide a local fallback target when possible.
if(NOT TARGET zstd::libzstd_shared)
    find_library(ZSTD_SHARED_LIB
        NAMES zstd zstd.so.1 libzstd.so.1
        PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu
    )
    if(ZSTD_SHARED_LIB)
        add_library(zstd::libzstd_shared SHARED IMPORTED)
        set_target_properties(zstd::libzstd_shared PROPERTIES
            IMPORTED_LOCATION "${ZSTD_SHARED_LIB}"
        )
    else()
        add_library(zstd::libzstd_shared INTERFACE IMPORTED)
    endif()
endif()

# -------------------------
# LLVM
# -------------------------

find_package(LLVM CONFIG)

if(NOT LLVM_FOUND)
    message(FATAL_ERROR "LLVM not found. Run 'make setup'")
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# -------------------------
# Sources
# -------------------------

file(GLOB_RECURSE SOURCES
    src/*.cpp
)

add_executable(analyzer ${SOURCES})

target_include_directories(analyzer PRIVATE include)

# -------------------------
# LLVM libs
# -------------------------

llvm_map_components_to_libnames(LLVM_LIBS
    core
    analysis
    scalaropts
    support
    irreader
)

target_link_libraries(analyzer
    PRIVATE
    ${LLVM_LIBS}
)
