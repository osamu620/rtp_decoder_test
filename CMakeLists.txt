cmake_minimum_required(VERSION 3.19)
project(rtp_decoder LANGUAGES C CXX DESCRIPTION "RTP decoder and JPEG 2000 codestream parser for hw decoder")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wconversion -Wsign-conversion -Wunused-parameter -Wformat=0 -fexceptions -D__RTP_NO_CRYPTO__"
)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O2")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS} -O2 -g")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS} -Os -g")

add_executable(rtp_decoder
    receiving_poll.cc
        frame_handler.hpp
)

include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(getrandom sys/random.h HAVE_GETRANDOM)
    check_cxx_symbol_exists(sendmsg sys/socket.h HAVE_SENDMSG)
    check_cxx_symbol_exists(sendmmsg sys/socket.h HAVE_SENDMMSG)
    if(HAVE_GETRANDOM)
        list(APPEND UVGRTP_CXX_FLAGS "-DUVGRTP_HAVE_GETRANDOM=1")
        target_compile_definitions(${PROJECT_NAME} PRIVATE UVGRTP_HAVE_GETRANDOM=1)
    endif()
    if(HAVE_SENDMSG)
        list(APPEND UVGRTP_CXX_FLAGS "-DUVGRTP_HAVE_SENDMSG=1")
        target_compile_definitions(${PROJECT_NAME} PRIVATE UVGRTP_HAVE_SENDMSG=1)
    endif()
    if(HAVE_SENDMMSG)
        list(APPEND UVGRTP_CXX_FLAGS "-DUVGRTP_HAVE_SENDMMSG=1")
        target_compile_definitions(${PROJECT_NAME} PRIVATE UVGRTP_HAVE_SENDMMSG=1)
    endif()

add_subdirectory(packet_parser)
add_subdirectory(uvgrtp)
target_include_directories(rtp_decoder PRIVATE ./ ./packet_parser)
target_link_libraries(rtp_decoder PUBLIC pthread)


