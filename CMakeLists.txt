cmake_minimum_required(VERSION 3.28)
project(dlss5-sidecar LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Windows-only project (spec: Global Constraints).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# I12: no obfuscation, no packing, and a plain user-level manifest.
add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

include(FetchContent)
FetchContent_Declare(catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1)
FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5)
FetchContent_MakeAvailable(catch2 tomlplusplus imgui)

# Our project-wide _UNICODE (above) would otherwise select Catch2's wmain entry
# point, which MSVC cannot auto-detect from inside a static library. This is
# Catch2's own documented opt-out.
target_compile_definitions(Catch2WithMain PRIVATE DO_NOT_USE_WMAIN)

include(cmake/Invariants.cmake)

# Shaders are compiled to headers at build time so nothing is compiled at
# runtime and no .cso files need shipping.
find_program(DXC_EXECUTABLE dxc
  HINTS "$ENV{WindowsSdkVerBinPath}x64"
        "$ENV{WindowsSdkDir}bin/x64"
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "$ENV{ProgramFiles}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(NOT DXC_EXECUTABLE)
  message(FATAL_ERROR "dxc not found. Install the Windows SDK.")
endif()

set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/generated/gpu/Shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

function(compile_compute_shader name)
  add_custom_command(
    OUTPUT ${SHADER_OUT_DIR}/${name}.cs.h
    COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSMain
            -Fh ${SHADER_OUT_DIR}/${name}.cs.h
            -Vn g_${name}CS
            ${CMAKE_SOURCE_DIR}/src/common/gpu/Shaders/${name}.hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/src/common/gpu/Shaders/${name}.hlsl
    COMMENT "Compiling compute shader ${name}"
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${SHADER_OUT_DIR}/${name}.cs.h PARENT_SCOPE)
endfunction()

compile_compute_shader(FormatNormalize)
compile_compute_shader(FlowToMotionVec)
compile_compute_shader(LuminanceR8)
compile_compute_shader(UiMaskBlend)
compile_compute_shader(NrEncode)
compile_compute_shader(NrResolve)
compile_compute_shader(NrCompose)
compile_compute_shader(HdrToSdr)
compile_compute_shader(HdrCompose)

add_library(sidecar_common STATIC)
# src/common for the library's own headers; src so the forwarder's ABI header
# is reachable as forwarder/nvngx_forwarder_abi.h from both sides.
target_include_directories(sidecar_common PUBLIC src/common src)
target_link_libraries(sidecar_common PUBLIC
  d3d11 d3d12 dxgi dxguid dcomp windowsapp bcrypt tomlplusplus::tomlplusplus)
target_sources(sidecar_common PRIVATE
  src/common/core/GpuProfile.cpp
  src/common/core/LatencyStats.cpp
  src/common/core/Config.cpp
  src/common/core/ControlChannel.cpp
  src/common/core/Hotkeys.cpp
  src/common/core/Text.cpp
  src/common/core/Log.cpp
  src/common/core/ImageDump.cpp
  src/common/core/UiCalibration.cpp
  src/common/core/Sha256.cpp
  src/common/core/PanicSwitch.cpp
  src/common/gpu/DeviceBridge.cpp
  src/common/gpu/FormatNormalize.cpp
  src/common/gpu/Luminance.cpp
  src/common/gpu/UiMask.cpp
  src/common/gpu/NrColorBridge.cpp
  src/common/gpu/HdrBridge.cpp
  src/common/flow/NvofaFlow.cpp
  src/common/flow/FlowToMotionVec.cpp
  src/common/capture/WgcSource.cpp
  src/common/present/DCompOverlay.cpp
  src/common/present/WindowTracker.cpp
  src/common/present/HdrDisplay.cpp
  src/common/present/Hud.cpp
  src/common/neural/PassthroughPass.cpp
  src/common/neural/RuntimeManifest.cpp
  src/common/neural/NgxSession.cpp
  src/common/neural/NrForwarder.cpp
  src/common/neural/DirectNrPass.cpp
  src/common/neural/NeuralPassFactory.cpp)

# The NVIDIA Optical Flow SDK is a manual download and is never vendored (I11).
# Without it the project still builds; NvofaFlow reports Available() false and
# the pipeline runs on a zero motion field.
set(NVOF_SDK_DIR "" CACHE PATH "NVIDIA Optical Flow SDK root")
if(NVOF_SDK_DIR)
  # Quoted: the SDK is often unpacked under a path containing a space.
  target_include_directories(sidecar_common PUBLIC "${NVOF_SDK_DIR}/NvOFInterface")
  target_compile_definitions(sidecar_common PUBLIC SIDECAR_HAVE_NVOF=1)
else()
  message(WARNING "NVOF_SDK_DIR not set; optical flow will report unavailable.")
  target_compile_definitions(sidecar_common PUBLIC SIDECAR_HAVE_NVOF=0)
endif()

# The NVIDIA DLSS SDK, on the same terms as the Optical Flow SDK: a manual
# download, never vendored (I11), and optional. Without it NgxSession compiles
# to a stub that reports why it is unavailable, and the neural pass falls back
# to passthrough.
#
# Linking nvsdk_ngx_d.lib adds no DLL import -- it resolves _nvngx.dll with
# LoadLibrary at first use, verified against ci/check_imports.py on the spike
# binaries. So the shipped executables still pass the import-table check and
# still load on a machine with no NGX core.
set(DLSS_SDK_DIR "" CACHE PATH "NVIDIA DLSS SDK root (github.com/NVIDIA/DLSS)")
if(DLSS_SDK_DIR)
  target_include_directories(sidecar_common PUBLIC "${DLSS_SDK_DIR}/include")
  target_compile_definitions(sidecar_common PUBLIC SIDECAR_HAVE_NGX=1)
  # The SDK ships one library per CRT flavour; mixing them is a hard link error
  # about _ITERATOR_DEBUG_LEVEL rather than anything subtle.
  target_link_libraries(sidecar_common PUBLIC
    "$<$<CONFIG:Debug>:${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>")
else()
  message(WARNING "DLSS_SDK_DIR not set; the neural pass will report unavailable.")
  target_compile_definitions(sidecar_common PUBLIC SIDECAR_HAVE_NGX=0)
endif()

# Generated shader headers, listed as sources so the custom commands run.
target_sources(sidecar_common PRIVATE ${SHADER_HEADERS})
target_include_directories(sidecar_common PUBLIC ${CMAKE_BINARY_DIR}/generated)
# d3dcompiler is deliberately not linked: shaders are compiled by dxc at build
# time and D3D12SerializeRootSignature lives in d3d12.dll, so linking it would
# add a DLL dependency this binary never calls.

# C++/WinRT needs the Windows Runtime headers and coroutine support.
target_compile_options(sidecar_common PUBLIC /await:strict)
target_link_libraries(sidecar_common PUBLIC windowsapp)

add_executable(sidecar WIN32
  src/runtime/main.cpp
  src/runtime/Pipeline.cpp)
target_include_directories(sidecar PRIVATE src)
target_link_libraries(sidecar PRIVATE sidecar_common shell32)
add_invariant_check(sidecar)

# The neural-rendering forwarder. The runtime accepts calls only from a module
# whose path contains "nvngx.dll" (see src/forwarder/nvngx_forwarder.cpp), so
# this library is named for exactly that and does nothing else. It is ours, so
# it goes through the same import-table check as the executables; it needs no
# SDK and builds unconditionally.
add_library(nvngx_forwarder SHARED src/forwarder/nvngx_forwarder.cpp)
set_target_properties(nvngx_forwarder PROPERTIES
  OUTPUT_NAME "nvngx.dll_sidecar"
  PREFIX ""
  SUFFIX ".dll")
target_include_directories(nvngx_forwarder PRIVATE src)
add_invariant_check(nvngx_forwarder)
# Beside the runtime, since that is where it is looked for.
add_dependencies(sidecar nvngx_forwarder)
add_custom_command(TARGET sidecar POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nvngx_forwarder> $<TARGET_FILE_DIR:sidecar>
  VERBATIM)

add_executable(sidecar-manager WIN32
  src/manager/main.cpp
  src/manager/Install.cpp
  src/manager/Probes.cpp
  src/manager/Theme.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(sidecar-manager PRIVATE
  src ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(sidecar-manager PRIVATE sidecar_common shell32 comdlg32 dwmapi)

# Delay-load DXGI and D3D11 in the manager and pin the System32 copies first
# (PinSystemGraphicsDlls), so a proxy DLL of either name left beside the
# executables by some other tool is never consulted by the control panel.
target_link_options(sidecar-manager PRIVATE /DELAYLOAD:dxgi.dll /DELAYLOAD:d3d11.dll)
target_link_libraries(sidecar-manager PRIVATE delayimp)
add_invariant_check(sidecar-manager)

add_executable(testpattern WIN32
  src/testpattern/main.cpp
  src/testpattern/Pattern.cpp)
target_link_libraries(testpattern PRIVATE d3d11 dxgi)
add_invariant_check(testpattern)

# Throwaway spike harness for the M3 plan, Task 3. Deliberately not part of the
# product: it is not linked into sidecar_common, not shipped, and not built
# unless someone points DLSS_SDK_DIR at a checkout of github.com/NVIDIA/DLSS.
# CI has no such checkout and simply skips it.
#
# It is also exempt from the import-table check on purpose. The check exists to
# prove what the *shipped* binaries do not do; extending it to a scratch tool
# that never leaves this machine would say nothing useful.
if(DLSS_SDK_DIR)
  add_executable(spike_ngx tools/spike_ngx/main.cpp)
  target_include_directories(spike_ngx PRIVATE "${DLSS_SDK_DIR}/include")
  # The SDK ships one library per CRT flavour; mixing them is a hard link
  # error about _ITERATOR_DEBUG_LEVEL rather than anything subtle.
  target_link_libraries(spike_ngx PRIVATE
    d3d12 dxgi
    "$<$<CONFIG:Debug>:${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>")

  # Second spike, same terms. spike_ngx asked the NGX core for a feature and was
  # refused; this one skips the core and calls nvngx_dlssnr.dll's own exported
  # entry points. It links the SDK library only to obtain a parameter block --
  # every feature call it makes is resolved with GetProcAddress at runtime.
  add_executable(spike_snippet tools/spike_snippet/main.cpp)
  target_include_directories(spike_snippet PRIVATE "${DLSS_SDK_DIR}/include")
  target_link_libraries(spike_snippet PRIVATE
    d3d12 dxgi
    "$<$<CONFIG:Debug>:${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib>")
endif()

# Which window styles actually make a DirectComposition overlay click-through
# for another process's hit test. Needs no SDK, so it builds unconditionally.
add_executable(spike_clickthrough tools/spike_clickthrough/main.cpp)
target_link_libraries(spike_clickthrough PRIVATE d3d11 d3d12 dxgi dcomp)

enable_testing()
add_subdirectory(tests)
