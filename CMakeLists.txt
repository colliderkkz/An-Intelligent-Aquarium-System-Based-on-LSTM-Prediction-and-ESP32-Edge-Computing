cmake_minimum_required(VERSION 3.5)  #指定最低版本要求
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fish_gundam_panzer)  #项目名称


# 添加额外的组件目录（如果需要）
set(EXTRA_COMPONENT_DIRS components/tflite-micro)

# 设置编译选项（可选）
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# 设置Flash大小（根据你的ESP32-S3N16R8，16MB Flash）
set(ESPTOOLPY_FLASHSIZE "16MB")

# 设置分区表（如果需要自定义分区表）
# set(PARTITION_TABLE "partitions.csv")