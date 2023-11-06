/* Copyright (c) 2023 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#pragma once
#include <chrono>

#include <random>
#include <string>

class context {
protected:
    virtual void finish(int r) = 0;
    virtual void finish_del(int r) {}
public:
    context(bool needs_delete = true)
    : _needs_delete(needs_delete) {}

    virtual ~context() {}
    virtual void complete(int r) {
        if(_needs_delete){
            finish(r);
            delete this;
        }else{
            finish_del(r);
        }
    }
private:
    bool _needs_delete;
};

struct osd_info_t
{
	int node_id;
	bool isin;
	bool isup;
	bool ispendingcreate;
	int port;
	std::string address;
};

template <typename T>
inline constexpr
T align_up(T v, T align) {
    return (v + align - 1) & ~(align - 1);
}

template <typename T>
inline constexpr
T align_down(T v, T align) {
    return v & ~(align - 1);
}

inline int64_t get_time(){
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    return now_ms.time_since_epoch().count();
}

namespace utils {

template<typename T>
static T random_int(const T min, const T max) {
    std::random_device rd{};
    std::uniform_int_distribution<T> dist{min, max};
    return dist(rd);
}

static std::string random_string(const size_t length) {
    static std::string chars{
      "abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "1234567890"
      "!@#$%^&*()"
      "`~-_=+[{]}\\|;:'\",<.>/? "};

    std::random_device rd{};
    std::uniform_int_distribution<decltype(chars)::size_type> index_dist{0, chars.size() - 1};
    std::string ret(length, ' ');
    for (size_t i{0}; i < length; ++i) {
        ret[i] = chars[index_dist(rd)];
    }

    return ret;
}

}
