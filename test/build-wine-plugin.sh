#!/usr/bin/env bash
# build-wine-plugin.sh
# Compiles ara-plugin-test as a Wine PE (.exe.so) using wineg++.
#
# Run from ARA_Library/:
#   bash test/build-wine-plugin.sh
#
# wineg++ always names the output <stem>.exe.so regardless of -o flag,
# so "-o build/test/ara-plugin-test" produces build/test/ara-plugin-test.exe.so
# which Wine runs as a normal Windows executable.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARA_LIB="${SCRIPT_DIR}/.."                  # ARA_Library/
ARA_SDK="${ARA_LIB}/.."                     # ara-sdk/ (contains ARA_API/ and ARA_Library/)
VST3_SDK="/home/samuel/repos/yabridge/subprojects/vst3"
OUT_DIR="${ARA_LIB}/build/test"

mkdir -p "${OUT_DIR}"

SOURCES=(
    "${SCRIPT_DIR}/ara_plugin_test.cpp"

    # ARA IPC layer
    "${ARA_LIB}/IPC/ARAIPCConnection.cpp"
    "${ARA_LIB}/IPC/ARAIPCProxyHost.cpp"
    "${ARA_LIB}/IPC/ARAIPCProxyPlugIn.cpp"

    # Dispatch adapters (used by ProxyHost/ProxyPlugIn)
    "${ARA_LIB}/Dispatch/ARAHostDispatch.cpp"
    "${ARA_LIB}/Dispatch/ARAPlugInDispatch.cpp"

    # Utilities
    "${ARA_LIB}/Utilities/ARAChannelFormat.cpp"
    "${ARA_LIB}/Utilities/ARAPitchInterpretation.cpp"

    # Debug
    "${ARA_LIB}/Debug/ARADebug.c"

    # VST3 SDK — funknown.cpp provides FUnknown vtable methods needed by IPtr/FUnknownPtr.
    # coreiids.cpp is NOT included — we define the IIDs we need via DEF_CLASS_IID in ara_plugin_test.cpp.
    "${VST3_SDK}/pluginterfaces/base/funknown.cpp"
)

INCLUDES=(
    # Must come first: case-fix shims for Wine (Windows.h -> windows.h etc.)
    "-I${SCRIPT_DIR}/wine-shims"
    "-I${ARA_SDK}"       # resolves "ARA_Library/..." and "ARA_API/..."
    "-I${VST3_SDK}"      # resolves "pluginterfaces/..." and "public.sdk/..."
    "-I${SCRIPT_DIR}"    # resolves "test/SocketEncoder.h" etc.
    "-I${ARA_LIB}"       # resolves "test/..." from ARA_Library root
)

DEFINES=(
    "-DARA_ENABLE_IPC=1"
    "-DWIN32_LEAN_AND_MEAN"
)

FLAGS=(
    "-std=c++20"
    "-m64"
    "-O0" "-g"
    "-Wall"
    "-Wno-deprecated-declarations"
    "-Wno-unused-function"
    # Force-include the Wine compat shim before ARADebug.c sees _WIN32
    "-include" "${SCRIPT_DIR}/wine-shims/wine_debug_compat.h"
)

echo "=== Compiling ara-plugin-test (Wine PE) ==="
echo "Output will be: ${OUT_DIR}/ara-plugin-test.exe.so"

wineg++ \
    "${FLAGS[@]}" \
    "${INCLUDES[@]}" \
    "${DEFINES[@]}" \
    "${SOURCES[@]}" \
    -o "${OUT_DIR}/ara-plugin-test" \
    -lpthread \
    -lole32

# wineg++ produces <stem>.exe.so — verify it exists
if [[ -f "${OUT_DIR}/ara-plugin-test.exe.so" ]]; then
    echo "=== Build succeeded ==="
    ls -lh "${OUT_DIR}/ara-plugin-test.exe.so"

    # Create a launcher script named "ara-plugin-test" (no extension) that
    # ara-host-test will exec directly. This script invokes wine on the .exe.so.
    LAUNCHER="${OUT_DIR}/ara-plugin-test"
    # Absolute path to the preload shim (stays valid regardless of cwd)
    SHIM_SO="${ARA_LIB}/test/wine-shims/no_rt_threads.so"
    cat > "${LAUNCHER}" << LAUNCHEREOF
#!/usr/bin/env bash
# Launcher: runs ara-plugin-test.exe.so under Wine (64-bit), forwarding all arguments.
# LD_PRELOAD strips SCHED_FIFO thread attrs that fail under Wine without
# CAP_SYS_NICE, preventing Melodyne's background threads from crashing.
# Uses wine64 explicitly to avoid 32-bit wine trying to load 64-bit PE.
DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
exec env LD_PRELOAD="${SHIM_SO}" \\
     /usr/lib/wine/wine64 "\${DIR}/ara-plugin-test.exe.so" "\$@"
LAUNCHEREOF
    chmod +x "${LAUNCHER}"
    echo "Launcher: ${LAUNCHER}"
else
    echo "ERROR: expected ${OUT_DIR}/ara-plugin-test.exe.so not found"
    exit 1
fi
