#include "ipc_impl.h"

#include "floserver/core/process.h"

#include <algorithm>
#include <array>

namespace floserver::core::ipc {

bool is_instance_name(std::string_view name) {
    return !name.empty() && name.size() <= 48 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::string default_instance() {
    std::string name = "floserverd";
    if (const auto suffix = get_env("FLOSERVER_INSTANCE"); suffix && !suffix->empty()) {
        const std::string candidate = name + "-" + *suffix;
        if (is_instance_name(candidate)) {
            name = candidate;
        }
    }
    return name;
}

Connection::Connection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Connection::~Connection() = default;
Connection::Connection(Connection&& other) noexcept = default;
Connection& Connection::operator=(Connection&& other) noexcept = default;

bool Connection::valid() const { return impl_ && impl_->is_open() && !impl_->broken; }

bool Connection::send(std::string_view message) {
    if (!valid() || message.size() > kMaxMessage) {
        return false;
    }
    const auto size = static_cast<std::uint32_t>(message.size());
    const std::array<std::uint8_t, 4> header = {
        static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8U),
        static_cast<std::uint8_t>(size >> 16U), static_cast<std::uint8_t>(size >> 24U)};
    if (!impl_->write_all(header.data(), header.size()) ||
        !impl_->write_all(reinterpret_cast<const std::uint8_t*>(message.data()), message.size())) {
        impl_->broken = true;
        return false;
    }
    return true;
}

std::optional<std::string> Connection::receive() {
    if (!valid()) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 4> header{};
    if (!impl_->read_all(header.data(), header.size())) {
        impl_->broken = true;
        return std::nullopt;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(header[0]) |
                               (static_cast<std::uint32_t>(header[1]) << 8U) |
                               (static_cast<std::uint32_t>(header[2]) << 16U) |
                               (static_cast<std::uint32_t>(header[3]) << 24U);
    if (size > kMaxMessage) {
        // The stream can't be trusted after this: stop talking.
        impl_->broken = true;
        return std::nullopt;
    }
    std::string message(size, '\0');
    if (size != 0 && !impl_->read_all(reinterpret_cast<std::uint8_t*>(message.data()), size)) {
        impl_->broken = true;
        return std::nullopt;
    }
    return message;
}

bool Connection::set_timeout(std::chrono::milliseconds limit) {
    return impl_ && impl_->set_timeout(limit);
}

void Connection::close() {
    if (impl_) {
        impl_->close();
    }
}

}  // namespace floserver::core::ipc
