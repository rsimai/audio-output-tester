cmake_minimum_required(VERSION 3.16)
project(audio_device_tester C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(ALSA REQUIRED alsa)

add_executable(audio-device-tester src/main.c)

target_include_directories(audio-device-tester PRIVATE
    ${GTK3_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
)

target_link_directories(audio-device-tester PRIVATE
    ${GTK3_LIBRARY_DIRS}
    ${ALSA_LIBRARY_DIRS}
)

target_link_libraries(audio-device-tester PRIVATE
    ${GTK3_LIBRARIES}
    ${ALSA_LIBRARIES}
    m
)

target_compile_options(audio-device-tester PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${ALSA_CFLAGS_OTHER}
)
