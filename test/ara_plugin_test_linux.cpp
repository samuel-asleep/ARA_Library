// ara_plugin_test_linux.cpp
// Plugin side — native Linux process, no Wine.
// Loads a Linux VST3 (.so) via dlopen, finds the ARA::IMainFactory,
// registers the real ARAFactory with ARAIPCProxyHostAddFactory, and
// serves IPC messages from the host.
//
// Usage:
//   ara_plugin_test_linux <main_fd> <other_fd> [ready_fd] [vst3_so_path]
//
// Default vst3_so_path: the ARAPluginDemo.so built from JUCE_ARA.

#define ARA_ENABLE_IPC 1

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <dlfcn.h>

// -- ARA + VST3 SDK ----------------------------------------------------------
#include "ARA_API/ARAVST3.h"

ARA_DISABLE_VST3_WARNINGS_BEGIN
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
ARA_DISABLE_VST3_WARNINGS_END

#include "ARA_Library/IPC/ARAIPCProxyHost.h"
#include "ARA_Library/IPC/ARAIPCConnection.h"
#include "ARA_Library/Debug/ARADebug.h"

#include "test/SocketChannel.h"
#include "test/SocketEncoder.h"

// ---------------------------------------------------------------------------
// IID definitions (one per TU)
// ---------------------------------------------------------------------------
DEF_CLASS_IID (Steinberg::IPluginFactory)
DEF_CLASS_IID (ARA::IMainFactory)
DEF_CLASS_IID (ARA::IPlugInEntryPoint)
DEF_CLASS_IID (ARA::IPlugInEntryPoint2)

using namespace Steinberg;
using namespace ARA;

// ---------------------------------------------------------------------------
// Linux VST3 entry points (POSIX, no Win32)
// ---------------------------------------------------------------------------
typedef bool (*ModuleEntryFunc)(void* moduleHandle);
typedef bool (*ModuleExitFunc)();

struct LoadedVST3 {
    void*             handle      { nullptr };
    IPluginFactory*   factory     { nullptr };
    IMainFactory*     mainFactory { nullptr };
    const ARAFactory* araFactory  { nullptr };
};
static LoadedVST3 g_vst3;

static const ARAFactory* loadARAFactory(const char* soPath)
{
    std::printf("[plugin] loading VST3: %s\n", soPath);
    std::fflush(stdout);

    g_vst3.handle = ::dlopen(soPath, RTLD_LAZY | RTLD_GLOBAL);
    if (!g_vst3.handle) {
        std::fprintf(stderr, "[plugin] dlopen failed: %s\n", ::dlerror());
        return nullptr;
    }

    // ModuleEntry is optional on Linux but preferred
    auto moduleEntry = reinterpret_cast<ModuleEntryFunc>(::dlsym(g_vst3.handle, "ModuleEntry"));
    if (moduleEntry) {
        if (!moduleEntry(g_vst3.handle)) {
            std::fprintf(stderr, "[plugin] ModuleEntry returned false\n");
            ::dlclose(g_vst3.handle); g_vst3.handle = nullptr;
            return nullptr;
        }
    }

    auto factoryProc = reinterpret_cast<GetFactoryProc>(::dlsym(g_vst3.handle, "GetPluginFactory"));
    if (!factoryProc) {
        std::fprintf(stderr, "[plugin] GetPluginFactory not found: %s\n", ::dlerror());
        ::dlclose(g_vst3.handle); g_vst3.handle = nullptr;
        return nullptr;
    }
    g_vst3.factory = factoryProc();
    if (!g_vst3.factory) {
        std::fprintf(stderr, "[plugin] GetPluginFactory returned nullptr\n");
        ::dlclose(g_vst3.handle); g_vst3.handle = nullptr;
        return nullptr;
    }

    std::printf("[plugin] scanning %d classes...\n", (int)g_vst3.factory->countClasses());
    std::fflush(stdout);

    for (int32 i = 0; i < g_vst3.factory->countClasses(); ++i) {
        PClassInfo info;
        if (g_vst3.factory->getClassInfo(i, &info) != kResultOk) continue;

        std::printf("[plugin]   class[%d]: category='%s' name='%s'\n",
                    (int)i, info.category, info.name);
        std::fflush(stdout);

        if (std::strcmp(info.category, kARAMainFactoryClass) != 0) continue;

        IMainFactory* mf = nullptr;
        tresult r = g_vst3.factory->createInstance(
            info.cid, IMainFactory::iid, reinterpret_cast<void**>(&mf));
        if (r != kResultOk || !mf) {
            std::fprintf(stderr, "[plugin] createInstance(IMainFactory) failed\n");
            continue;
        }

        const ARAFactory* af = mf->getFactory();
        if (!af) {
            std::fprintf(stderr, "[plugin] IMainFactory::getFactory returned nullptr\n");
            mf->release();
            continue;
        }

        std::printf("[plugin] found ARAFactory: plugInName='%s' factoryID='%s'\n",
                    af->plugInName ? af->plugInName : "(null)",
                    af->factoryID  ? af->factoryID  : "(null)");
        std::fflush(stdout);

        g_vst3.mainFactory = mf;
        g_vst3.araFactory  = af;
        return af;
    }

    std::fprintf(stderr, "[plugin] no ARA::IMainFactory found\n");
    return nullptr;
}

