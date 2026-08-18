/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 */
#include "fastblock/msg/sock/quota.h"

namespace msg {
namespace sock {

process_quota& process_quota::instance() noexcept {
    static process_quota q;
    return q;
}

} // namespace sock
} // namespace msg
