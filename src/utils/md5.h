/* Copyright (c) 2023-2024 ChinaUnicom
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

#include <openssl/md5.h>
#include <string>

namespace utils {

[[nodiscard]] inline std::string md5(const char *data, size_t len)
{
    MD5_CTX c;
    unsigned char md[MD5_DIGEST_LENGTH];

    MD5_Init(&c);
    MD5_Update(&c, static_cast<const void *>(data), len);
    MD5_Final(md, &c);

    std::string hash(md, md + MD5_DIGEST_LENGTH);
    return hash;
}

}