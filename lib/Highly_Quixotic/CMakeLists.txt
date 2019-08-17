cmake_minimum_required(VERSION 3.5)
project(Highly_Quixotic)

set(SOURCES Core/kabuki.c
            Core/qmix.c
            Core/qsound.c
            Core/z80.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
add_definitions(-DEMU_COMPILE -DEMU_LITTLE_ENDIAN)

add_library(highly_quixotic STATIC ${SOURCES})
