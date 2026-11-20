cmake_minimum_required(VERSION 3.5)  #指定最低版本要求

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fish_gundam_panzer VERSION 1.1.1)  #项目名称（VERSION 决定 esp_app_desc 里的 ver）

# 说明：
#  - 旧工程在 project() 之后才 set(EXTRA_COMPONENT_DIRS components/tflite-micro)，
#    既晚于 include(project.cmake) 而完全无效，而且该目录并不存在；
#    同时 set(ESPTOOLPY_FLASHSIZE "16MB") 也不是有效的 CMake 变量，
#    Flash 大小必须通过 sdkconfig / sdkconfig.defaults 设置（已修正）。
#  - 编译告警选项放在 main/CMakeLists.txt 里，只作用于本工程代码，
#    避免把 IDF 自带组件也一起打开 -Wextra。
