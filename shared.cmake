# wxl-housing build glue: vendored third-party sources compiled into the extension.
#
# wxl-core's root CMakeLists auto-discovers this extension (extensions/wxl-housing/*.cpp)
# and builds it with include dirs include/ + src/ and the WXL_EXTENSION define. It does
# NOT put this extension's own folder on the include path, so ImGuizmo.h is reached via
# the relative fallback in the sources (#include "../../third_party/imguizmo/ImGuizmo.h"),
# while this file adds the Dear ImGui core/backends and ImGuizmo translation unit that
# the extension owns. Each DLL needs its own ImGui objects because contexts/backends do
# not cross the plugin ABI.
#
# If a future core version adds the extension dir to the include path, the fallback in
# src/ImGuiHostExt.cpp / src/Placement.cpp is harmless (__has_include picks the short form).

set(WXL_EXT_SHARED_SRC
    ${WXL_EXT_SHARED_SRC}
    ${CMAKE_SOURCE_DIR}/deps/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/deps/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/deps/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/deps/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/deps/imgui/backends/imgui_impl_dx9.cpp
    ${CMAKE_SOURCE_DIR}/deps/imgui/backends/imgui_impl_win32.cpp
    ${CMAKE_CURRENT_LIST_DIR}/third_party/imguizmo/ImGuizmo.cpp
)

set_property(SOURCE ${WXL_EXT_SRC} ${WXL_EXT_SHARED_SRC} APPEND PROPERTY INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/deps/imgui
    ${CMAKE_SOURCE_DIR}/deps/imgui/backends
)
