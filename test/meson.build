# meson.build — ARA IPC standalone Linux test
#
# Configure and build from ARA_Library/:
#
#   cd ARA_Library
#   meson setup build/test test/
#   ninja -C build/test
#   build/test/ara-host-test

project('ara-ipc-test', 'c', 'cpp',
  default_options: [
    'cpp_std=c++20',
    'c_std=c11',
    'warning_level=1',
    'buildtype=debug',
  ]
)

# ---------------------------------------------------------------------------
# Include paths
#
# The source root passed to `meson setup` is ARA_Library/test/, so:
#   '..'      → ARA_Library/        (for "ARA_Library/IPC/..." includes)
#   '../..'   → ara-sdk/            (for "ARA_API/..." includes)
#
# Note: include_directories() accepts paths relative to the *source* root
# of the current project (i.e. the directory containing this meson.build).
# ---------------------------------------------------------------------------

ara_inc = include_directories(
  '..',         # ARA_Library/ — so '#include "ARA_Library/IPC/..."' resolves
  '../..',      # ara-sdk/     — so '#include "ARA_API/ARAInterface.h"' resolves
  '.',          # test/        — so '#include "test/SocketEncoder.h"' resolves
  '../../../vst3',  # VST3 SDK — so '#include "pluginterfaces/base/..."' resolves
                    # (used by ara_plugin_test_linux.cpp for IPluginFactory etc.)
)

ara_defines = ['-DARA_ENABLE_IPC=1']

# ---------------------------------------------------------------------------
# ARA Library sources shared by both binaries
# ---------------------------------------------------------------------------

ara_lib_sources = files(
  # IPC layer
  '../IPC/ARAIPCConnection.cpp',
  '../IPC/ARAIPCProxyHost.cpp',
  '../IPC/ARAIPCProxyPlugIn.cpp',
  # ARAIPCCFEncoding.cpp is Apple-only — do not include on Linux

  # Dispatch adapters (used by ProxyHost and ProxyPlugIn)
  '../Dispatch/ARAHostDispatch.cpp',
  '../Dispatch/ARAPlugInDispatch.cpp',

  # Utilities
  '../Utilities/ARAChannelFormat.cpp',
  '../Utilities/ARAPitchInterpretation.cpp',

  # Debug (C file)
  '../Debug/ARADebug.c',

  # VST3 SDK — FUnknown vtable + atomicAdd needed by IMainFactory ref-counting
  '../../../vst3/pluginterfaces/base/funknown.cpp',
  # Note: coreiids.cpp is NOT included here — ara_plugin_test_linux.cpp
  # defines the IIDs it needs via DEF_CLASS_IID directly.
)

ara_lib = static_library(
  'ara_ipc',
  ara_lib_sources,
  include_directories: ara_inc,
  c_args:   ara_defines,
  cpp_args: ara_defines,
)

# ---------------------------------------------------------------------------
# ara-plugin-test-linux  (native Linux plugin side — uses dlopen, no Wine)
# ---------------------------------------------------------------------------

executable(
  'ara-plugin-test-linux',
  files('ara_plugin_test_linux.cpp'),
  include_directories: ara_inc,
  cpp_args: ara_defines,
  link_with: ara_lib,
  link_args: ['-ldl'],
)

# ---------------------------------------------------------------------------
# ara-host-test  (parent process — host side)
# ---------------------------------------------------------------------------

executable(
  'ara-host-test',
  files('ara_host_test.cpp'),
  include_directories: ara_inc,
  cpp_args: ara_defines,
  link_with: ara_lib,
)
