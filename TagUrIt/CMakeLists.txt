cmake_minimum_required(VERSION 3.10)
project(TagUrIt)

set(CMAKE_CXX_STANDARD 17)

# Add DMALibrary
include_directories(../DMALibrary/DMALibrary)
link_directories(../DMALibrary/DMALibrary/libs)

add_executable(TagUrIt main.cpp)

# Link libraries
target_link_libraries(TagUrIt leechcore vmm d2d1 dwrite)