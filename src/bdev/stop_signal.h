
#pragma once

#include <seastar/core/condition-variable.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>

class bdev_stop_signal {
public:
    bdev_stop_signal() {
        seastar::engine().handle_signal(SIGINT, [this] { signaled(); });
        seastar::engine().handle_signal(SIGTERM, [this] { signaled(); });
        seastar::engine().handle_signal(SIGTSTP, [this] { signaled(); });
        
    }
    ~bdev_stop_signal() {
        seastar::engine().handle_signal(SIGINT, [] {});
        seastar::engine().handle_signal(SIGTERM, [] {});
        seastar::engine().handle_signal(SIGTSTP, [] {});
    }
    seastar::future<> wait() {
        return _cond.wait([this] { return _caught; });
    }

    bool stopping() const { return _caught; }

    void signaled() {
        _caught = true;
        _cond.broadcast();
    }

private:
    bool _caught = false;
    seastar::condition_variable _cond;
};
