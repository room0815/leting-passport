# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# 乐听护照固件自己的版本号。PROJECT_VER（2.6.0）是上游小智的版本，跟乐听的
# 迭代不是一回事，之前乐听版本只写在交付文件名里，设备上完全看不出来 ——
# 用户报问题时只能说「最新那版」，排查很费劲。
# 这里单独定义一个宏，开机日志会打出来，串口一抓就知道跑的是哪一版。
add_compile_options(-DLETING_FW_VERSION="1.4.15")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.6.0")
project(xiaozhi)
