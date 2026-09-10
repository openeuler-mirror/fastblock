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
#include "checkpoint.h"
#include "types.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace image_verify {
namespace {

struct extent_key {
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  std::string name;  // preferred after snapshot ingest (new object_ids)
  uint64_t object_id{0};
  uint64_t object_offset{0};

  bool operator<(const extent_key& o) const {
    if (pool_id != o.pool_id) {
      return pool_id < o.pool_id;
    }
    if (pg_id != o.pg_id) {
      return pg_id < o.pg_id;
    }
    if (name != o.name) {
      return name < o.name;
    }
    if (name.empty() && object_id != o.object_id) {
      return object_id < o.object_id;
    }
    return object_offset < o.object_offset;
  }
};

struct oid_key {
  uint64_t pool_id{0};
  uint64_t pg_id{0};
  uint64_t store_epoch{0};
  uint64_t object_id{0};
  bool operator<(const oid_key& o) const {
    return std::tie(pool_id, pg_id, store_epoch, object_id) <
           std::tie(o.pool_id, o.pg_id, o.store_epoch, o.object_id);
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
  uint64_t name_map{0};
  uint64_t serving_max{0};
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

  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) {
    v.error = "failed to open";
    return v;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < 0) {
    ::close(fd);
    v.error = "failed to size";
    return v;
  }
  const size_t sz = static_cast<size_t>(st.st_size);
  const uint8_t* image = nullptr;
  if (sz > 0) {
    void* map = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
      v.error = "failed to mmap";
      return v;
    }
    image = static_cast<const uint8_t*>(map);
  } else {
    ::close(fd);
  }

  const auto report = fastblock::aos::scan_image(image, sz, align,
                                                 /*collect_live=*/true);
  if (image != nullptr) {
    ::munmap(const_cast<uint8_t*>(image), sz);
  }
  v.scan_ok = report.ok();
  v.record_count = report.record_count;
  if (!report.ok()) {
    v.error = "scan failed";
    if (!report.errors.empty()) {
      v.error += ": " + report.errors.front().message;
    }
  }

  // Serving epoch only: snapshot ingest activates a new store_epoch; old
  // epoch records keep their local object_ids and would collide if mixed.
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> serving_epoch;
  for (const auto& rec : report.live_records) {
    auto key = std::make_pair(rec.pool_id, rec.pg_id);
    if (rec.store_epoch > serving_epoch[key]) {
      serving_epoch[key] = rec.store_epoch;
    }
  }

  auto is_serving = [&](const fastblock::aos::dump_report::live_record& rec) {
    auto it = serving_epoch.find({rec.pool_id, rec.pg_id});
    return it != serving_epoch.end() && rec.store_epoch == it->second;
  };

  // Snapshot ingest restages objects with new ids. Compare by name.
  // After GC, object_name records may have been reclaimed; names survive
  // in the latest checkpoint_marker payload.
  std::map<oid_key, std::string> id_to_name;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> ckpt_gen;
  for (const auto& rec : report.live_records) {
    if (!is_serving(rec)) {
      continue;
    }
    if (rec.type == fastblock::aos::record_type::object_name &&
        !rec.payload.empty()) {
      id_to_name[oid_key{rec.pool_id, rec.pg_id, rec.store_epoch,
                         rec.object_id}] = rec.payload;
      continue;
    }
    if (rec.type != fastblock::aos::record_type::checkpoint_marker ||
        rec.payload.empty()) {
      continue;
    }
    fastblock::aos::checkpoint_snapshot snap;
    if (!fastblock::aos::decode_checkpoint_payload(rec.payload, &snap)) {
      continue;
    }
    auto pg = std::make_pair(rec.pool_id, rec.pg_id);
    if (snap.generation < ckpt_gen[pg]) {
      continue;
    }
    ckpt_gen[pg] = snap.generation;
    for (const auto& obj : snap.objects) {
      if (obj.name.empty()) {
        continue;
      }
      id_to_name[oid_key{rec.pool_id, rec.pg_id, rec.store_epoch,
                         obj.object_id}] = obj.name;
    }
  }
  v.name_map = id_to_name.size();
  for (const auto& kv : serving_epoch) {
    if (kv.second > v.serving_max) {
      v.serving_max = kv.second;
    }
  }

  auto make_key = [&](uint64_t pool, uint64_t pg, uint64_t epoch, uint64_t oid,
                      uint64_t obj_off) {
    extent_key k;
    k.pool_id = pool;
    k.pg_id = pg;
    k.object_offset = obj_off;
    auto nit = id_to_name.find(oid_key{pool, pg, epoch, oid});
    if (nit != id_to_name.end()) {
      k.name = nit->second;
    } else {
      k.object_id = oid;
    }
    return k;
  };

