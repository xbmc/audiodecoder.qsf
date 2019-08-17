cmake_minimum_required(VERSION 3.5)
project(psflib)

set(SOURCES psf2fs.c
            psflib.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

add_library(psflib STATIC ${SOURCES})
