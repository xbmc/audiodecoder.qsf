project(audiodecoder.qsf)

cmake_minimum_required(VERSION 2.6)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

enable_language(CXX)

find_package(kodi REQUIRED)
find_package(kodiplatform REQUIRED)

include_directories(${KODI_INCLUDE_DIR}
                    ${KODIPLATFORM_INCLUDE_DIRS}
                    ${PROJECT_SOURCE_DIR}/lib/psflib
                    ${PROJECT_SOURCE_DIR}/lib/Highly_Quixotic/Core)

add_subdirectory(lib/psflib)
add_subdirectory(lib/Highly_Quixotic)

set(QSF_SOURCES src/QSFCodec.cpp
                src/RingBuffer.cpp)

set(DEPLIBS highly_quixotic psflib)

build_addon(audiodecoder.qsf QSF DEPLIBS)

include(CPack)
