// ara_plugin_test.cpp
// Plugin side (child process, runs under Wine).
// Receives two socket fds via argv as integers:
//   argv[1] = main-thread channel fd
//   argv[2] = other-threads channel fd
//   argv[3] = (optional) path to .vst3 file; defaults to Melodyne
//
// 1. Loads the real VST3 plugin via LoadLibraryA + GetPluginFactory.
// 2. Finds the ARA::IMainFactory class and obtains the real ARAFactory.
// 3. Registers that ARAFactory with ARAIPCProxyHostAddFactory.
// 4. Constructs an ARA::IPC::ProxyHost and serves messages.

// ---------------------------------------------------------------------------
// INCLUDE ORDER IS CRITICAL under wineg++:
//   1. C++ stdlib headers first  (avoid std::size_t / nullptr_t conflicts)
//   2. ARA + VST3 SDK headers    (they pull in more C++ stdlib transitively)
//   3. <windows.h> LAST          (Win32 headers must not precede C++ stdlib)
// ---------------------------------------------------------------------------

#define ARA_ENABLE_IPC 1

// -- (1) C++ stdlib ----------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>   // write, close (POSIX — available under Wine)

// -- (2) ARA SDK + VST3 SDK --------------------------------------------------
//   ARAVST3.h defines ARA_DISABLE_VST3_WARNINGS_BEGIN/_END, which we need
//   before we can use those macros.
#include "ARA_API/ARAVST3.h"                   // ARA::IMainFactory, kARAMainFactoryClass

ARA_DISABLE_VST3_WARNINGS_BEGIN
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"   // IPluginFactory, GetFactoryProc
ARA_DISABLE_VST3_WARNINGS_END

#include "ARA_Library/IPC/ARAIPCProxyHost.h"
#include "ARA_Library/IPC/ARAIPCConnection.h"
#include "ARA_Library/Debug/ARADebug.h"

#include "test/SocketChannel.h"
#include "test/SocketEncoder.h"

// -- (3) Win32 headers — needed by LoadLibraryA / GetProcAddress / HMODULE.
//   NOMINMAX must be set before windows.h to prevent min/max macro pollution.
//   The wine-shims/Windows.h in our -I path resolves the case-sensitive
//   #include <Windows.h> that some SDK headers still emit.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>   // OleInitialize / OleUninitialize

// ---------------------------------------------------------------------------
// IID definitions — exactly one DEF per interface used with FUnknownPtr
// or createInstance, in exactly one translation unit.
// ---------------------------------------------------------------------------
DEF_CLASS_IID (Steinberg::IPluginFactory)
DEF_CLASS_IID (ARA::IMainFactory)
DEF_CLASS_IID (ARA::IPlugInEntryPoint)
DEF_CLASS_IID (ARA::IPlugInEntryPoint2)

using namespace Steinberg;
using namespace ARA;

// ---------------------------------------------------------------------------
// VST3 plugin loading
// ---------------------------------------------------------------------------

typedef bool (PLUGIN_API *InitModuleProc) ();
typedef bool (PLUGIN_API *ExitModuleProc) ();

struct LoadedVST3 {
    HMODULE           module        { nullptr };
    IPluginFactory*   factory       { nullptr };
    IMainFactory*     mainFactory   { nullptr };  // kept alive to hold ARAFactory valid
    const ARAFactory* araFactory    { nullptr };
};

static LoadedVST3 g_vst3; // process-lifetime singleton

