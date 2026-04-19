# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
FILE(GLOB_RECURSE lib_sources ${CMAKE_SOURCE_DIR}/lib/daplink/*.*)

idf_component_register(SRCS ${app_sources} ${lib_sources}
                       INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/lib/daplink
                       REQUIRES esp_tinyusb esp_wifi esp_netif esp_event esp_websocket_client)
