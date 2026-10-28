#pragma once

// Private to the ipc_*.cpp files: the platform part of a connection.

#include "floserver/core/ipc.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace floserver::core::ipc {

struct Connection::Impl {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool overlapped = false;  // client handles, so they can time out
    DWORD timeout_ms = 0;     // 0: no limit
#else
    int fd = -1;
#endif
    bool broken = false;

    Impl() = default;
    ~Impl() { close(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] bool is_open() const;
    // Whole buffers or nothing; false when the other end is gone.
    [[nodiscard]] bool write_all(const std::uint8_t* data, std::size_t size) const;
    [[nodiscard]] bool read_all(std::uint8_t* data, std::size_t size) const;
    void close();
    bool set_timeout(std::chrono::milliseconds limit);
};

// Letters, digits and '-', 1 to 48 characters.
[[nodiscard]] bool is_instance_name(std::string_view name);

}  // namespace floserver::core::ipc
