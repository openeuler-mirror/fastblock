/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#pragma once

/**
 * Offline AOS dump / light fsck (design v2 P4 tools).
 *
 * Pure image scan — no SPDK reactor required. Suitable for unit tests and
 * future CLI wrappers that mmap/read a device image.
 */

#include "superblock.h"
#include "types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fastblock::aos {

struct dump_record_count {
  record_type type{record_type::data};
  uint64_t count{0};
};

struct dump_error {
  uint64_t offset{0};
  std::string message;
};

struct dump_report {
  bool sb_a_ok{false};
  bool sb_b_ok{false};
  superblock sb_a{};
  superblock sb_b{};
  superblock chosen{};
  bool has_chosen{false};

  bool manifest_ok{false};
  manifest man{};

  uint64_t data_scan_start{0};
  uint64_t data_scan_end{0}; // exclusive: first byte not consumed
  uint64_t record_count{0};
  std::map<uint8_t, uint64_t> records_by_type; // record_type as u8
  std::vector<dump_error> errors;

  /** True when no hard errors and at least one valid superblock. */
  bool ok() const { return has_chosen && errors.empty(); }
};

/**
 * Scan a full AOS region image (meta + data).
 * align: on-disk record padding (usually device block size, e.g. 4096).
 * max_data: 0 = scan until end of image or first hard decode failure streak.
 */
dump_report scan_image(const uint8_t* image, size_t len, uint32_t align = 4096);

/** Human-readable multi-line summary (for tests / CLI). */
std::string format_report(const dump_report& r);

/** record_type name for dump text. */
const char* record_type_name(record_type t);

} // namespace fastblock::aos
