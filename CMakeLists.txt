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

# ===========================================================================
#  第三方库：nlohmann/json（JSON 序列化/反序列化）
#  ---------------------------------------------------------------------------
#  C++ 没有 npm/pip 那种中心化包管理，"用别人的库"常见的四种做法：
#
#   1) 把源码拷进项目（vendoring）—— 最土但最稳，缺点是 900KB 的别人的头文件
#      躺在自己仓库里，diff/blame 全被污染。
#   2) FetchContent —— 下面用的这个。仓库里只留几行"从哪下、什么版本、
#      校验值多少"，configure 时自动下到 build/_deps/ 下，源码不进版本库。
#   3) git submodule —— 仓库里只留一个 commit 号，但 clone 要加 --recursive，
#      忘了就编译不过；国内网络下同样容易卡。
#   4) vcpkg / Conan —— 大型项目的正规解法，但本项目卖点是"零运行时依赖、
#      单文件 exe"，为一个大头文件拉一个包管理器进来不合算。
#
#  注意项：
#   - 固定 URL + SHA256：下到的东西必须和作者发布的一致，防止被中间人替换。
#   - 国内直连 github.com 实测超时（返回 000），默认走 ghfast.top 镜像。
#     能直连就覆盖这个变量：
#       cmake -S . -B build -G "MinGW Makefiles" -DNLOHMANN_URL=<官方地址>
#   - 想完全离线构建：把单头文件 json.hpp 放到 third_party/nlohmann/ 下，
#     下面的 if 会自动改用它，跳过一切下载。
# ===========================================================================
set(NLOHMANN_TAG "v3.12.0")
set(NLOHMANN_URL
    "https://ghfast.top/https://github.com/nlohmann/json/releases/download/${NLOHMANN_TAG}/json.tar.xz"
    CACHE STRING "nlohmann/json 下载地址（可换成官方 GitHub 地址或 file:/// 本地包）")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(STATUS "nlohmann/json: 使用本地副本 third_party/nlohmann/json.hpp（不下载）")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
else()
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)   # 解压出来的文件用当前时间戳，避免每次都重编
    endif()
    set(JSON_BuildTests OFF CACHE INTERNAL "")   # 只要头文件，不要它的单元测试
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
    nlohmann_json::nlohmann_json   # 头文件库，只提供 include 路径
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
