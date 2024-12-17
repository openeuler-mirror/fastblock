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

#include <boost/format.hpp>

#define FB_FMT_1(text, v_1) (boost::format(text) % v_1).str()
#define FB_FMT_2(text, v_1, v_2) (boost::format(text) % v_1 % v_2).str()
#define FB_FMT_3(text, v_1, v_2, v_3) (boost::format(text) % v_1 % v_2 % v_3).str()
#define FB_FMT_4(text, v_1, v_2, v_3, v_4) (boost::format(text) % v_1 % v_2 % v_3 % v_4).str()
#define FB_FMT_5(text, v_1, v_2, v_3, v_4, v_5) (boost::format(text) % v_1 % v_2 % v_3 % v_4 % v_5).str()
#define FB_FMT_6(text, v_1, v_2, v_3, v_4, v_5, v_6) (boost::format(text) % v_1 % v_2 % v_3 % v_4 % v_5 % v_6).str()
#define FB_FMT_7(text, v_1, v_2, v_3, v_4, v_5, v_6, v_7) (boost::format(text) % v_1 % v_2 % v_3 % v_4 % v_5 % v_6 % v_7).str()

#define FB_RFMT_1(text, v_1) (boost::format(text) % v_1)
#define FB_RFMT_2(text, v_1, v_2) (boost::format(text) % v_1 % v_2)
#define FB_RFMT_3(text, v_1, v_2, v_3) (boost::format(text) % v_1 % v_2 % v_3)
#define FB_RFMT_4(text, v_1, v_2, v_3, v_4) (boost::format(text) % v_1 % v_2 % v_3 % v_4)
#define FB_RFMT_5(text, v_1, v_2, v_3, v_4, v_5) (boost::format(text) % v_1 % v_2 % v_3 % v_4 % v_5)
#define FB_RFMT_6(text, v_1, v_2, v_3, v_4, v_5, v_6) (boost::format(text) % v_1 % v_2 % v_3 % v_4 % v_5 % v_6)
#define FB_RFMT_7(text, v_1, v_2, v_3, v_4, v_5, v_6, v_7) (boost::format(text) % v_1 % v_2 % v_3 % v_4 % v_5 % v_6 % v_7)