static void unloadVST3()
{
    if (g_vst3.mainFactory) { g_vst3.mainFactory->release(); g_vst3.mainFactory = nullptr; }
    g_vst3.araFactory = nullptr;
    if (g_vst3.factory) { g_vst3.factory->release(); g_vst3.factory = nullptr; }
    if (g_vst3.handle) {
        auto moduleExit = reinterpret_cast<ModuleExitFunc>(::dlsym(g_vst3.handle, "ModuleExit"));
        if (moduleExit) moduleExit();
        ::dlclose(g_vst3.handle);
        g_vst3.handle = nullptr;
    }
}

// ---------------------------------------------------------------------------
// ProxyHost subclass
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
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: ara_plugin_test_linux <main_fd> <other_fd> [ready_fd] [vst3_so_path]\n");
        return 1;
    }

    int mainFd  = std::atoi(argv[1]);
    int otherFd = std::atoi(argv[2]);
    int readyFd = (argc >= 4) ? std::atoi(argv[3]) : -1;

    // Default: ARAPluginDemo built from JUCE_ARA
    const char* soPath = (argc >= 5)
        ? argv[4]
        : "ARAPluginDemo-VST3-linux-debug/examples/Plugins/ARAPluginDemo_artefacts/"
          "Debug/VST3/ARAPluginDemo.vst3/Contents/x86_64-linux/ARAPluginDemo.so";

    std::printf("[plugin] starting (native Linux), main_fd=%d other_fd=%d\n",
                mainFd, otherFd);
    std::fflush(stdout);

    const ARAFactory* araFactory = loadARAFactory(soPath);
    if (!araFactory) {
        std::fprintf(stderr, "[plugin] failed to load ARAFactory\n");
        return 1;
    }
    ARA::IPC::ARAIPCProxyHostAddFactory(araFactory);

    // Connection and dispatch loop on the main thread — no COM, no Wine.
    auto proxyHost = TestProxyHost::create(mainFd, otherFd);

    std::printf("[plugin] proxy host ready\n");
    std::fflush(stdout);

    if (readyFd >= 0) {
        char byte = 1;
        ::write(readyFd, &byte, 1);
        ::close(readyFd);
    }

    ARA::IPC::Connection* conn = proxyHost->connection();
    while (true) {
        conn->processPendingMessageOnCreationThreadIfNeeded();
        ::usleep(1000);
    }

    unloadVST3();
    return 0;
}
