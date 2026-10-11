# vip_af — Plugify C++ plugin: port of the Pisex VIP module "[VIP] Anti Flash".
#
#   plugify-vip_af  the plugin shared library (entry "bin/vip_af" -> bin/libvip_af.so)

set(VIP_AF_VERSION "1.1.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_* / __hudmenu_* (s2sdk_bindings, hudmenu_bindings - both come in through
#   pisexcompat's PUBLIC link) and __vip_* (vip_bindings, the generated consumer bindings of
#   plugins/vip).
if(NOT TARGET vip_bindings)
    message(FATAL_ERROR "vip_af needs the generated vip consumer bindings (plugins/vip/CMakeLists.txt, target vip_bindings)")
endif()

add_library(plugify-vip_af SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_af PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg/pisexcompat headers must not fail -Werror.
target_include_directories(plugify-vip_af SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include")
target_link_libraries(plugify-vip_af PRIVATE
        pisexcompat
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_af PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_af PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_af"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_af PROPERTIES OUTPUT_NAME vip_af)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_af PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_af.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_af.pplugin"
)

# --- tests --------------------------------------------------------------------
# Engine-free logic tests; run by ./test.sh (build/tests).
add_executable(vip_af_tests tests/vip_af_tests.cpp)
target_include_directories(vip_af_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(vip_af_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_af_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(vip_af_tests PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(vip_af_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
