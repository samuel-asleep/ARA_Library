// SocketChannel.h
// ARA::IPC::MessageChannel backed by a Unix domain socketpair fd.
//
// Wire frame over the fd:
//   [int32_t  messageID ]  (4 bytes, LE)
//   [uint32_t payloadLen]  (4 bytes, LE)
//   [uint8_t  payload[payloadLen]]
//
// A background thread is started on construction that reads frames and calls
// routeReceivedMessage().  Because routing happens on the background thread:
//   receivesMessagesOnCurrentThread() → false  (creation thread never owns the fd)
//   waitForMessageOnCurrentThread()   → false  (never called by Connection)
//
// The Connection's MainThreadMessageDispatcher then uses the
// WaitableSingleMessageQueue semaphore path to forward messages to the
// creation thread via dispatchToCreationThread().

#pragma once

#define ARA_ENABLE_IPC 1
#include "ARA_Library/IPC/ARAIPCConnection.h"
#include "test/SocketEncoder.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

namespace SocketIPC {

// ---------------------------------------------------------------------------
// Low-level frame I/O
// ---------------------------------------------------------------------------

namespace detail {

// Write exactly `n` bytes; returns false on error.
inline bool writeAll(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::write(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

// Read exactly `n` bytes; returns false on error / EOF.
inline bool readAll(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

inline uint32_t packU32(uint32_t v) { return v; } // native LE assumed (same-arch)

} // namespace detail

// ---------------------------------------------------------------------------
// SocketChannel
// ---------------------------------------------------------------------------

class SocketChannel : public ARA::IPC::MessageChannel {
public:
    // Takes ownership of fd. Starts the receive thread immediately.
    explicit SocketChannel(int fd)
        : _fd(fd)
    {
        _recvThread = std::thread([this] { _receiveLoop(); });
    }

    ~SocketChannel() override {
        _stop.store(true, std::memory_order_release);
        ::shutdown(_fd, SHUT_RDWR);
        if (_recvThread.joinable()) {
            try {
                _recvThread.join();
            } catch (const std::system_error& e) {
                std::fprintf(stderr, "[SocketChannel fd=%d] join error: %s\n",
                             _fd, e.what());
            }
        }
        ::close(_fd);
    }

    // -----------------------------------------------------------------------
    // MessageChannel interface
    // -----------------------------------------------------------------------

    // Write a frame: [messageID 4B][payloadLen 4B][payload].
    void sendMessage(ARA::IPC::MessageID messageID,
                     std::unique_ptr<ARA::IPC::MessageEncoder>&& encoder) override
    {
        auto payload = serializeEncoder(static_cast<const SocketEncoder&>(*encoder));

        uint32_t hdr[2];
        hdr[0] = (uint32_t)messageID;
        hdr[1] = (uint32_t)payload.size();

        std::lock_guard<std::mutex> lk(_sendMutex);
        detail::writeAll(_fd, hdr, 8);
        if (!payload.empty())
            detail::writeAll(_fd, payload.data(), payload.size());
    }

    // The background receive thread does not run on the creation thread.
    bool receivesMessagesOnCurrentThread() override { return false; }

    // Never called because receivesMessagesOnCurrentThread() returns false.
    bool waitForMessageOnCurrentThread() override { return false; }

private:
    void _receiveLoop() {
        std::fprintf(stderr, "[SocketChannel fd=%d] receive loop started (tid=%zu)\n",
                     _fd, (size_t)std::hash<std::thread::id>{}(std::this_thread::get_id()));
        std::fflush(stderr);
        while (!_stop.load(std::memory_order_acquire)) {
            uint32_t hdr[2];
            if (!detail::readAll(_fd, hdr, 8)) {
                std::fprintf(stderr, "[SocketChannel fd=%d] readAll failed\n", _fd);
                std::fflush(stderr);
                break;
            }
            auto messageID = static_cast<ARA::IPC::MessageID>((int32_t)hdr[0]);
            uint32_t payloadLen = hdr[1];
            std::fprintf(stderr, "[SocketChannel fd=%d] received msgID=%d payloadLen=%u\n",
                         _fd, (int)messageID, payloadLen);
            std::fflush(stderr);

            std::unique_ptr<ARA::IPC::MessageDecoder> decoder;
            if (payloadLen > 0) {
                std::vector<uint8_t> payload(payloadLen);
                if (!detail::readAll(_fd, payload.data(), payloadLen)) break;
                decoder = deserializeDecoder(payload.data(), payload.size());
            } else {
                // Empty payload → empty decoder (nullptr is fine per ARA IPC spec)
                decoder = nullptr;
            }

            // routeReceivedMessage is protected — call it via the inherited method.
            routeReceivedMessage(messageID, std::move(decoder));
        }
    }

    int          _fd;
    std::mutex   _sendMutex;
    std::thread  _recvThread;
    std::atomic<bool> _stop { false };
};

} // namespace SocketIPC
