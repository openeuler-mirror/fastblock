#pragma once

#include <chrono>

namespace utils {

class time_check {

public:

    time_check() = delete;

    time_check(const std::chrono::system_clock::duration dur) : _check_dur{dur} {}

public:

    bool check_and_update() {
        if (_check_point >= std::chrono::system_clock::now()) {
            return false;
        }

        _check_point = std::chrono::system_clock::now() + _check_dur;
        return true;
    }

private:

    std::chrono::system_clock::duration _check_dur{};
    std::chrono::system_clock::time_point _check_point{std::chrono::system_clock::now()};
};

}
