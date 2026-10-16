cmake_minimum_required(VERSION 3.20)
project(legion-power-manager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Super-optimized release flags
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG -Wall -Wextra")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

include_directories(include ${SYSTEMD_INCLUDE_DIRS})

add_executable(legion-power
    src/main.cpp
    src/hardware.cpp
    src/notifier.cpp
    src/service.cpp
)

target_link_libraries(legion-power PRIVATE ${SYSTEMD_LIBRARIES})
target_compile_options(legion-power PRIVATE ${SYSTEMD_CFLAGS_OTHER})

install(TARGETS legion-power DESTINATION bin)
