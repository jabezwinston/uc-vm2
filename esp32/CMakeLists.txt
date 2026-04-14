cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ucvm)

# Generate SPIFFS image from web/ directory and flash it to the "web" partition
spiffs_create_partition_image(web "${CMAKE_CURRENT_LIST_DIR}/web" FLASH_IN_PROJECT)
