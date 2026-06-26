cmake_minimum_required(VERSION 3.13)
project(softmodem C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -g -O2)

# Feature toggles - each milestone flips one on as its deps land.
option(WITH_DSP "Build spandsp modem core (milestone 2+)" OFF)
option(WITH_RTP "Build oRTP media transport (milestone 3+)" OFF)
option(WITH_SIP "Build sofia-sip signalling (milestone 4+)" OFF)

find_package(Threads REQUIRED)

set(SOFTMODEM_SRC
    src/main.c
    src/config.c
    src/fifo.c
    src/vtty.c
    src/at_engine.c
)

set(SOFTMODEM_LIBS Threads::Threads)
set(SOFTMODEM_DEFS "")

if(WITH_DSP)
    list(APPEND SOFTMODEM_SRC src/modem_core.c src/audio.c)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SPANDSP REQUIRED spandsp)
    list(APPEND SOFTMODEM_LIBS ${SPANDSP_LIBRARIES} m)
    list(APPEND SOFTMODEM_DEFS WITH_DSP=1)
endif()

if(WITH_RTP)
    list(APPEND SOFTMODEM_SRC src/rtp.c)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ORTP REQUIRED ortp)
    pkg_check_modules(BCTBX bctoolbox)          # oRTP 5.x logging depends on it
    list(APPEND SOFTMODEM_LIBS ${ORTP_LIBRARIES} ${BCTBX_LIBRARIES})
    list(APPEND SOFTMODEM_DEFS WITH_RTP=1)
endif()

if(WITH_SIP)
    list(APPEND SOFTMODEM_SRC src/sip.c)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SOFIA REQUIRED sofia-sip-ua)
    list(APPEND SOFTMODEM_LIBS ${SOFIA_LIBRARIES})
    list(APPEND SOFTMODEM_DEFS WITH_SIP=1)
endif()

add_executable(softmodem ${SOFTMODEM_SRC})
target_include_directories(softmodem PRIVATE src ${SPANDSP_INCLUDE_DIRS} ${ORTP_INCLUDE_DIRS} ${SOFIA_INCLUDE_DIRS})
target_link_libraries(softmodem PRIVATE ${SOFTMODEM_LIBS})
target_compile_definitions(softmodem PRIVATE ${SOFTMODEM_DEFS})
