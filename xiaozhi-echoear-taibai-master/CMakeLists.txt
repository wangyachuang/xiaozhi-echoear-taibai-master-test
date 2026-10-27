# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.5")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# 说明：gfx 的逐帧性能日志（"W (xxxx) gfx_perf: frame blocks=..."）
# 已在组件源码 gfx_core.c 里把 GFX_REFR_PERF_LOG 默认值改为 0 关闭。
# 之前在这里用 add_compile_definitions(GFX_REFR_PERF_LOG=0) 不生效：
# 该调用在 project() 之后，作用域覆盖不到已注册的 esp_emote_gfx 组件，
# 已确认其编译命令里没有该宏。故改用源码默认值，最可靠。
# 需要排查渲染性能时，把那行改回 1 重新编译即可。

