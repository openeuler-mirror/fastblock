/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "aos_dump.h"
#include "layout.h"
#include "record.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace fastblock::aos {
namespace {

uint64_t align_up(uint64_t n, uint32_t align) {
  if (align == 0) {
    return n;
  }
  const uint64_t a = align;
  return (n + a - 1) / a * a;
}

void push_err(dump_report* r, uint64_t off, std::string msg) {
  if (r == nullptr) {
    return;
  }
  r->errors.push_back(dump_error{off, std::move(msg)});
}

} // namespace

const char* record_type_name(record_type t) {
  switch (t) {
  case record_type::data:
    return "data";
  case record_type::delete_tombstone:
    return "delete_tombstone";
  case record_type::gc_rewrite:
    return "gc_rewrite";
  case record_type::object_name:
    return "object_name";
  case record_type::segment_seal:
    return "segment_seal";
  case record_type::range_tombstone:
    return "range_tombstone";
  case record_type::checkpoint_marker:
    return "checkpoint_marker";
  case record_type::raft_snapshot_pin:
    return "raft_snapshot_pin";
  case record_type::raft_snapshot_unpin:
    return "raft_snapshot_unpin";
  case record_type::image_snapshot_pin:
    return "image_snapshot_pin";
  case record_type::image_snapshot_unpin:
    return "image_snapshot_unpin";
  case record_type::install_seal:
    return "install_seal";
  case record_type::install_activate:
    return "install_activate";
  case record_type::raft_log_entry:
    return "raft_log_entry";
  case record_type::kv_put:
    return "kv_put";
  case record_type::kv_delete:
    return "kv_delete";
  case record_type::kv_checkpoint:
    return "kv_checkpoint";
  default:
    return "unknown";
  }
}

dump_report scan_image(const uint8_t* image, size_t len, uint32_t align) {
  dump_report r;
  if (image == nullptr || len == 0) {
    push_err(&r, 0, "empty image");
    return r;
  }

  if (len >= kSuperblockBytes) {
    r.sb_a_ok =
        decode_superblock(image + kSuperblockAOffset, kSuperblockBytes, &r.sb_a);
  }
  if (len >= kSuperblockBOffset + kSuperblockBytes) {
    r.sb_b_ok =
        decode_superblock(image + kSuperblockBOffset, kSuperblockBytes, &r.sb_b);
  }
  if (prefer_superblock(r.sb_a, r.sb_a_ok, r.sb_b, r.sb_b_ok, &r.chosen)) {
    r.has_chosen = true;
  } else {
    push_err(&r, 0, "no valid superblock (A/B)");
    return r;
  }

  // Manifest for chosen generation.
  const uint64_t man_off = manifest_slot_offset(r.chosen.manifest_generation);
  if (man_off + kManifestSlotBytes <= len) {
    r.manifest_ok = decode_manifest(image + man_off,
                                    static_cast<size_t>(kManifestSlotBytes),
                                    &r.man);
    if (!r.manifest_ok) {
      // Try the other slot as fallback (torn dual-write).
      const uint64_t alt =
          manifest_slot_offset(r.chosen.manifest_generation + 1);
      if (alt + kManifestSlotBytes <= len &&
          decode_manifest(image + alt, static_cast<size_t>(kManifestSlotBytes),
                          &r.man)) {
        r.manifest_ok = true;
      } else {
        push_err(&r, man_off, "manifest decode failed for both slots");
      }
    }
  } else {
    push_err(&r, man_off, "image shorter than manifest slot");
  }

  r.data_scan_start = r.chosen.data_offset;
  if (r.data_scan_start >= len) {
    r.data_scan_end = r.data_scan_start;
    return r;
  }

  // Scan the full data region (capped by image length). Manifest
  // data_high_water can lag durable appends until the next persist_manifest,
  // so limiting to high_water would hide tail object/log/kv records — same
  // policy as osd_store::recover_after_meta.
  uint64_t data_end = r.data_scan_start;
  if (r.chosen.data_length > 0) {
    data_end = r.data_scan_start + r.chosen.data_length;
  }
  uint64_t scan_limit = std::min(static_cast<uint64_t>(len), data_end);
  if (scan_limit < r.data_scan_start) {
    scan_limit = r.data_scan_start;
  }

  size_t off = static_cast<size_t>(r.data_scan_start);
  const size_t end = static_cast<size_t>(scan_limit);
  uint32_t consecutive_bad = 0;
  constexpr uint32_t kMaxBadStreak = 8;

  while (off < end) {
    data_record rec;
    size_t consumed = 0;
    const decode_status st =
        decode_record(image + off, end - off, &rec, &consumed);
    if (st == decode_status::need_more) {
      break;
    }
    if (st == decode_status::bad_magic) {
      // Unwritten / padding region — stop cleanly if we already saw records
      // or if zero streak is long.
      if (consumed == 0) {
        // Peek: if remaining looks empty, stop; else skip one align unit.
        bool all_zero = true;
        const size_t peek = std::min(static_cast<size_t>(align), end - off);
        for (size_t i = 0; i < peek; ++i) {
          if (image[off + i] != 0) {
            all_zero = false;
            break;
          }
        }
        if (all_zero) {
          break;
        }
        ++consecutive_bad;
        if (consecutive_bad >= kMaxBadStreak) {
          push_err(&r, off, "too many consecutive bad_magic while scanning");
          break;
        }
        off += (align > 0 ? align : 1);
        continue;
      }
    }
    if (st != decode_status::ok) {
      ++consecutive_bad;
      char buf[96];
      std::snprintf(buf, sizeof(buf), "decode_status=%d",
                    static_cast<int>(st));
      push_err(&r, off, buf);
      if (consecutive_bad >= kMaxBadStreak) {
        break;
      }
      off += (align > 0 ? align : 1);
      continue;
    }
    consecutive_bad = 0;
    ++r.record_count;
    r.records_by_type[static_cast<uint8_t>(rec.type)]++;
    const uint64_t step = align_up(static_cast<uint64_t>(consumed), align);
    off += static_cast<size_t>(step == 0 ? consumed : step);
  }
  r.data_scan_end = off;

  // Informational: lagging high_water is expected until post-recover snap /
  // create_pg / GC persist. Not a hard error.
  if (r.manifest_ok && r.man.data_high_water > 0 &&
      r.data_scan_end > r.man.data_high_water + align) {
    r.warnings.push_back(dump_error{
        r.data_scan_end,
        "scan advanced past manifest data_high_water (stale high_water)"});
  }

  return r;
}

