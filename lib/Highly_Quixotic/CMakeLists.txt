project(Highly_Quixotic)

enable_language(C)

cmake_minimum_required(VERSION 2.6)

set(SOURCES Core/kabuki.c
            Core/qmix.c
            Core/qsound.c
            Core/z80.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
add_definitions(-DEMU_COMPILE -DEMU_LITTLE_ENDIAN)

add_library(highly_quixotic STATIC ${SOURCES})
