# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
                       REQUIRES esp_tinyusb esp_wifi esp_netif esp_event esp_websocket_client)
