/* Copyright (c) 2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "fb_client.h"
#include "spdk/log.h"

#include <tuple>

// jenkins hash function
// code is borrowed from http://burtleburtle.net/bob/hash/evahash.html
// copyright belongs to the Robert J. Jenkins Jr

SPDK_LOG_REGISTER_COMPONENT(libblk)
/* The mixing step */
#define mix(a, b, c)       \
    {                      \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 13); \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 8);  \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 13); \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 12); \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 16); \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 5);  \
        a = a - b;         \
        a = a - c;         \
        a = a ^ (c >> 3);  \
        b = b - c;         \
        b = b - a;         \
        b = b ^ (a << 10); \
        c = c - a;         \
        c = c - b;         \
        c = c ^ (b >> 15); \
    }

unsigned jenkins_hash(const std::string &str, unsigned length)
{
    const unsigned char *k = (const unsigned char *)str.c_str();
    uint32_t a, b, c; /* the internal state */
    uint32_t len;     /* how many key bytes still need mixing */

    /* Set up the internal state */
    len = length;
    a = 0x9e3779b9; /* the golden ratio; an arbitrary value */
    b = a;
    c = 0; /* variable initialization of internal state */

    /* handle most of the key */
    while (len >= 12)
    {
        a = a + (k[0] + ((uint32_t)k[1] << 8) + ((uint32_t)k[2] << 16) +
                 ((uint32_t)k[3] << 24));
        b = b + (k[4] + ((uint32_t)k[5] << 8) + ((uint32_t)k[6] << 16) +
                 ((uint32_t)k[7] << 24));
        c = c + (k[8] + ((uint32_t)k[9] << 8) + ((uint32_t)k[10] << 16) +
                 ((uint32_t)k[11] << 24));
        mix(a, b, c);
        k = k + 12;
        len = len - 12;
    }

    /* handle the last 11 bytes */
    c = c + length;
    switch (len)
    { /* all the case statements fall through */
    case 11:
        c = c + ((uint32_t)k[10] << 24);
        [[fallthrough]];
    case 10:
        c = c + ((uint32_t)k[9] << 16);
        [[fallthrough]];
    case 9:
        c = c + ((uint32_t)k[8] << 8);
        /* the first byte of c is reserved for the length */
        [[fallthrough]];
    case 8:
        b = b + ((uint32_t)k[7] << 24);
        [[fallthrough]];
    case 7:
        b = b + ((uint32_t)k[6] << 16);
        [[fallthrough]];
    case 6:
        b = b + ((uint32_t)k[5] << 8);
        [[fallthrough]];
    case 5:
        b = b + k[4];
        [[fallthrough]];
    case 4:
        a = a + ((uint32_t)k[3] << 24);
        [[fallthrough]];
    case 3:
        a = a + ((uint32_t)k[2] << 16);
        [[fallthrough]];
    case 2:
        a = a + ((uint32_t)k[1] << 8);
        [[fallthrough]];
    case 1:
        a = a + k[0];
        /* case 0: nothing left to add */
    }
    mix(a, b, c);

    return c;
}

// count bits (set + any 0's that follow)
template <class T>
inline typename std::enable_if<
    (std::is_integral<T>::value &&
     sizeof(T) <= sizeof(unsigned)),
    unsigned>::type
cbits(T v)
{
    if (v == 0)
        return 0;
    return (sizeof(v) * 8) - __builtin_clz(v);
}

unsigned fblock_client::calc_target(const std::string &sstr, int32_t target_pool_id)
{
    unsigned seed = jenkins_hash(sstr, sstr.size());
    calc_pg_masks(target_pool_id);
    return (seed % _pg_num);
}

void fblock_client::calc_pg_masks(int32_t target_pool_id)
{
    _pg_num = _mon_cli->get_pg_num(target_pool_id);
    _pg_mask = (1 << cbits(_pg_num - 1)) - 1;
    return;
}
