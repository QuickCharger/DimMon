cmake_minimum_required(VERSION 3.16)

# brightnessctl — Windows 显示器亮度控制（DDC/CI + WMI）+ 本地 Web UI
#
# 本文件不含任何机器相关的路径或工具链假设：编译器、生成器、依赖来源
# 一律由外部指定（见 README.md 的「构建」一节）。换电脑不需要改本文件。

# ---- 可选：指定工具链 bin 目录 ----
# 通过 -DMINGW_BIN=<dir> 或环境变量 MINGW 指定；不指定时由 CMake 从 PATH
# 自动查找编译器。同时兼容带 x86_64-w64-mingw32- 前缀的交叉工具链。
if(NOT DEFINED MINGW_BIN AND DEFINED ENV{MINGW})
    set(MINGW_BIN "$ENV{MINGW}")
endif()
if(MINGW_BIN)
    if(EXISTS "${MINGW_BIN}/g++.exe")
        set(_PFX "")
    elseif(EXISTS "${MINGW_BIN}/x86_64-w64-mingw32-g++.exe")
        set(_PFX "x86_64-w64-mingw32-")
    else()
        message(FATAL_ERROR "${MINGW_BIN} 下未找到 g++.exe，请检查 MINGW_BIN")
    endif()
    set(CMAKE_C_COMPILER   "${MINGW_BIN}/${_PFX}gcc.exe")
    set(CMAKE_CXX_COMPILER "${MINGW_BIN}/${_PFX}g++.exe")
    set(CMAKE_RC_COMPILER  "${MINGW_BIN}/${_PFX}windres.exe")
endif()

project(brightnessctl VERSION 1.0.0 LANGUAGES CXX RC)

message(STATUS "CXX 编译器: ${CMAKE_CXX_COMPILER}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# windres 要在源目录里找 manifest / ico / ui 页面
set(CMAKE_RC_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}")

# ---- nlohmann/json v3.12.0（header-only）----
# 仓库内 third_party/nlohmann/json.hpp 优先；否则 FetchContent 下载。
# 下载源默认直连 GitHub；可用 -DNLOHMANN_URL=<URL 或本地归档路径> 覆盖。
set(NLOHMANN_URL
    "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz"
    CACHE STRING "nlohmann/json 来源")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
else()
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    FetchContent_Declare(nlohmann_json
        URL "${NLOHMANN_URL}"
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---- 目标 ----
add_executable(brightnessctl
    brightnessctl.cpp
    httpd.cpp
    httpd.h
    brightnessctl.rc
    ui/index.html
)
set_source_files_properties(ui/index.html httpd.h PROPERTIES HEADER_FILE_ONLY TRUE)

# windres 不扫描 .rc 引用的外部文件；显式声明依赖，否则改了 UI 不会重新内嵌
set_source_files_properties(brightnessctl.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html;${CMAKE_CURRENT_SOURCE_DIR}/brightnessctl.ico;${CMAKE_CURRENT_SOURCE_DIR}/brightnessctl.manifest")

target_include_directories(brightnessctl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(brightnessctl PRIVATE _WIN32_WINNT=0x0601 WINVER=0x0601 NOMINMAX)
target_compile_options(brightnessctl PRIVATE -Wall -O2)

# 静态链入运行时得到自包含单文件；-s 去符号；其余为安全加固（GCC/MinGW 选项）
target_link_options(brightnessctl PRIVATE -static -s
    -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va)

target_link_libraries(brightnessctl PRIVATE
    nlohmann_json::nlohmann_json
    dxva2 wbemuuid ole32 oleaut32 uuid ws2_32 user32 shell32 advapi32
)