// Returns the ARAFactory* from a flat Windows .vst3 DLL.
// Follows the same sequence as the ARA SDK's VST3Loader.cpp (WIN32 path).
static const ARAFactory* loadARAFactory(const char* path)
{
    std::printf("[plugin] loading VST3: %s\n", path);
    std::fflush(stdout);

    // 1. Load the DLL.
    g_vst3.module = ::LoadLibraryA(path);
    if (!g_vst3.module) {
        std::fprintf(stderr, "[plugin] LoadLibraryA failed (error %lu)\n",
                     (unsigned long)::GetLastError());
        return nullptr;
    }

    // 2. Call InitDll (mandatory per VST3 spec).
    auto initProc = reinterpret_cast<InitModuleProc>(
        ::GetProcAddress(g_vst3.module, "InitDll"));
    if (!initProc) {
        std::fprintf(stderr, "[plugin] InitDll export not found\n");
        ::FreeLibrary(g_vst3.module); g_vst3.module = nullptr;
        return nullptr;
    }
    if (!initProc()) {
        std::fprintf(stderr, "[plugin] InitDll returned false\n");
        ::FreeLibrary(g_vst3.module); g_vst3.module = nullptr;
        return nullptr;
    }

    // 3. Get the plugin factory.
    auto factoryProc = reinterpret_cast<GetFactoryProc>(
        ::GetProcAddress(g_vst3.module, "GetPluginFactory"));
    if (!factoryProc) {
        std::fprintf(stderr, "[plugin] GetPluginFactory export not found\n");
        ::FreeLibrary(g_vst3.module); g_vst3.module = nullptr;
        return nullptr;
    }
    g_vst3.factory = factoryProc();
    if (!g_vst3.factory) {
        std::fprintf(stderr, "[plugin] GetPluginFactory returned nullptr\n");
        ::FreeLibrary(g_vst3.module); g_vst3.module = nullptr;
        return nullptr;
    }

    std::printf("[plugin] IPluginFactory obtained, scanning %d classes...\n",
                (int)g_vst3.factory->countClasses());
    std::fflush(stdout);

    // 4. Scan classes for kARAMainFactoryClass.
    for (int32 i = 0; i < g_vst3.factory->countClasses(); ++i) {
        PClassInfo info;
        if (g_vst3.factory->getClassInfo(i, &info) != kResultOk)
            continue;

        std::printf("[plugin]   class[%d]: category='%s' name='%s'\n",
                    (int)i, info.category, info.name);
        std::fflush(stdout);

        if (std::strcmp(info.category, kARAMainFactoryClass) != 0)
            continue;

        // 5. Instantiate the IMainFactory.
        IMainFactory* mainFactory = nullptr;
        tresult result = g_vst3.factory->createInstance(
            info.cid, IMainFactory::iid, reinterpret_cast<void**>(&mainFactory));

        if (result != kResultOk || !mainFactory) {
            std::fprintf(stderr, "[plugin] createInstance(IMainFactory) failed\n");
            continue;
        }

        // 6. Get the ARAFactory pointer.
        const ARAFactory* araFactory = mainFactory->getFactory();
        if (!araFactory) {
            std::fprintf(stderr, "[plugin] IMainFactory::getFactory returned nullptr\n");
            mainFactory->release();
            continue;
        }

        std::printf("[plugin] found ARAFactory: plugInName='%s' factoryID='%s'\n",
                    araFactory->plugInName ? araFactory->plugInName : "(null)",
                    araFactory->factoryID  ? araFactory->factoryID  : "(null)");
        std::fflush(stdout);

        // Keep the IMainFactory alive — its ARAFactory pointer is only valid
        // while the IMainFactory object is alive.
        g_vst3.mainFactory = mainFactory;  // ref already held by createInstance
        g_vst3.araFactory  = araFactory;
        return araFactory;
    }

    std::fprintf(stderr, "[plugin] no ARA::IMainFactory class found in VST3\n");
    return nullptr;
}

static void unloadVST3()
{
    if (g_vst3.mainFactory) { g_vst3.mainFactory->release(); g_vst3.mainFactory = nullptr; }
    g_vst3.araFactory = nullptr;
    if (g_vst3.factory)     { g_vst3.factory->release();     g_vst3.factory     = nullptr; }
    if (g_vst3.module) {
        auto exitProc = reinterpret_cast<ExitModuleProc>(
            ::GetProcAddress(g_vst3.module, "ExitDll"));
        if (exitProc) exitProc();
        ::FreeLibrary(g_vst3.module);
        g_vst3.module = nullptr;
    }
}

// ---------------------------------------------------------------------------
// ProxyHost subclass (base constructor is protected)
// ---------------------------------------------------------------------------

class TestProxyHost : public ARA::IPC::ProxyHost
{
public:
    static std::unique_ptr<TestProxyHost> create(int mainFd, int otherFd)
    {
        return std::unique_ptr<TestProxyHost>(new TestProxyHost(mainFd, otherFd));
    }

    ARA::IPC::Connection* connection() const {
        return ARA::IPC::RemoteCaller::getConnection();
    }

private:
    explicit TestProxyHost(int mainFd, int otherFd)
        : ARA::IPC::ProxyHost(buildConnection(this, mainFd, otherFd))
    {}

