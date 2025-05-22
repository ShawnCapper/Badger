cmake_minimum_required(VERSION 3.20)
project(Badger VERSION 1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Define source files
set(SOURCES
    src/InMemory.cpp
    src/PSARCHandler.cpp
    src/StreamedCopy.cpp
    src/DiskIO.cpp
    src/ParallelCopy.cpp
    main.cpp
)

# Define include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Platform-specific configuration
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX _WIN32)
endif()

# Install configuration
install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# Output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Enable all warnings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
