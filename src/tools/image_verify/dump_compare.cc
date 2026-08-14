/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "dump_compare.h"
#include "verify_pattern.h"

#include "aos_dump.h"
#include "types.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace image_verify {
namespace {

struct extent_key {
  uint64_t pool_id{0};
  uint64_t object_id{0};
  uint64_t object_offset{0};

  bool operator<(const extent_key& o) const {
    return std::tie(pool_id, object_id, object_offset) <
           std::tie(o.pool_id, o.object_id, o.object_offset);
  }
};

struct extent_val {
  uint64_t raft_apply_index{0};
  uint32_t payload_crc{0};
  uint32_t payload_len{0};
  bool patterned{false};
  uint64_t image_offset{0};
  uint64_t seed{0};
};

struct file_view {
  std::string path;
  bool scan_ok{false};
  uint64_t record_count{0};
  uint64_t data_count{0};
  uint64_t patterned_count{0};
  std::map<extent_key, extent_val> live;
  std::string error;
};

bool parse_u32(const char* s, uint32_t* out) {
  if (s == nullptr || *s == '\0' || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long v = std::strtoul(s, &end, 0);
  if (end == s || *end != '\0' || v > 0xfffffffful) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

bool parse_u64(const char* s, uint64_t* out) {
  if (s == nullptr || *s == '\0' || out == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 0);
  if (end == s || *end != '\0') {
    return false;
  }
  *out = static_cast<uint64_t>(v);
  return true;
}

bool is_data_type(fastblock::aos::record_type t) {
  return t == fastblock::aos::record_type::data ||
         t == fastblock::aos::record_type::gc_rewrite;
}

bool is_delete_type(fastblock::aos::record_type t) {
  return t == fastblock::aos::record_type::delete_tombstone;
}

file_view load_one(const char* path, uint32_t align,
                   std::optional<uint64_t> seed) {
  file_view v;
  v.path = path;

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    v.error = "failed to open";
    return v;
  }
  const std::streamoff sz = in.tellg();
  if (sz < 0) {
    v.error = "failed to size";
    return v;
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> image(static_cast<size_t>(sz));
  if (sz > 0 && !in.read(reinterpret_cast<char*>(image.data()), sz)) {
    v.error = "failed to read";
    return v;
  }

  const auto report = fastblock::aos::scan_image(image.data(), image.size(),
                                                 align, /*collect_live=*/true);
  v.scan_ok = report.ok();
  v.record_count = report.record_count;
  if (!report.ok()) {
    v.error = "scan failed";
    if (!report.errors.empty()) {
      v.error += ": " + report.errors.front().message;
    }
  }

  for (const auto& rec : report.live_records) {
    if (is_delete_type(rec.type)) {
      for (auto it = v.live.begin(); it != v.live.end();) {
        if (it->first.pool_id == rec.pool_id &&
            it->first.object_id == rec.object_id &&
            rec.raft_apply_index >= it->second.raft_apply_index) {
          it = v.live.erase(it);
        } else {
          ++it;
        }
      }
      continue;
    }
    if (!is_data_type(rec.type)) {
      continue;
    }
    ++v.data_count;
    extent_key key{rec.pool_id, rec.object_id, rec.object_offset};
    auto it = v.live.find(key);
    if (it != v.live.end() && rec.raft_apply_index < it->second.raft_apply_index) {
      continue;
    }
    auto consider = [&](uint64_t obj_off, const char* data, size_t len,
                        uint32_t crc) {
      extent_key k{rec.pool_id, rec.object_id, obj_off};
      auto it = v.live.find(k);
      if (it != v.live.end() &&
          rec.raft_apply_index < it->second.raft_apply_index) {
        return;
      }
      extent_val val;
      val.raft_apply_index = rec.raft_apply_index;
      val.payload_crc = crc;
      val.payload_len = static_cast<uint32_t>(len);
      decoded_payload decoded;
      if (decode_payload(data, len, &decoded) == pattern_error::ok &&
          (!seed.has_value() || decoded.seed == *seed)) {
        val.patterned = true;
        val.image_offset = decoded.offset;
        val.seed = decoded.seed;
        ++v.patterned_count;
        // Snapshot restage allocates new object_ids; compare by image offset.
        k.object_id = 0;
        k.object_offset = decoded.offset;
      }
      v.live[k] = val;
    };

    consider(rec.object_offset, rec.payload.data(), rec.payload.size(),
             rec.payload_crc);
    // Snapshot / full-object writes store many 4KiB VERIFYA1 blocks in one
    // record. Walk 4KiB windows so replica compare still sees them.
    constexpr size_t kBlk = 4096;
    if (rec.payload.size() > kBlk) {
      for (size_t off = 0; off + kBlk <= rec.payload.size(); off += kBlk) {
        const auto* p = rec.payload.data() + off;
        if (std::memcmp(p, kMagic, 8) != 0) {
          continue;
        }
        consider(rec.object_offset + off, p, kBlk, pattern_crc32(p, kBlk));
      }
    }
  }

  if (seed.has_value()) {
    for (auto it = v.live.begin(); it != v.live.end();) {
      if (!it->second.patterned) {
        it = v.live.erase(it);
      } else {
        ++it;
      }
    }
  }
  return v;
}

void usage() {
  std::fprintf(stderr,
               "Usage: image_verify --mode dump-compare [--align N] [--seed N] "
               "<aos-file>...\n"
               "  Scan one or more AOS device images and compare last-writer-wins\n"
               "  object extents. With --seed, only VERIFYA1 payloads of that seed\n"
               "  are compared (ignores leftover 0x55 / older images).\n");
}

} // namespace

int run_dump_compare(int argc, char** argv) {
  uint32_t align = 512;
  std::optional<uint64_t> seed;
  std::vector<const char*> files;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
      usage();
      return 0;
    }
    if (std::strcmp(a, "--mode") == 0 || std::strcmp(a, "-M") == 0) {
      if (i + 1 < argc) {
        ++i;
      }
      continue;
    }
    if (std::strncmp(a, "--mode=", 7) == 0) {
      continue;
    }
    if (std::strcmp(a, "--align") == 0) {
      if (i + 1 >= argc || !parse_u32(argv[++i], &align) || align == 0) {
        std::fprintf(stderr, "invalid --align\n");
        return 2;
      }
      continue;
    }
    if (std::strcmp(a, "--seed") == 0 || std::strcmp(a, "-S") == 0) {
      uint64_t s = 0;
      if (i + 1 >= argc || !parse_u64(argv[++i], &s)) {
        std::fprintf(stderr, "invalid --seed\n");
        return 2;
      }
      seed = s;
      continue;
    }
    if (std::strcmp(a, "-C") == 0 || std::strcmp(a, "-m") == 0) {
      if (i + 1 < argc) {
        ++i;
      }
      continue;
    }
    if (a[0] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", a);
      usage();
      return 2;
    }
    files.push_back(a);
  }

