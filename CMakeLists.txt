# 福瑞音乐 - 仓库根 CMake 入口（薄转发到 src/）
#
# 主工程一直在 src/，README 里的 `cmake -S src -B build ...` 照旧有效。
# 这里只为让 `cmake -S . -B build -G "MinGW Makefiles" ...` 也能直接跑：
# IDE 打开仓库根目录、或按直觉敲命令时不必再记一层 src。
#
# 注意：CMakeCache.txt 会记录源目录，同一个 build 目录不能在 -S . 与
# -S src 之间来回切换；换入口时请换一个构建目录（例如 build-qt57）。
cmake_minimum_required(VERSION 3.16)

# MinGW Makefiles 生成器在读取工程前就要确定 make 程序；只给 CMAKE_C_COMPILER
# 而没把 Qt Tools/mingw*/bin 加进 PATH 时会直接报 CMAKE_MAKE_PROGRAM is not set。
# 顺手在编译器同目录里找一下 mingw32-make，让下面这条命令原样可用：
#   cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=<Qt5> \
#         -DCMAKE_C_COMPILER=<gcc.exe> -DCMAKE_CXX_COMPILER=<g++.exe>
if(WIN32 AND CMAKE_GENERATOR STREQUAL "MinGW Makefiles" AND NOT CMAKE_MAKE_PROGRAM)
    get_filename_component(_furui_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    if(EXISTS "${_furui_cc_dir}/mingw32-make.exe")
        set(CMAKE_MAKE_PROGRAM "${_furui_cc_dir}/mingw32-make.exe"
            CACHE FILEPATH "MinGW Makefiles 的 make 程序" FORCE)
    endif()
endif()

project(FuruiMusicRoot LANGUAGES C CXX)
add_subdirectory(src)
