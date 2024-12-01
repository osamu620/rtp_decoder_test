cmake_policy(SET CMP0076 NEW)
target_sources(rtp_decoder
        PRIVATE
        j2k_header.c
        j2k_header.h
        j2k_packet.c
        j2k_packet.h
        j2k_tile.c
        j2k_tile.h
        read_packet.h
        type.h
        utils.c
        utils.h
)