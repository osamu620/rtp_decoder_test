cmake_policy(SET CMP0076 NEW)
target_sources(rtp_decoder
        PRIVATE
    j2k_header.cpp
    j2k_header.hpp
    j2k_packet.cpp
    j2k_packet.hpp
    j2k_tile.cpp
    j2k_tile.hpp
    type.hpp
    utils.cpp
    utils.hpp
)