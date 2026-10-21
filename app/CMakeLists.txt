# Deepstream —— Switch 流媒体音乐播放器
#
# 结构照抄 borealis 官方 demo 的 CMakeLists，只改：
#   - 源码目录 src/ + 复用 engine_cpp/src/（音源引擎）
#   - 加上 curl / mbedtls / z（取流 + TLS）
# 平台用 deko3d 后端（Switch 上比 mesa 软件渲染快得多）。

cmake_minimum_required(VERSION 3.10)

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/../lib/borealis/library)
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../engine_cpp)

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

# ——— devkitPro 路径自注入 ———
# 正常应在 devkitPro 的 msys2 shell 里构建（那里 C:/devkitPro 挂载为 /opt/devkitpro）。
# 若从普通 shell 直接跑 cmake，ENV{DEVKITPRO} 可能缺失，这里兜底注入真实 Windows 路径。
if (PLATFORM_SWITCH AND NOT DEFINED ENV{DEVKITPRO})
    if (EXISTS "C:/devkitPro/cmake/Switch.cmake")
        set(ENV{DEVKITPRO} "C:/devkitPro")
        message(STATUS "DEVKITPRO (fallback injected) = $ENV{DEVKITPRO}")
    endif ()
endif ()

# Dependencies
option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)

include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(deepstream)

set(VERSION_MAJOR "0")
set(VERSION_MINOR "9")
set(VERSION_ALTER "6")
set(VERSION_BUILD "0")
set(PACKAGE_NAME "com.nxhomebrew.deepstream")
set(PROJECT_TITLEID "0100000000AAAAA")
set(PROJECT_AUTHOR "nxhomebrew")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/img/demo_icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)

if (USE_LIBROMFS)
    add_libromfs(${PROJECT_NAME} ${PROJECT_RESOURCES})
endif ()

add_subdirectory(${BOREALIS_LIBRARY} borealis_lib)

# ——— AAC 解码：faad2 ———
# QQ 音乐的「标准」音质有一部分只提供 m4a(AAC)，没有 mp3。
# 只编译 libfaad 的 38 个 .c（frontend/ 用不上），做成静态库链进来。
# ⚠ 许可：faad2 是 GPL-2.0，链接它的 nro 必须按 GPL 分发（源码已随仓库提供）。
set(FAAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/faad2)
file(GLOB FAAD_SRC ${FAAD_DIR}/libfaad/*.c ${FAAD_DIR}/libfaad/codebook/*.c)
add_library(faad2 STATIC ${FAAD_SRC})
target_include_directories(faad2 PUBLIC ${FAAD_DIR}/include ${FAAD_DIR}/libfaad)
# faad2 用 fixed-point 还是 float 由它自己的宏决定，默认 float，和我们的代码一致
# PACKAGE_VERSION 平时由 autotools 生成，这里直接给（decoder.c 里做版本字符串用）
target_compile_definitions(faad2 PRIVATE
    HAVE_STDINT_H=1 HAVE_STRING_H=1 PACKAGE_VERSION=\"2.11.2\")
set_target_properties(faad2 PROPERTIES C_STANDARD 99)

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

# ——— 源码 ———
file(GLOB_RECURSE MAIN_SRC src/*.cpp)
file(GLOB ENGINE_SRC ${ENGINE_DIR}/src/*.cpp)
list(APPEND MAIN_SRC ${ENGINE_SRC})

if (PLATFORM_SWITCH)
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif ()

program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

# ——— Switch 打包 ———
if (PLATFORM_SWITCH)
    if (BOREALIS_USE_DEKO3D)
        gen_dksh("${PROJECT_RESOURCES}/shaders")
    endif ()
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${PROJECT_NAME}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp --titleid=${PROJECT_TITLEID}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/font
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ENGINE_DIR}/include
    ${BOREALIS_LIBRARY}/include)

target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections)

# curl + TLS + zlib：取流链路（engine_cpp 用，audio_engine 下载也用）
# FLAC  : 边下边播，libFLAC 回调式流解码
# mpg123: MP3 流解码
# faad2 : AAC 解码（m4a 容器由 mp4_demux.cpp 拆包）
target_link_libraries(${PROJECT_NAME} PRIVATE
    borealis
    curl z mbedtls mbedx509 mbedcrypto
    FLAC ogg mpg123 faad2
    ${APP_PLATFORM_LIB})

# 封面解码库：CJPG/PNG 由 borealis 依赖的库提供（borealis 内部已带）
# 这里不动，Image::setImageFromMem 走 borealis 自带解码器
