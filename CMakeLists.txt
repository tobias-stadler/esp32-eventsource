cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-eventsource)

spiffs_create_partition_image(storage web FLASH_IN_PROJECT)
