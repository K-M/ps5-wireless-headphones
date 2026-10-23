cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(LINK_MIC)
    idf_build_set_property(COMPILE_DEFINITIONS "LINK_MIC=1" APPEND)   # headset build: mic on receiver, USB mic on transmitter
endif()
project(link_rx)
