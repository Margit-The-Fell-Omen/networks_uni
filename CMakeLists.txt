cmake_minimum_required(VERSION 3.16)

project(SerialChat VERSION 1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(serial_chat WIN32
    main.cpp
    serial.cpp
    serial.h
    resources.rc
)

if(MSVC)
    target_compile_options(serial_chat PRIVATE /utf-8)
else()
    target_compile_options(serial_chat PRIVATE -O2 -s)
    target_link_options(serial_chat PRIVATE -static)
endif()

target_link_libraries(serial_chat PRIVATE user32 gdi32 advapi32)
