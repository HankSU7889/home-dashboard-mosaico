cmake_minimum_required(VERSION 3.20)
project(dash_backend LANGUAGES C)

if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    foreach(candidate IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../../../managed_components/espressif__esp-gsp")
        get_filename_component(candidate "${candidate}" ABSOLUTE)
        if(EXISTS "${candidate}/tools/sim_bridge/CMakeLists.txt")
            set(ESP_GSP_COMPONENT_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR
        "Install espressif/esp-gsp first, or set ESP_GSP_COMPONENT_DIR")
endif()

add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)
# 数据层的头文件(dash_data.h / dash_cfg.h / dash_types.h)刻意不依赖 ESP-IDF,
# 这样界面这一层可以原样编到 PC 上跑; 真数据层由 dash_data_stub.c 顶替。
gsp_add_backend(dash_backend
    SOURCES ../app_dash.c platform_pc.c dash_data_stub.c
    INCLUDE_DIRECTORIES ../include ../../dash_data/include
    SCENES ../ui/main.json)
