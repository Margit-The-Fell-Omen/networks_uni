cmake_minimum_required(VERSION 3.16)

project(SerialChat VERSION 1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(serial_chat WIN32
        main.cpp
        serial.cpp
        serial.h
        frame.cpp
        frame.h
        channel.cpp
        channel.h
        resources.rc
)

if(MSVC)
    target_compile_options(serial_chat PRIVATE /utf-8)
    # resources.rc уже содержит app.manifest; иначе линковщик
    # встраивает второй манифест с тем же идентификатором.
    target_link_options(serial_chat PRIVATE /MANIFEST:NO)
else()
    target_compile_options(serial_chat PRIVATE -O2 -s)
endif()

target_link_libraries(serial_chat PRIVATE user32 gdi32 advapi32)
