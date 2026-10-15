cmake_minimum_required(VERSION 3.16)

# ===========================================================================
#  brightnessctl — Windows 显示器亮度控制（DDC/CI + WMI）+ 本地 Web UI
#  ---------------------------------------------------------------------------
#  构建（MinGW-w64 / GCC；不需要 MSVC，也不需要 Windows SDK）：
#
#      cmake -S . -B build -G "MinGW Makefiles"
#      cmake --build build
#
#  指定工具链（下面的默认路径都不存在时才需要）：
#      cmake -S . -B build -G "MinGW Makefiles" -DMINGW_BIN=<mingw 的 bin 目录>
#
#  产物：brightnessctl.exe
#  ===========================================================================

# ---- 工具链选择（必须在 project() 之前设置编译器）----
if(NOT DEFINED MINGW_BIN OR MINGW_BIN STREQUAL "")
    if(DEFINED ENV{MINGW})
        set(MINGW_BIN "$ENV{MINGW}")
    else()
        set(MINGW_BIN "D:/git/tools/Qt/Qt6.8.3/Tools/mingw1310_64/bin")
    endif()
endif()

# 默认路径不存在时，回退到 Qt 的常规安装位置
if(NOT EXISTS "${MINGW_BIN}/g++.exe")
    set(MINGW_BIN "C:/Qt/Qt6.8.3/Tools/mingw1310_64/bin")
endif()

if(NOT EXISTS "${MINGW_BIN}/g++.exe")
    message(FATAL_ERROR
        "未找到 MinGW 的 g++.exe。请用以下方式之一指定工具链：\n"
        "  cmake -S . -B build -G \"MinGW Makefiles\" -DMINGW_BIN=<mingw 的 bin 目录>\n"
        "  或设置环境变量 MINGW=<mingw 的 bin 目录>")
endif()

# MinGW 的 bin 必须在 PATH 里，否则编译会失败：
# gcc 的真正编译器 cc1.exe / cc1plus.exe 位于 libexec\gcc\...\ 下，
# 但它们依赖的 libwinpthread-1.dll 只在 bin\ 里；不在 PATH 时会以 0xC0000135
# 静默失败（只返回退出码，没有任何报错信息），排查起来非常费劲。
# 这里提前检查并给出明确提示，避免掉进 CMake 那句 "compiler is broken"。
string(REPLACE "/" "\\" _MINGW_BIN_NORM "${MINGW_BIN}")
string(REGEX REPLACE "\\\\+$" "" _MINGW_BIN_NORM "${_MINGW_BIN_NORM}")
string(TOLOWER "${_MINGW_BIN_NORM}" _MINGW_BIN_LOWER)

string(REPLACE ";" "|" _PATH_RAW "$ENV{PATH}")
string(REPLACE "|" ";" _PATH_LIST "${_PATH_RAW}")
set(_MINGW_IN_PATH FALSE)
foreach(_dir IN LISTS _PATH_LIST)
    string(REPLACE "/" "\\" _d "${_dir}")
    string(REGEX REPLACE "\\\\+$" "" _d "${_d}")
    string(TOLOWER "${_d}" _d)
    if(_d STREQUAL _MINGW_BIN_LOWER)
        set(_MINGW_IN_PATH TRUE)
    endif()
endforeach()

if(NOT _MINGW_IN_PATH)
    message(FATAL_ERROR
        "MinGW 的 bin 目录不在 PATH 里，编译器将无法启动（cc1plus.exe 找不到 libwinpthread-1.dll）。\n"
        "请先执行（路径换成你的实际位置）：\n"
        "  set PATH=${_MINGW_BIN_NORM};%PATH%\n"
        "然后再运行 cmake。")
endif()

set(CMAKE_C_COMPILER   "${MINGW_BIN}/gcc.exe")
set(CMAKE_CXX_COMPILER "${MINGW_BIN}/g++.exe")
set(CMAKE_RC_COMPILER  "${MINGW_BIN}/windres.exe")
set(CMAKE_MAKE_PROGRAM "${MINGW_BIN}/mingw32-make.exe" CACHE FILEPATH "make 程序" FORCE)

project(brightnessctl VERSION 1.0.0 LANGUAGES CXX RC)

message(STATUS "工具链: ${MINGW_BIN}")

# ---- 基本设置 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# windres 要在源目录里找 manifest / ico / ui 页面
set(CMAKE_RC_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}")

# ---- 目标 ----
add_executable(brightnessctl
    brightnessctl.cpp
    httpd.cpp
    httpd.h
    brightnessctl.rc
    ui/index.html      # 内嵌进 exe 的页面（HEADER_FILE_ONLY：只用于追踪改动）
)

set_source_files_properties(ui/index.html PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(httpd.h       PROPERTIES HEADER_FILE_ONLY TRUE)

# windres 不会自动扫描 .rc 里引用的外部文件。若不显式声明依赖，改了
# ui/index.html 之后 .rc 不会重新编译，exe 里内嵌的仍然是旧页面
# （表现为"改了前端但页面没变化"，且没有任何报错）。
set_source_files_properties(brightnessctl.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html;${CMAKE_CURRENT_SOURCE_DIR}/brightnessctl.ico;${CMAKE_CURRENT_SOURCE_DIR}/brightnessctl.manifest")

target_include_directories(brightnessctl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(brightnessctl PRIVATE
    _WIN32_WINNT=0x0601
    WINVER=0x0601
    NOMINMAX
)

target_compile_options(brightnessctl PRIVATE -Wall -O2)

# -static：把 libgcc / libstdc++ / libwinpthread 静态链进去，产出自包含单文件
# -s      ：去掉符号表；后面三个是安全加固选项
target_link_options(brightnessctl PRIVATE
    -static
    -s
    -Wl,--nxcompat
    -Wl,--dynamicbase
    -Wl,--high-entropy-va
)

target_link_libraries(brightnessctl PRIVATE
    dxva2        # DDC/CI：GetMonitorBrightness / GetVCPFeatureAndVCPFeatureReply
    wbemuuid     # WMI：WmiSetBrightness
    ole32
    oleaut32
    uuid
    ws2_32       # 本地 HTTP 服务
    user32
    shell32
    advapi32
)