  for (const auto& rec : report.live_records) {
    if (!is_serving(rec)) {
      continue;
    }
    if (is_delete_type(rec.type)) {
      std::string del_name;
      auto nit = id_to_name.find(
          oid_key{rec.pool_id, rec.pg_id, rec.store_epoch, rec.object_id});
      if (nit != id_to_name.end()) {
        del_name = nit->second;
      }
      for (auto it = v.live.begin(); it != v.live.end();) {
        const bool same_obj =
            it->first.pool_id == rec.pool_id && it->first.pg_id == rec.pg_id &&
            ((!del_name.empty() && it->first.name == del_name) ||
             (del_name.empty() && it->first.name.empty() &&
              it->first.object_id == rec.object_id));
        if (same_obj && rec.raft_apply_index >= it->second.raft_apply_index) {
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
    auto consider = [&](uint64_t obj_off, const char* data, size_t len,
                        uint32_t crc) {
      extent_key k = make_key(rec.pool_id, rec.pg_id, rec.store_epoch,
                              rec.object_id, obj_off);
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
        // Snapshot restage: compare VERIFYA1 blocks by image offset.
        k.name.clear();
        k.object_id = 0;
        k.object_offset = decoded.offset;
      }
      v.live[k] = val;
    };

    // Snapshot install writes a 4MiB object unit; user writes are often 4KiB.
    // Trailing zero 4KiB blocks are padding, not a replica difference.
    size_t eff = rec.payload.size();
    constexpr size_t kBlk = 4096;
    while (eff > kBlk) {
      const char* tail = rec.payload.data() + (eff - kBlk);
      bool z = true;
      for (size_t i = 0; i < kBlk; ++i) {
        if (tail[i] != 0) {
          z = false;
          break;
        }
      }
      if (!z) {
        break;
      }
      eff -= kBlk;
    }
    consider(rec.object_offset, rec.payload.data(), eff,
             pattern_crc32(rec.payload.data(), eff));
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
               "[--min-replicas N] <aos-file>...\n"
               "  --min-replicas N: an extent counts as present when at least N\n"
               "  dumps hold it (default: every dump, i.e. one PG's replicas).\n"
               "  Scan one or more AOS device images and compare last-writer-wins\n"
               "  object extents (by object name, not local object_id — snapshot\n"
               "  ingest restages new ids). With --seed, only VERIFYA1 payloads of\n"
               "  that seed are compared (ignores leftover 0x55 / older images).\n");
}

} // namespace

int run_dump_compare(int argc, char** argv) {
  uint32_t align = 512;
  std::optional<uint64_t> seed;
  uint64_t min_replicas = 0;
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
    if (std::strcmp(a, "--min-replicas") == 0) {
      if (i + 1 >= argc || !parse_u64(argv[++i], &min_replicas)) {
        std::fprintf(stderr, "invalid --min-replicas\n");
        return 2;
      }
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
    uint64_t named = 0;
    for (const auto& kv : v.live) {
      if (!kv.first.name.empty()) {
        ++named;
      }
    }
    std::cout << "  " << v.path << ": records=" << v.record_count
              << " data=" << v.data_count
              << " patterned=" << v.patterned_count
              << " live=" << v.live.size() << " named=" << named
              << " namemap=" << v.name_map << " max_epoch=" << v.serving_max
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
  // 0 keeps the original contract: every dump must hold every live extent,
  // which only holds when the dumps are the replicas of one PG.  With
  // --min-replicas N an extent is present once N dumps hold it, so dumps
  // spanning several PGs (more OSDs than the replica count) still compare.
  const size_t required =
    (min_replicas == 0 || min_replicas >= views.size())
      ? views.size()
      : static_cast<size_t>(min_replicas);
  for (const auto& kv : union_map) {
    size_t have = 0;
    for (size_t i = 0; i < views.size(); ++i) {
      if (views[i].live.count(kv.first) != 0) {
        ++have;
      }
    }
    if (have < required) {
      ++missing;
      if (missing + mismatch <= kMaxPrint) {
        std::cout << "  MISSING " << have << "/" << views.size()
                  << " pg=" << kv.first.pool_id << "." << kv.first.pg_id;
        if (!kv.first.name.empty()) {
          std::cout << " name=" << kv.first.name;
        } else {
          std::cout << " obj=0x" << std::hex << kv.first.object_id << std::dec;
        }
        std::cout << " obj_off=" << kv.first.object_offset;
        if (kv.second.patterned) {
          std::cout << " image_off=" << kv.second.image_offset;
        }
        std::cout << "\n";
      }
    }
    for (size_t i = 0; i < views.size(); ++i) {
      auto it = views[i].live.find(kv.first);
      if (it == views[i].live.end()) {
        continue;
      }
      if (it->second.payload_crc != kv.second.payload_crc ||
          it->second.payload_len != kv.second.payload_len) {
        ++mismatch;
        if (missing + mismatch <= kMaxPrint) {
          std::cout << "  MISMATCH replica[" << i << "] " << views[i].path
                    << " pg=" << kv.first.pool_id << "." << kv.first.pg_id;
          if (!kv.first.name.empty()) {
            std::cout << " name=" << kv.first.name;
          } else {
            std::cout << " obj=0x" << std::hex << kv.first.object_id << std::dec;
          }
          std::cout << " obj_off=" << kv.first.object_offset
                    << " crc=" << std::hex << it->second.payload_crc
                    << " vs " << kv.second.payload_crc << std::dec << "\n";
        }
      }
    }
  }

  std::cout << "live extents (union): " << union_map.size()
            << " required=" << required
            << " missing=" << missing << " mismatch=" << mismatch << "\n";
  if (any_fail || missing != 0 || mismatch != 0) {
    std::cout << "dump-compare: FAIL\n";
    return 1;
  }
  std::cout << "dump-compare: OK\n";
  return 0;
}

} // namespace image_verify