  if (files.empty()) {
    usage();
    return 2;
  }

  std::vector<file_view> views;
  views.reserve(files.size());
  bool any_fail = false;
  std::cout << "dump-compare: " << files.size() << " image(s) align=" << align;
  if (seed.has_value()) {
    std::cout << " seed=" << *seed;
  }
  std::cout << "\n";

  for (const char* path : files) {
    auto v = load_one(path, align, seed);
    std::cout << "  " << v.path << ": records=" << v.record_count
              << " data=" << v.data_count
              << " patterned=" << v.patterned_count
              << " live=" << v.live.size()
              << (v.scan_ok ? " scan=ok" : " scan=FAIL");
    if (!v.error.empty()) {
      std::cout << " (" << v.error << ")";
    }
    std::cout << "\n";
    if (!v.scan_ok) {
      any_fail = true;
    }
    views.push_back(std::move(v));
  }

  if (seed.has_value()) {
    bool any_pattern = false;
    for (const auto& v : views) {
      if (!v.live.empty()) {
        any_pattern = true;
        break;
      }
    }
    if (!any_pattern) {
      std::cout << "no VERIFYA1 extents with seed=" << *seed
                << " on any replica\n";
      return 1;
    }
  }

  std::map<extent_key, extent_val> union_map;
  for (const auto& v : views) {
    for (const auto& kv : v.live) {
      auto it = union_map.find(kv.first);
      if (it == union_map.end() ||
          kv.second.raft_apply_index > it->second.raft_apply_index) {
        union_map[kv.first] = kv.second;
      }
    }
  }

  uint64_t missing = 0;
  uint64_t mismatch = 0;
  constexpr uint64_t kMaxPrint = 16;
  for (const auto& kv : union_map) {
    for (size_t i = 0; i < views.size(); ++i) {
      auto it = views[i].live.find(kv.first);
      if (it == views[i].live.end()) {
        ++missing;
        if (missing + mismatch <= kMaxPrint) {
          std::cout << "  MISSING replica[" << i << "] " << views[i].path
                    << " obj=0x" << std::hex << kv.first.object_id << std::dec
                    << " obj_off=" << kv.first.object_offset;
          if (kv.second.patterned) {
            std::cout << " image_off=" << kv.second.image_offset;
          }
          std::cout << "\n";
        }
        continue;
      }
      if (it->second.payload_crc != kv.second.payload_crc ||
          it->second.payload_len != kv.second.payload_len) {
        ++mismatch;
        if (missing + mismatch <= kMaxPrint) {
          std::cout << "  MISMATCH replica[" << i << "] " << views[i].path
                    << " obj=0x" << std::hex << kv.first.object_id << std::dec
                    << " obj_off=" << kv.first.object_offset
                    << " crc=" << std::hex << it->second.payload_crc
                    << " vs " << kv.second.payload_crc << std::dec << "\n";
        }
      }
    }
  }

  std::cout << "live extents (union): " << union_map.size()
            << " missing=" << missing << " mismatch=" << mismatch << "\n";
  if (any_fail || missing != 0 || mismatch != 0) {
    std::cout << "dump-compare: FAIL\n";
    return 1;
  }
  std::cout << "dump-compare: OK\n";
  return 0;
}

} // namespace image_verify