    static std::unique_ptr<ARA::IPC::Connection> buildConnection(
        TestProxyHost* self, int mainFd, int otherFd)
    {
        auto conn = std::make_unique<ARA::IPC::Connection>(
            SocketIPC::makeSocketEncoder,
            [self](const ARA::IPC::MessageID      msgID,
                   const ARA::IPC::MessageDecoder* decoder,
                   ARA::IPC::MessageEncoder*       replyEncoder)
            {
                self->handleReceivedMessage(msgID, decoder, replyEncoder);
            },
            /*receiverEndianessMatches=*/true
        );
        conn->setMainThreadChannel(
            std::make_unique<SocketIPC::SocketChannel>(mainFd));
        conn->setOtherThreadsChannel(
            std::make_unique<SocketIPC::SocketChannel>(otherFd));
        return conn;
    }
};

// ---------------------------------------------------------------------------
// Thread architecture for Wine/COM compatibility:
//
//   main() thread:
//     - Initializes COM as STA owner (OleInitialize)
//     - Runs the Windows message pump (GetMessage loop)
//     - This unblocks any COM cross-apartment calls Melodyne makes
//
//   pluginMainLoop thread (32 MB stack, COINIT_MULTITHREADED):
//     - Owns the ARA Connection (creation thread for IPC)
//     - Runs processPendingMessageOnCreationThreadIfNeeded in a loop
//     - When Melodyne's initializeARAWithConfiguration marshals COM calls
//       back to the STA, main()'s message pump services them
//
//   The main() thread signals the plugin thread via an Event when it's
//   time to quit (currently never — killed externally).
// ---------------------------------------------------------------------------

struct PluginThreadArgs {
    int    mainFd;
    int    otherFd;
    int    readyFd;    // Linux pipe fd — write 1 byte when proxy host is ready
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: ara_plugin_test <main_fd> <other_fd> [ready_fd] [vst3_path]\n");
        return 1;
    }

    int mainFd   = std::atoi(argv[1]);
    int otherFd  = std::atoi(argv[2]);
    int readyFd  = (argc >= 4) ? std::atoi(argv[3]) : -1;

    const char* vst3Path = (argc >= 5)
        ? argv[4]
        : "C:\\Program Files\\Common Files\\VST3\\Celemony\\Melodyne\\Melodyne.vst3";

    std::printf("[plugin] starting, main_fd=%d other_fd=%d\n", mainFd, otherFd);
    std::fflush(stdout);

    // Initialize COM on the REAL main thread first.
    // Stefan: "most ARA calls are required to be done on the main thread."
    // The Connection creation thread must be the main thread.
    // OleInitialize makes this the STA owner.
    ::OleInitialize(nullptr);
    // Force Win32 message queue creation on main thread before any other work.
    { MSG dummy; ::PeekMessageW(&dummy, nullptr, 0, 0, PM_NOREMOVE); }

    // Load VST3 on main thread (COM already initialized).
    const ARAFactory* araFactory = loadARAFactory(vst3Path);
    if (!araFactory) {
        std::fprintf(stderr, "[plugin] failed to load ARAFactory — aborting\n");
        ::OleUninitialize();
        return 1;
    }
    ARA::IPC::ARAIPCProxyHostAddFactory(araFactory);

    // Create the Connection and ProxyHost on the main thread.
    // This makes main() the "creation thread" for ARA IPC dispatch.
    auto proxyHost = TestProxyHost::create(mainFd, otherFd);

    std::printf("[plugin] proxy host ready, serving messages...\n");
    std::fflush(stdout);

    // Signal the host that we're ready.
    if (readyFd >= 0) {
        char byte = 1;
        ::write(readyFd, &byte, 1);
        ::close(readyFd);
    }

    // Run the ARA dispatch loop on the main thread.
    // processPendingMessageOnCreationThreadIfNeeded drains the
    // dispatchToCreationThread() queue.
    ARA::IPC::Connection* conn = proxyHost->connection();
    while (true) {
        // Pump Win32 messages so COM STA callbacks are serviced.
        MSG msg;
        while (::PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageA(&msg);
        }
        conn->processPendingMessageOnCreationThreadIfNeeded();
        ::usleep(1000);
    }

    ::OleUninitialize();
    unloadVST3();
    return 0;
}
