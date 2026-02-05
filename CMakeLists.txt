cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_DEFINITIONS "BTM_DEFAULT_CONN_WINDOW=0x0012" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "BTM_DEFAULT_CONN_INTERVAL=0x0012" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "BTM_DEFAULT_DISC_WINDOW=0x0012" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "BTM_DEFAULT_DISC_INTERVAL=0x0012" APPEND)
project(clock)
