cmake_minimum_required(VERSION 3.13)

project(qa_app)

# 设置 C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows MSVC 下使用 vcpkg，MinGW 下使用系统包
if(MSVC)
    set(VCPKG_CMAKE_PATH $ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake CACHE FILEPATH "Location of vcpkg.cmake")
    include(${VCPKG_CMAKE_PATH})
    find_package(ZLIB REQUIRED)
endif()

# 添加 liboai 库（从上级目录引入）
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../liboai" "${CMAKE_CURRENT_BINARY_DIR}/liboai")

# 创建可执行文件 (WIN32 = 无控制台窗口)
if(WIN32)
    add_executable(qa_app WIN32 main.cpp)
else()
    add_executable(qa_app main.cpp)
endif()

# 链接库（oai 会自动传递 include 路径和依赖）
target_link_libraries(qa_app PRIVATE oai)

# Windows 下链接 WinSock2, Crypt32, Shell32, User32
if(WIN32)
    target_link_libraries(qa_app PRIVATE ws2_32 crypt32 shell32 user32)
    # MinGW 需要设置 GUI 子系统
    if(MINGW)
        target_link_options(qa_app PRIVATE -Wl,--subsystem,windows)
    endif()
endif()

# 链接线程库
find_package(Threads REQUIRED)
target_link_libraries(qa_app PRIVATE Threads::Threads)

# 复制 web 目录和配置文件到构建目录
add_custom_command(TARGET qa_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/web
    $<TARGET_FILE_DIR:qa_app>/web
)

# 复制 config.txt（如果存在）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/config.txt")
    add_custom_command(TARGET qa_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/build/config.txt
        $<TARGET_FILE_DIR:qa_app>/config.txt
    )
endif()

# MinGW 下修复 PE 子系统为 Windows GUI（隐藏控制台）
if(WIN32 AND MINGW)
    add_custom_command(TARGET qa_app POST_BUILD
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/patch_gui.ps1"
        -exePath "$<TARGET_FILE:qa_app>"
        COMMENT "Patching PE subsystem to Windows GUI"
    )
endif()
