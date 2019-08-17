cmake_minimum_required(VERSION 3.5)
project(psflib)

set(SOURCES psf2fs.c
            psflib.c)

add_library(psflib STATIC ${SOURCES})
set_property(TARGET psflib PROPERTY POSITION_INDEPENDENT_CODE ON)
