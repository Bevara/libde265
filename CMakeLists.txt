cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBDE265_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_hevc.c
)

SET(LIBDE265_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libde265.a
)

SET(LIBDE265_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libde265
        "${LIBDE265_SRC}"
        "${LIBDE265_LIB}"
        []
        ""
        "${LIBDE265_INC}"
        ""
        "1")
