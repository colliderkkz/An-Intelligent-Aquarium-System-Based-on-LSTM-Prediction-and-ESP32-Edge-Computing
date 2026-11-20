# 可选：用官方 ESP-IDF 镜像编译固件（不需要在 Windows 装 IDF）。
# 顺便解决"工程路径含中文导致 IDF 编不了"的问题：容器里挂到 /project 就是纯 ASCII。
FROM espressif/idf:v5.4.4
WORKDIR /project
CMD ["idf.py", "build"]
