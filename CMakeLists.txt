cmake_minimum_required(VERSION 3.7)
project(webrtc_rc_control
	VERSION 0.1.0
	LANGUAGES C CXX)
set(PROJECT_DESCRIPTION "C/C++ RC Control through WebRTC")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -lmmal -lmmal_core  -lmmal_util -I/opt/vc/include/ -L/opt/vc/lib/ -lpthread -lvcos -flto -ffunction-sections -fdata-sections -Wl,--gc-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -O3 -lmmal -lmmal_core  -lmmal_util -I/opt/vc/include/ -L/opt/vc/lib/ -lpthread -lvcos -lssl -lcrypto -flto -ffunction-sections -fdata-sections -Wl,--gc-sections")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Findgpio.cmake")
# list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(pgpio REQUIRED) # FOO_INCLUDE_DIR, FOO_LIBRARIES

set(SOURCE_LIST
${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
${CMAKE_CURRENT_SOURCE_DIR}/src/viewfinder.c
${CMAKE_CURRENT_SOURCE_DIR}/src/mmalcam.c
${CMAKE_CURRENT_SOURCE_DIR}/src/helpers.cpp
${CMAKE_CURRENT_SOURCE_DIR}/src/h264_common.cc
${CMAKE_CURRENT_SOURCE_DIR}/src/dispatchqueue.cpp
${CMAKE_CURRENT_SOURCE_DIR}/src/ArgParser.cpp)

set(LIBRARY_LIST
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libdatachannel.so
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libjuice.a
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libsrtp2.a
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libusrsctp.a
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(main ${SOURCE_LIST})
target_link_libraries(main PRIVATE ${LIBRARY_LIST} Threads::Threads pigpio)