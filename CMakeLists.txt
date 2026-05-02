cmake_minimum_required(VERSION 3.19)
project(rtp_decoder LANGUAGES CXX DESCRIPTION "RTP decoder and JPEG 2000 codestream parser for hw decoder")

#set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

## WARNING
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -Wstrict-aliasing -Wall -Wextra -Wconversion -Wunused-parameter -Wformat=0 -fexceptions"
)
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -Wno-sign-conversion -Wno-sign-compare"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcolor-diagnostics -Wno-shorten-64-to-32 -Wno-implicit-int-conversion")
endif ()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=always")
endif()

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^[xX]86_64$|^[aA][mM][dD]64$") # x86_64
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mtune=native -march=native")
endif()
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^[aA][rR][mM]64$|^[aA][aA][rR][cC][hH]64$") # aarch64
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mtune=cortex-a72 -march=armv8-a+simd")
endif()
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O3")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS} -O3 -g")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS} -Os -g")

add_executable(rtp_decoder
    frame_handler.hpp
    rtp_receiver.hpp
    rtp_receiver.cpp
    main.cpp
)

target_compile_definitions(${PROJECT_NAME} PRIVATE NDEBUG)

option(PARSER_OVERSHOOT_INSTR "Track per-precinct parser byte overshoot/drift" OFF)
if(PARSER_OVERSHOOT_INSTR)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PARSER_OVERSHOOT_INSTR)
endif()

add_subdirectory(packet_parser)
target_include_directories(rtp_decoder PRIVATE ./ ./packet_parser)
target_link_libraries(rtp_decoder PUBLIC pthread)
