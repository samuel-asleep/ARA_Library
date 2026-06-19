// ara_host_test.cpp
// Host side (parent process).
//
// 1. Creates two socketpair()s  (main-thread channel + other-threads channel).
// 2. fork()s; child exec()s ara_plugin_test with the plugin-side fds as argv.
// 3. Closes plugin-side fds in the parent.
// 4. Constructs an ARA::IPC::ProxyPlugIn around a Connection.
// 5. Calls ARAIPCProxyPlugInGetFactoriesCount / GetFactoryAtIndex /
//    ARAIPCProxyPlugInInitializeARA.
// 6. Prints the result, waits for child, exits.

#define ARA_ENABLE_IPC 1

#include "ARA_Library/IPC/ARAIPCProxyPlugIn.h"
#include "ARA_Library/IPC/ARAIPCConnection.h"
#include "test/SocketChannel.h"
#include "test/SocketEncoder.h"

#include "ARA_API/ARAInterface.h"
#include "ARA_Library/Debug/ARADebug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace ARA;

// ---------------------------------------------------------------------------
// Build the Connection for the host side
// ---------------------------------------------------------------------------

static std::unique_ptr<ARA::IPC::Connection> makeHostConnection(
    int mainFd, int otherFd)
{
    // On Linux there is no run-loop.  When MainThreadMessageDispatcher blocks
    // waiting for a reply, it uses WaitableSingleMessageQueue (a
    // binary_semaphore).  The background SocketChannel thread calls
    // routeReceivedMessage() → MainThreadMessageDispatcher::routeReceivedMessage()
    // which signals the semaphore or dispatches to the creation thread via
    // dispatchToCreationThread().  For the latter case we provide a
    // waitForMessageDelegate that drains any pending dispatched functions by
    // calling processPendingMessageOnCreationThreadIfNeeded().
    //
    // We use a raw pointer here because the Connection outlives the lambda.
    ARA::IPC::Connection* connPtr = nullptr;

    auto conn = std::make_unique<ARA::IPC::Connection>(
        /*messageEncoderFactory  =*/ SocketIPC::makeSocketEncoder,
        /*messageHandler         =*/ ARA::IPC::ProxyPlugIn::handleReceivedMessage,
        /*receiverEndianessMatches=*/ true,
        /*waitForMessageDelegate =*/
        [&connPtr]() {
            if (connPtr)
                connPtr->processPendingMessageOnCreationThreadIfNeeded();
        }
    );
    connPtr = conn.get();

    conn->setMainThreadChannel(
        std::make_unique<SocketIPC::SocketChannel>(mainFd));
    conn->setOtherThreadsChannel(
        std::make_unique<SocketIPC::SocketChannel>(otherFd));

    return conn;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Determine path to ara_plugin_test binary.
    // Prefer ara-plugin-test-linux (native, no Wine) over ara-plugin-test (Wine).
    // Allow override via environment variable ARA_PLUGIN_TEST_BIN.
    std::string pluginBin;
    if (const char* env = std::getenv("ARA_PLUGIN_TEST_BIN")) {
        pluginBin = env;
    } else if (argc >= 1 && argv[0]) {
        std::string self(argv[0]);
        auto pos = self.rfind("ara-host-test");
        if (pos != std::string::npos) {
            // Try the native Linux binary first
            std::string linuxBin = self.substr(0, pos) + "ara-plugin-test-linux";
            if (::access(linuxBin.c_str(), X_OK) == 0)
                pluginBin = linuxBin;
            else
                pluginBin = self.substr(0, pos) + "ara-plugin-test";
        } else {
            pluginBin = "./ara-plugin-test-linux";
        }
    } else {
        pluginBin = "./ara-plugin-test-linux";
    }

    std::printf("[host] using plugin binary: %s\n", pluginBin.c_str());
    std::fflush(stdout);

    // 1. Create two socketpairs + a readiness pipe.
    //    sv[0] = host side, sv[1] = plugin side.
    //    ready[0] = host reads, ready[1] = plugin writes one byte when ready.
    int mainSv[2], otherSv[2], ready[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, mainSv) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, otherSv) != 0 ||
        ::pipe(ready) != 0) {
        std::perror("socketpair/pipe");
        return 1;
    }

    // 2. fork().
    pid_t child = ::fork();
    if (child < 0) {
        std::perror("fork");
        return 1;
    }

    if (child == 0) {
        // -------------------------------------------------------------------
        // Child process → exec ara_plugin_test
        // -------------------------------------------------------------------
        ::close(mainSv[0]);
        ::close(otherSv[0]);
        ::close(ready[0]); // child only writes

        char mainArg[32], otherArg[32], readyArg[32];
        std::snprintf(mainArg,  sizeof(mainArg),  "%d", mainSv[1]);
        std::snprintf(otherArg, sizeof(otherArg), "%d", otherSv[1]);
        std::snprintf(readyArg, sizeof(readyArg), "%d", ready[1]);

        ::execlp(pluginBin.c_str(), pluginBin.c_str(),
                 mainArg, otherArg, readyArg, (char*)nullptr);

        std::perror("execlp");
        _exit(127);
    }

    // -------------------------------------------------------------------
    // Parent process (host)
    // -------------------------------------------------------------------
    ::close(mainSv[1]);
    ::close(otherSv[1]);
    ::close(ready[1]); // host only reads

    std::printf("[host] child pid=%d, waiting for plugin ready signal...\n", (int)child);
    std::fflush(stdout);

    // Block until plugin signals readiness (or child dies).
    char readyByte = 0;
    ssize_t n = ::read(ready[0], &readyByte, 1);
    ::close(ready[0]);
    if (n <= 0) {
        std::fprintf(stderr, "[host] plugin never became ready (read=%zd)\n", n);
        ::kill(child, SIGTERM);
        ::waitpid(child, nullptr, 0);
        return 1;
    }
    std::printf("[host] plugin ready, building connection...\n");
    std::fflush(stdout);

    // 3. Build the connection and proxy.
    auto conn = makeHostConnection(mainSv[0], otherSv[0]);
    auto proxyPlugIn = std::make_unique<ARA::IPC::ProxyPlugIn>(std::move(conn));

    // toIPCRef() is generated by ARA_MAP_IPC_REF inside ARAIPCProxyPlugIn.cpp
    // as a static free function — it's not accessible from other TUs.
    // Replicate the same reinterpret_cast the macro performs.
    auto proxyRef = reinterpret_cast<ARA::IPC::ARAIPCProxyPlugInRef>(proxyPlugIn.get());

    // 4. Query factory count.
    std::printf("[host] querying factory count...\n");
    std::fflush(stdout);

    size_t count = ARA::IPC::ARAIPCProxyPlugInGetFactoriesCount(proxyRef);
    std::printf("[host] factory count = %zu\n", count);
    std::fflush(stdout);

    if (count == 0) {
        std::fprintf(stderr, "[host] ERROR: no factories\n");
        ::kill(child, SIGTERM);
        ::waitpid(child, nullptr, 0);
        return 1;
    }

    // 5. Fetch factory at index 0.
    const ARAFactory* factory = ARA::IPC::ARAIPCProxyPlugInGetFactoryAtIndex(proxyRef, 0);
    std::printf("[host] factory name = %s\n",
                factory && factory->plugInName ? factory->plugInName : "(null)");
    std::fflush(stdout);

    // 6. InitializeARA.
    std::printf("[host] calling initializeARA...\n");
    std::fflush(stdout);

    ARA::IPC::ARAIPCProxyPlugInInitializeARA(
        proxyRef,
        factory->factoryID,
        kARAAPIGeneration_2_0_Final);

    std::printf("[host] initializeARA returned\n");
    std::fflush(stdout);

    // 7. Uninitialize + clean up.
    ARA::IPC::ARAIPCProxyPlugInUninitializeARA(proxyRef, factory->factoryID);

    std::printf("[host] initializeARA + uninitializeARA complete\n");
    std::fflush(stdout);

    // Brief pause to let the uninitializeARA reply route through the
    // receive thread before we destroy the Connection and join the thread.
    ::usleep(100'000); // 100ms

    std::printf("[host] done — destroying proxy and waiting for child\n");
    std::fflush(stdout);

    // Destroy the proxy (and connection), which closes the fds.
    proxyPlugIn.reset();

    // Kill and reap child.
    ::kill(child, SIGTERM);
    int status = 0;
    ::waitpid(child, &status, 0);
    std::printf("[host] child exited with status %d\n", WEXITSTATUS(status));

    return 0;
}
