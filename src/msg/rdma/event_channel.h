#pragma once

#include <spdk/log.h>

#include <rdma/rdma_cma.h>

#include <fcntl.h>
#include <poll.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace msg {
namespace rdma {

class event_channel {

public:

    event_channel() {
        _channel = ::rdma_create_event_channel();
        if (not _channel) [[unlikely]] {
            throw std::runtime_error{"create rdma event channel failed"};
        }

        _fd.fd = _channel->fd;
        _fd.events = POLLIN;

        int ret = ::fcntl(_fd.fd, F_GETFL);
        ret = ::fcntl(_fd.fd, F_SETFL, ret | O_NONBLOCK);

        if (ret) [[unlikely]] {
            throw std::runtime_error{"make poll fd nonblock failed"};
        }
    }

    event_channel(const event_channel&) = delete;

    event_channel(event_channel&& other) noexcept
      : _channel{std::exchange(other._channel, nullptr)} {}

    event_channel& operator=(const event_channel&) = delete;

    event_channel& operator=(event_channel&&) = delete;

    ~event_channel() noexcept {
        close();
    }

public:

    void close() noexcept {
        if (_channel) {
            ::rdma_destroy_event_channel(_channel);
            _channel = nullptr;
        }
    }

    ::rdma_event_channel* value() noexcept { return _channel; }

    ::rdma_cm_event* poll() noexcept {
        if (not _channel) { return nullptr; }

        int ret = ::poll(&_fd, 1, 0);
        if (ret <= 0 or !(_fd.revents & POLLIN)) {
            return nullptr;
        }

        ::rdma_cm_event* event;
        ret = ::rdma_get_cm_event(_channel, &event);
        if (ret) [[unlikely]] {
            if (errno != EAGAIN and errno != EWOULDBLOCK) {
                SPDK_ERRLOG_EX("ERROR: Poll rdma event error: %s\n", std::strerror(errno));
            }

            return nullptr;
        }

        return event;
    }

private:
    ::rdma_event_channel* _channel{};
    ::pollfd _fd{};
};

} // namespace rdma
} // namespace msg
