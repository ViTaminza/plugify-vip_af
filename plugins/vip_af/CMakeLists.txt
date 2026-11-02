# vip_af — Plugify C++ plugin: port of the Pisex VIP module "[VIP] Anti Flash".
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no s2guard. All safety rules (map
# gate, slot/controller/pawn checks, the controller team read, the map-gated schema offsets, the
# validated pawn handle of the flash write) live in src/plugin.cpp.
#
#   plugify-vip_af  the plugin shared library (entry "bin/vip_af" -> bin/libvip_af.so)
#   vip_af_tests    engine-free unit tests: the anti-flash mode table, plus the map gate/guards/
#                   event path/flash write driven through fake s2sdk/vip bindings
#                   (build/tests/vip_af_tests)

set(VIP_AF_VERSION "1.2.1")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*   (s2sdk_bindings)
#   __vip_*     (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
# In the monorepo plugins/vip is added first ("vip" < "vip_af"), so vip_bindings already exists; a
# standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-vip_af SHARED
        src/plugin.cpp
        # The __s2sdk_* / __vip_* binding globals the plugify module fills from the manifest's
        # dependencies; compiled in, never linked as a library.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_af PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_af SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_af PRIVATE
        plugify_sdk
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

# --- tests --------------------------------------------------------------------------------------
# vip_af_tests: no engine. The plugin TU is compiled into the test (tests/vip_af_tests.cpp includes
# ../src/plugin.cpp) and the __s2sdk_* / __vip_* binding globals are recording fakes: the mode
# table, the map gate (B1/B2), IsConnected/IsAlive (C2), the map-cached schema offsets (C8), the
# whole player_blind decision and the validated flash write (C1/C12), and a clean unload (A7).
# No pisexcompat.
add_executable(vip_af_tests
        tests/vip_af_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(vip_af_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vip_af_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(vip_af_tests PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(vip_af_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_af_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_af"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(vip_af_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
