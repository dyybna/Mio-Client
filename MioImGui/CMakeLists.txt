cmake_minimum_required(VERSION 3.16)
project(MioImGui LANGUAGES C CXX RC)   # RC 用于把 app.ico 编进 exe（MinGW 走 windres）

set(IMGUI_DIR "../imgui-1.92.9b" CACHE PATH "Path to the Dear ImGui source tree")
set(MINHOOK_DIR "../MinHook" CACHE PATH "Path to the MinHook source tree")
# BASS：bass.dll 已编成字节数组内嵌进 exe（BassBlob.h），启动时释放到程序同目录再加载，
#       所以【不需要】把 bass.dll 拷到输出目录。BASS_DIR 只用于编译期找官方 bass.h：
#       找得到就用官方的，找不到自动回退到工程内的 MioBassMini.h（见 MioGain.h）。
set(BASS_DIR "C:/Users/35687/Desktop/bass24" CACHE PATH "Path to the BASS SDK (optional; bass.h)")

add_executable(MioImGui WIN32
    main.cpp
    MioApp.rc
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(MioImGui PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
if(EXISTS "${BASS_DIR}/c/bass.h")
    target_include_directories(MioImGui PRIVATE "${BASS_DIR}/c")
endif()
target_link_libraries(MioImGui PRIVATE opengl32 gdi32 user32 dwmapi winmm ole32)

set_target_properties(MioImGui PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(MSVC)
    target_compile_options(MioImGui PRIVATE /utf-8 /W3)
endif()

# ---------------------------------------------------------------------------
# MioZegoProbe：注入 TT renderer 的码率探针载荷（房间码率功能）
# MinHook 从源码静态编入，无需额外 DLL。

add_library(MioZegoProbe SHARED
    MioZegoProbe/dllmain.cpp
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)

target_include_directories(MioZegoProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MINHOOK_DIR}/include
    ${MINHOOK_DIR}/src
    ${MINHOOK_DIR}/src/hde
)

set_target_properties(MioZegoProbe PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(MSVC)
    target_compile_options(MioZegoProbe PRIVATE /utf-8 /W3)
    set_property(TARGET MioZegoProbe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# ---------------------------------------------------------------------------
# MinGW / GCC 分支：
#   -municode  : 源码入口是 wWinMain（Unicode），MinGW 的 GUI 子系统默认要 WinMain，
#                不加就会链接报 "undefined reference to `WinMain'"
#   -static    : 静态链接 gcc/stdc++ 运行时，避免 exe/DLL 依赖 libstdc++-6.dll /
#                libgcc_s_seh-1.dll —— 载荷 DLL 要注入第三方进程，绝不能有外部运行时依赖
#                （与上面 MSVC 的 MSVC_RUNTIME_LIBRARY "MultiThreaded" 同一目的）
#   PREFIX ""  : 让共享库产物叫 MioZegoProbe.dll 而不是 libMioZegoProbe.dll
if(MINGW)
    target_link_options(MioImGui PRIVATE -municode -static)
    target_link_options(MioZegoProbe PRIVATE -static)
    set_target_properties(MioZegoProbe PROPERTIES PREFIX "")
endif()

add_dependencies(MioImGui MioZegoProbe)
add_custom_command(TARGET MioImGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:MioZegoProbe>" "$<TARGET_FILE_DIR:MioImGui>"
    COMMENT "Copy MioZegoProbe.dll next to MioImGui.exe"
)

# ---------------------------------------------------------------------------
# 防篡改 + 体积填充：
#   1) 扫描 exe 里的定位块，算出 PE 映像部分(跳过该块)的 SHA-256 写进哈希槽
#   2) 把 exe 填充到 40MB（填充在映像之外，不参与校验）
# ---------------------------------------------------------------------------
add_custom_command(TARGET MioImGui POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_exe.ps1"
            -Exe "$<TARGET_FILE:MioImGui>" -TargetMB 40
    COMMENT "Anti-tamper: embed self SHA-256, then pad EXE to 40MB"
)
