cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 只编译 main 及其依赖，跳过 wifi_provisioning / esp_local_ctrl / protocomm
# 等用不到的组件：编译更快、固件更小
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32_key_send)