std::string format_report(const dump_report& r) {
  std::ostringstream os;
  os << "AOS dump/fsck report\n";
  os << "  superblock A: " << (r.sb_a_ok ? "ok" : "bad");
  if (r.sb_a_ok) {
    os << " gen=" << r.sb_a.generation
       << " man_gen=" << r.sb_a.manifest_generation;
  }
  os << "\n  superblock B: " << (r.sb_b_ok ? "ok" : "bad");
  if (r.sb_b_ok) {
    os << " gen=" << r.sb_b.generation
       << " man_gen=" << r.sb_b.manifest_generation;
  }
  os << "\n";
  if (r.has_chosen) {
    os << "  chosen: gen=" << r.chosen.generation
       << " data_off=" << r.chosen.data_offset
       << " data_len=" << r.chosen.data_length
       << " segment_size=" << r.chosen.segment_size << "\n";
  }
  if (r.manifest_ok) {
    os << "  manifest: gen=" << r.man.generation
       << " high_water=" << r.man.data_high_water
       << " open_seg=" << r.man.open_segment_id
       << " open_cursor=" << r.man.open_cursor
       << " pgs=" << r.man.pgs.size() << "\n";
    for (const auto& pe : r.man.pgs) {
      os << "    pg " << pe.pool_id << "." << pe.pg_id
         << " epoch=" << pe.active_epoch << "\n";
    }
  } else {
    os << "  manifest: unavailable\n";
  }
  os << "  data scan: [" << r.data_scan_start << ", " << r.data_scan_end
     << ") records=" << r.record_count << "\n";
  for (const auto& kv : r.records_by_type) {
    os << "    " << record_type_name(static_cast<record_type>(kv.first)) << ": "
       << kv.second << "\n";
  }
  if (!r.errors.empty()) {
    os << "  errors (" << r.errors.size() << "):\n";
    for (const auto& e : r.errors) {
      os << "    @" << e.offset << ": " << e.message << "\n";
    }
  }
  if (!r.warnings.empty()) {
    os << "  warnings (" << r.warnings.size() << "):\n";
    for (const auto& e : r.warnings) {
      os << "    @" << e.offset << ": " << e.message << "\n";
    }
  }
  os << "  result: " << (r.ok() ? "OK" : "FAIL") << "\n";
  return os.str();
}

} // namespace fastblock::aos
