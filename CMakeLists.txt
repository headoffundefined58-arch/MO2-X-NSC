cmake_minimum_required(VERSION 3.16)
project(nsc_mo2_plugin LANGUAGES CXX)

# MO2 2.5.2 自带的 Qt 是 6.7.1；uibase v2.5.2 头文件需要 C++20。
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/utf-8)

# Qt6 由 CI 通过 CMAKE_PREFIX_PATH 提供。虽然插件运行时只用到 QtCore 的符号（QObject /
# QString / QProcess / QPluginLoader 元数据，以及 versioninfo.cpp），但 uibase 的接口头
# 会 transitively #include Qt 的 Gui/Widgets 头（imoinfo.h -> <QMainWindow>，imodinterface.h
# -> <QColor> 等），因此编译期必须能找到 Gui/Widgets 的 include 目录。qtbase 安装包已包含
# Core/Gui/Widgets，这里一并 find；链接 Widgets/Gui 不会在运行时强拉这两个 dll 的额外符号
# （插件不实例化任何 widget），而 MO2 目录本身就带有这些 Qt6 dll，加载无碍。
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# 极简 uibase：只编 versioninfo.cpp（自包含，仅依赖 QtCore，不碰 Boost/spdlog/widgets）。
# 以静态库形式链进插件，因此插件不需要 MO2 的 uibase.lib，也不会产生第二份 uibase.dll；
# 运行时直接复用 MO2 自带 uibase.dll 的纯虚接口（ABI 同版本，安全）。
add_library(nsc_versioninfo STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/uibase/src/versioninfo.cpp")
target_include_directories(nsc_versioninfo PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/uibase/src")
# _NODLL：让 versioninfo.h / dllimport.h 里的 QDLLEXPORT 变成空（普通符号），
# 这样 VersionInfo 及其实例化都在本静态库内，而非从 uibase.dll 导入。
target_compile_definitions(nsc_versioninfo PRIVATE _NODLL)
# versioninfo.cpp 用到 std::map 等，但 uibase 平时靠 pch.h 提供；这里强制包含一个小头文件。
target_compile_options(nsc_versioninfo PRIVATE
    "/FI\"${CMAKE_CURRENT_SOURCE_DIR}/nsc_stdinc.h\"")
target_link_libraries(nsc_versioninfo PUBLIC Qt6::Core)

# 我们的插件（MODULE = 无 main 的共享库，MO2 用 QPluginLoader 动态加载）。
add_library(nsc_plugin MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_plugin.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_plugin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_arc.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_arc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_layout.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_layout.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_prepare.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_prepare.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_merge.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_merge.cpp")
target_include_directories(nsc_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/uibase/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/uibase/src/game_features"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
# 同样定义 _NODLL，保证插件看到的 VersionInfo 符号与静态库一致（普通符号，非 dllimport）。
target_compile_definitions(nsc_plugin PRIVATE _NODLL)
# nsc_arc.cpp 用 7-Zip COM 接口（PROPVARIANT / SysFreeString），需要 ole32/oleaut32。
target_link_libraries(nsc_plugin PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets nsc_versioninfo
    ole32 oleaut32)
set_target_properties(nsc_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "nsc_plugin"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    AUTOMOC ON)

# 把 companion 元数据 json 放到构建产物旁边（MO2 同时也会读取 Q_PLUGIN_METADATA 内嵌的
# 元数据，这里再放一份只是双保险）。
add_custom_command(TARGET nsc_plugin POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/src/nsc_plugin.json"
            "$<TARGET_FILE_DIR:nsc_plugin>/nsc_plugin.json"
    VERBATIM)
