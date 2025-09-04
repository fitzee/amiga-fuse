cmake_minimum_required(VERSION 3.15)
project(amiga-fuse VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

# Find FUSE
if(APPLE)
    # macOS - try macFUSE first
    pkg_check_modules(FUSE REQUIRED fuse)
    if(NOT FUSE_FOUND)
        message(FATAL_ERROR "macFUSE not found. Install from https://osxfuse.github.io/")
    endif()
elseif(UNIX)
    # Linux - try fuse3 first, fallback to fuse
    pkg_check_modules(FUSE fuse3)
    if(NOT FUSE_FOUND)
        pkg_check_modules(FUSE REQUIRED fuse)
    endif()
    if(NOT FUSE_FOUND)
        message(FATAL_ERROR "FUSE not found. Install libfuse3-dev or libfuse-dev")
    endif()
endif()

# Create executable
add_executable(amiga-fuse amiga-fuse.cpp)

# Set compiler flags for optimization
target_compile_options(amiga-fuse PRIVATE
    -Os                    # Optimize for size
    -fPIC                  # Position-independent code
    -DNDEBUG              # Remove debug code
    -ffunction-sections   # Separate functions for dead code elimination
    -fdata-sections       # Separate data for dead code elimination
)

# Conditionally add flags based on compiler
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(amiga-fuse PRIVATE
        -fno-rtti          # Disable runtime type information
        -fno-exceptions    # Disable exceptions
    )
endif()

# Link-time optimization
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set_property(TARGET amiga-fuse PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# Link FUSE libraries
target_include_directories(amiga-fuse PRIVATE ${FUSE_INCLUDE_DIRS})

# Handle library linking properly
if(FUSE_LINK_LIBRARIES)
    target_link_libraries(amiga-fuse ${FUSE_LINK_LIBRARIES})
else()
    # Fallback to manual library paths
    find_library(FUSE_LIB fuse PATHS ${FUSE_LIBRARY_DIRS} /usr/local/lib /opt/homebrew/lib)
    if(FUSE_LIB)
        target_link_libraries(amiga-fuse ${FUSE_LIB})
    else()
        target_link_libraries(amiga-fuse ${FUSE_LIBRARIES})
    endif()
endif()

# Add FUSE compile flags
if(FUSE_CFLAGS_OTHER)
    target_compile_definitions(amiga-fuse PRIVATE ${FUSE_CFLAGS_OTHER})
endif()

# Add pthread for FUSE
find_package(Threads REQUIRED)
target_link_libraries(amiga-fuse Threads::Threads)

# Platform-specific linker flags
if(APPLE)
    target_link_options(amiga-fuse PRIVATE
        -Wl,-dead_strip    # Remove unused symbols
        -Wl,-x             # Strip local symbols
    )
elseif(UNIX)
    target_link_options(amiga-fuse PRIVATE
        -Wl,--gc-sections  # Remove unused sections
        -Wl,--strip-all    # Strip all symbols
    )
endif()

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Installation
install(TARGETS amiga-fuse
    RUNTIME DESTINATION bin
)

# Print configuration summary
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "FUSE libraries: ${FUSE_LIBRARIES}")
message(STATUS "FUSE include dirs: ${FUSE_INCLUDE_DIRS}")
if(APPLE)
    message(STATUS "Platform: macOS (using macFUSE)")
else()
    message(STATUS "Platform: Linux (using FUSE)")
endif()