/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

/**
 * M1 unit tests for append-only store pure logic + async API shape.
 * I/O uses mem_device_backend (callback model); production uses spdk_bdev_backend.
 */

#include "aos_dump.h"
#include "aos_runtime.h"
#include "checkpoint.h"
#include "crc32.h"
#include "extent_map.h"
#include "kv_engine.h"
#include "layout.h"
#include "log_store.h"
#include "mem_device_backend.h"
#include "osd_store.h"
#include "pg_store.h"
#include "record.h"
#include "segment.h"
#include "segment_allocator.h"
#include "snapshot_ingest.h"
#include "snapshot_reader.h"
#include "superblock.h"
#include "types.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define EXPECT_EQ(a, b)                                                        \
  do {                                                                         \
    const auto _a = (a);                                                       \
    const auto _b = (b);                                                       \
    if (!(_a == _b)) {                                                         \
      std::fprintf(stderr,                                                     \
                   "FAIL %s:%d: %s == %s (got %lld vs %lld)\n", __FILE__,      \
                   __LINE__, #a, #b, static_cast<long long>(_a),               \
                   static_cast<long long>(_b));                                \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using namespace fastblock::aos;

struct unified_log_guard {
  bool prev_;
  explicit unified_log_guard(bool on) {
    prev_ = unified_log_enabled();
    set_unified_log_enabled(on);
  }
  ~unified_log_guard() { set_unified_log_enabled(prev_); }
};

void test_crc32_smoke() {
  const char* s = "123456789";
  // ISO-HDLC CRC-32 of "123456789" is 0xCBF43926
  EXPECT_EQ(crc32(s, 9), 0xCBF43926u);
}

void test_record_roundtrip() {
  data_record in;
  in.type = record_type::data;
  in.flags = 0;
  in.pool_id = 1;
  in.pg_id = 7;
  in.store_epoch = 3;
  in.object_id = 42;
  in.object_offset = 4096;
  in.raft_apply_index = 100;
  in.image_snap_seq = 0;
  in.payload.assign(4096, 'A');

  auto bytes = encode_record(in, kFormatVersion);
  EXPECT_EQ(bytes.size(), static_cast<size_t>(kRecordHeaderLenV1 + 4096));

  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
  EXPECT_TRUE(st == decode_status::ok);
  EXPECT_EQ(consumed, bytes.size());
  EXPECT_EQ(static_cast<uint8_t>(out.type), static_cast<uint8_t>(record_type::data));
  EXPECT_EQ(out.pool_id, 1u);
  EXPECT_EQ(out.pg_id, 7u);
  EXPECT_EQ(out.store_epoch, 3u);
  EXPECT_EQ(out.object_id, 42u);
  EXPECT_EQ(out.object_offset, 4096u);
  EXPECT_EQ(out.raft_apply_index, 100u);
  EXPECT_EQ(out.image_snap_seq, 0u);
  EXPECT_EQ(out.header_len, kRecordHeaderLenV1);
  EXPECT_EQ(out.raft_term, 0u);
  EXPECT_EQ(out.payload.size(), 4096u);
  EXPECT_TRUE(out.payload == in.payload);
}

void test_record_v3_roundtrip() {
  data_record in;
  in.type = record_type::data;
  in.flags = 0;
  in.pool_id = 1;
  in.pg_id = 7;
  in.store_epoch = 3;
  in.object_id = 42;
  in.object_offset = 4096;
  in.raft_apply_index = 100;
  in.image_snap_seq = 5;
  in.raft_term = 17;
  in.payload.assign(4096, 'A');

  auto bytes = encode_record(in, kFormatVersionV3);
  EXPECT_EQ(bytes.size(), static_cast<size_t>(kRecordHeaderLenV3 + 4096));
  EXPECT_TRUE(std::equal(bytes.begin() + kRecordHeaderLenV3, bytes.end(),
                         in.payload.begin()));

  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
  EXPECT_TRUE(st == decode_status::ok);
  EXPECT_EQ(consumed, bytes.size());
  EXPECT_EQ(out.header_len, kRecordHeaderLenV3);
  EXPECT_EQ(out.raft_term, 17u);
  EXPECT_EQ(out.image_snap_seq, 5u);
  EXPECT_EQ(out.payload.size(), 4096u);
  EXPECT_TRUE(out.payload == in.payload);
}

void test_record_default_encode_v2() {
  data_record in;
  in.type = record_type::data;
  in.raft_term = 99;
  in.payload.assign(128, 'B');

  // Explicit v2 still encodes; default encode is v3 (log=STM).
  auto v2 = encode_record(in, kFormatVersion);
  EXPECT_EQ(v2.size(), static_cast<size_t>(kRecordHeaderLenV1 + 128));
  auto bytes = encode_record(in);
  EXPECT_EQ(bytes.size(), static_cast<size_t>(kRecordHeaderLenV3 + 128));

  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
  EXPECT_TRUE(st == decode_status::ok);
  EXPECT_EQ(out.header_len, kRecordHeaderLenV3);
  EXPECT_EQ(out.raft_term, 99u);
}

void test_record_illegal_version_and_header_len() {
  data_record in;
  in.type = record_type::data;
  in.payload = "x";
  auto bytes = encode_record(in);

  // format_version at offset 4
  bytes[4] = 99;
  bytes[5] = 0;
  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
  EXPECT_TRUE(st == decode_status::bad_version);

  bytes = encode_record(in);
  // header_len at offset 8; 70 is neither 78 nor 86
  bytes[8] = 70;
  bytes[9] = 0;
  st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
  EXPECT_TRUE(st == decode_status::bad_header_len);
}

void test_record_v3_short_header_need_more() {
  data_record in;
  in.type = record_type::data;
  in.raft_term = 3;
  in.payload.assign(4096, 'C');
  auto bytes = encode_record(in, kFormatVersionV3);
  EXPECT_TRUE(bytes.size() > 80);

  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), 80, &out, &consumed);
  EXPECT_TRUE(st == decode_status::need_more);
}

void test_record_need_more() {
  data_record in;
  in.type = record_type::data;
  in.object_id = 1;
  in.payload.assign(512, 'x'); // still ok for codec; alignment enforced at store
  auto bytes = encode_record(in);

  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), 10, &out, &consumed);
  EXPECT_TRUE(st == decode_status::need_more);

  st = decode_record(bytes.data(), bytes.size() - 1, &out, &consumed);
  EXPECT_TRUE(st == decode_status::need_more);
}

void test_record_bad_payload_crc() {
  data_record in;
  in.type = record_type::data;
  in.payload = "hello";
  auto bytes = encode_record(in);
  // Flip a payload byte
  bytes.back() ^= 0xff;

  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
  EXPECT_TRUE(st == decode_status::bad_payload_checksum);
  // Scanner needs the full record length to skip a torn payload.
  EXPECT_EQ(consumed, bytes.size());
}

void test_record_bad_magic() {
  data_record in;
  in.type = record_type::delete_tombstone;
  auto bytes = encode_record(in);
  bytes[0] ^= 0x1;

  data_record out;
  size_t consumed = 0;
  auto st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
  EXPECT_TRUE(st == decode_status::bad_magic);
}

void test_extent_map_overwrite() {
  extent_map m;

  extent e0;
  e0.logical_offset = 0;
  e0.length = 8192;
  e0.raft_create_index = 1;
  e0.phys_segment = 0;
  e0.phys_offset = 0;
  m.put(e0);
  EXPECT_EQ(m.size(), 1u);

  // Overwrite middle 4K
  extent e1;
  e1.logical_offset = 4096;
  e1.length = 4096;
  e1.raft_create_index = 2;
  e1.phys_segment = 0;
  e1.phys_offset = 100;
  m.put(e1);

  // Expect left [0,4K), mid [4K,8K) from e1, no right (e0 ended at 8K)
  EXPECT_EQ(m.size(), 2u);
  auto q = m.query(0, 8192);
  EXPECT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0].logical_offset, 0u);
  EXPECT_EQ(q[0].length, 4096u);
  EXPECT_EQ(q[0].raft_create_index, 1u);
  EXPECT_EQ(q[1].logical_offset, 4096u);
  EXPECT_EQ(q[1].length, 4096u);
  EXPECT_EQ(q[1].raft_create_index, 2u);
}

void test_extent_map_split_right() {
  extent_map m;
  extent e0{0, 12288, 1, 0, 0, 0};
  m.put(e0);

  extent e1{4096, 4096, 2, 0, 1, 50};
  m.put(e1);

  // left [0,4K), mid [4K,8K)@2, right [8K,12K)
  EXPECT_EQ(m.size(), 3u);
  auto q = m.query(0, 12288);
  EXPECT_EQ(q.size(), 3u);
  EXPECT_EQ(q[0].length, 4096u);
  EXPECT_EQ(q[1].raft_create_index, 2u);
  EXPECT_EQ(q[2].logical_offset, 8192u);
  EXPECT_EQ(q[2].length, 4096u);
  EXPECT_EQ(q[2].raft_create_index, 1u);
}

void test_extent_map_discard() {
  extent_map m;
  extent e0{0, 8192, 1, 0, 0, 0};
  m.put(e0);
  m.discard(0, 4096);
  EXPECT_EQ(m.size(), 1u);
  auto q = m.query(0, 8192);
  EXPECT_EQ(q.size(), 1u);
  EXPECT_EQ(q[0].logical_offset, 4096u);
  EXPECT_EQ(q[0].length, 4096u);
}

void test_alignment_helpers() {
  EXPECT_TRUE(is_aligned(0, 4096));
  EXPECT_TRUE(!is_aligned(1, 4096));
  EXPECT_TRUE(valid_object_range(0, 4096));
  EXPECT_TRUE(!valid_object_range(kMaxObjectSize - 2048, 4096));
}

struct sync_result {
  int err{-999};
  bool done{false};
};

void on_done(void* arg, int err) {
  auto* r = static_cast<sync_result*>(arg);
  r->err = err;
  r->done = true;
}

void test_pg_store_write_read() {
  mem_device_backend dev(64 * 1024 * 1024);
  pg_id id{1, 2, 1};
  pg_store store(&dev, id);

  std::vector<char> data(4096, 'A');
  sync_result wr;
  store.write("obj0", 0, data.data(), data.size(), /*raft*/ 10, /*image*/ 0,
              on_done, &wr);
  EXPECT_TRUE(wr.done);
  EXPECT_EQ(wr.err, 0);

  std::vector<char> out(4096, 0);
  sync_result rr;
  store.read("obj0", 0, out.data(), out.size(), on_done, &rr);
  EXPECT_TRUE(rr.done);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(out == data);

  // Hole reads as zero
  std::vector<char> hole(4096, 1);
  sync_result hr;
  store.read("obj0", 4096, hole.data(), hole.size(), on_done, &hr);
  EXPECT_EQ(hr.err, 0);
  EXPECT_TRUE(std::all_of(hole.begin(), hole.end(),
                          [](char c) { return c == 0; }));
}

void test_pg_store_overwrite_and_idempotent() {
  mem_device_backend dev(64 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});

  std::vector<char> a(4096, 'A');
  std::vector<char> b(4096, 'B');
  sync_result r;
  store.write("o", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("o", 0, b.data(), b.size(), 2, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Idempotent re-apply of raft index 2
  r = {};
  store.write("o", 0, b.data(), b.size(), 2, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 0);
  r = {};
  store.read("o", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == b);
}

void test_pg_store_remove() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> a(4096, 'Z');
  sync_result r;
  store.write("x", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.remove("x", 2, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 9);
  r = {};
  store.read("x", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));
}

void test_pg_store_discard() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});

  // Write three 4K blocks: [A][B][C]
  std::vector<char> a(4096, 'A');
  std::vector<char> b(4096, 'B');
  std::vector<char> c(4096, 'C');
  sync_result r;
  store.write("d", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("d", 4096, b.data(), b.size(), 2, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("d", 8192, c.data(), c.size(), 3, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Discard middle 4K
  r = {};
  store.discard("d", 4096, 4096, 4, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Idempotent re-apply of same discard
  r = {};
  store.discard("d", 4096, 4096, 4, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(12288, 0);
  r = {};
  store.read("d", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.begin() + 4096,
                          [](char ch) { return ch == 'A'; }));
  EXPECT_TRUE(std::all_of(out.begin() + 4096, out.begin() + 8192,
                          [](char ch) { return ch == 0; }));
  EXPECT_TRUE(std::all_of(out.begin() + 8192, out.end(),
                          [](char ch) { return ch == 'C'; }));

  // Unaligned discard rejected
  r = {};
  store.discard("d", 1, 4096, 5, 0, on_done, &r);
  EXPECT_EQ(r.err, -EINVAL);

  // Missing object is no-op success
  r = {};
  store.discard("missing", 0, 4096, 6, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
}

void test_pg_store_discard_recover() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_id id{2, 3, 1};
  {
    pg_store store(&dev, id);
    std::vector<char> a(8192, 'X');
    sync_result r;
    store.write("dr", 0, a.data(), a.size(), 1, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.discard("dr", 0, 4096, 2, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  pg_store store2(&dev, id);
  sync_result rr;
  store2.recover(on_done, &rr);
  EXPECT_TRUE(rr.done);
  EXPECT_EQ(rr.err, 0);

  std::vector<char> out(8192, 7);
  rr = {};
  store2.read("dr", 0, out.data(), out.size(), on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.begin() + 4096,
                          [](char ch) { return ch == 0; }));
  EXPECT_TRUE(std::all_of(out.begin() + 4096, out.end(),
                          [](char ch) { return ch == 'X'; }));
}

void test_pg_store_replay_image() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_id id{9, 8, 3};
  pg_store store(&dev, id);

  std::vector<char> a(4096, 'Q');
  sync_result r;
  store.write("re", 0, a.data(), a.size(), 5, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("re", 4096, a.data(), a.size(), 6, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Rebuild from committed image [0, write_pointer).
  mem_device_backend dev2(32 * 1024 * 1024);
  const uint64_t wp = dev.write_pointer();
  EXPECT_TRUE(wp > 0);
  std::memcpy(dev2.storage().data(), dev.storage().data(),
              static_cast<size_t>(wp));
  dev2.set_write_pointer(wp);

  pg_store store2(&dev2, id);
  const uint64_t rebuilt =
      store2.rebuild_from_image(dev2.storage().data(), static_cast<size_t>(wp));
  EXPECT_EQ(rebuilt, wp);

  std::vector<char> out(8192, 0);
  r = {};
  store2.read("re", 0, out.data(), 8192, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.begin() + 4096,
                          [](char c) { return c == 'Q'; }));
  EXPECT_TRUE(std::all_of(out.begin() + 4096, out.end(),
                          [](char c) { return c == 'Q'; }));
}

void test_pg_store_async_recover() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_id id{3, 1, 1};
  {
    pg_store store(&dev, id);
    std::vector<char> a(4096, 'R');
    sync_result r;
    store.write("rec", 0, a.data(), a.size(), 1, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.write("rec", 4096, a.data(), a.size(), 2, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  // New store instance, same device image; write_pointer still at end of data.
  pg_store store2(&dev, id);
  sync_result rr;
  store2.recover(on_done, &rr);
  EXPECT_TRUE(rr.done);
  EXPECT_EQ(rr.err, 0);

  std::vector<char> out(8192, 0);
  rr = {};
  store2.read("rec", 0, out.data(), 8192, on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 'R'; }));
}

void test_pg_store_queue_serialized() {
  // With mem backend completions are sync, but queue path must not drop ops.
  mem_device_backend dev(16 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 1, 1});
  std::vector<char> a(4096, '1');
  std::vector<char> b(4096, '2');
  sync_result r1;
  sync_result r2;
  store.write("q", 0, a.data(), a.size(), 1, 0, on_done, &r1);
  store.write("q", 4096, b.data(), b.size(), 2, 0, on_done, &r2);
  EXPECT_EQ(r1.err, 0);
  EXPECT_EQ(r2.err, 0);

  std::vector<char> out(8192, 0);
  sync_result rr;
  store.read("q", 0, out.data(), 8192, on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(out[0], '1');
  EXPECT_EQ(out[4096], '2');
}

void test_checkpoint_payload_roundtrip() {
  checkpoint_snapshot in;
  in.generation = 3;
  in.pool_id = 1;
  in.pg_id = 2;
  in.store_epoch = 4;
  in.next_object_id = 9;
  checkpoint_object o;
  o.object_id = 7;
  o.name = "obj";
  o.deleted = false;
  o.delete_raft_index = 0;
  checkpoint_extent e{0, 4096, 5, 0, 0, 1234};
  o.extents.push_back(e);
  o.applied_writes.push_back({{0, 4096}, 5});
  in.objects.push_back(o);

  auto bytes = encode_checkpoint_payload(in);
  checkpoint_snapshot out;
  EXPECT_TRUE(decode_checkpoint_payload(bytes, &out));
  EXPECT_EQ(out.generation, 3u);
  EXPECT_EQ(out.pool_id, 1u);
  EXPECT_EQ(out.pg_id, 2u);
  EXPECT_EQ(out.store_epoch, 4u);
  EXPECT_EQ(out.next_object_id, 9u);
  EXPECT_EQ(out.objects.size(), 1u);
  EXPECT_EQ(out.objects[0].object_id, 7u);
  EXPECT_TRUE(out.objects[0].name == "obj");
  EXPECT_EQ(out.objects[0].extents.size(), 1u);
  EXPECT_EQ(out.objects[0].extents[0].phys_offset, 1234u);
  EXPECT_EQ(out.objects[0].applied_writes.size(), 1u);

  // Corrupt trailing CRC
  bytes.back() ^= 0xff;
  EXPECT_TRUE(!decode_checkpoint_payload(bytes, &out));
}

void test_pg_store_checkpoint_and_recover() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_id id{5, 6, 1};
  {
    pg_store store(&dev, id);
    std::vector<char> a(4096, 'A');
    std::vector<char> b(4096, 'B');
    sync_result r;
    store.write("c0", 0, a.data(), a.size(), 1, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.write("c1", 0, b.data(), b.size(), 2, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.checkpoint(on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(store.checkpoint_generation(), 1u);
    EXPECT_TRUE(store.last_checkpoint_end() > 0);

    // Post-checkpoint write
    std::vector<char> c(4096, 'C');
    r = {};
    store.write("c0", 4096, c.data(), c.size(), 3, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  pg_store store2(&dev, id);
  sync_result rr;
  store2.recover(on_done, &rr);
  EXPECT_TRUE(rr.done);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(store2.checkpoint_generation(), 1u);
  EXPECT_EQ(store2.object_count(), 2u);

  std::vector<char> out(8192, 0);
  rr = {};
  store2.read("c0", 0, out.data(), 8192, on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.begin() + 4096,
                          [](char ch) { return ch == 'A'; }));
  EXPECT_TRUE(std::all_of(out.begin() + 4096, out.end(),
                          [](char ch) { return ch == 'C'; }));

  std::vector<char> out1(4096, 0);
  rr = {};
  store2.read("c1", 0, out1.data(), 4096, on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(std::all_of(out1.begin(), out1.end(),
                          [](char ch) { return ch == 'B'; }));
}

void test_pg_store_torn_checkpoint_fallback() {
  // Complete checkpoint, then a post-ckpt write, then inject torn tail
  // so the next (incomplete) checkpoint/data is discarded.
  mem_device_backend dev(32 * 1024 * 1024);
  pg_id id{7, 1, 1};
  uint64_t good_end = 0;
  {
    pg_store store(&dev, id);
    std::vector<char> a(4096, 'T');
    sync_result r;
    store.write("t", 0, a.data(), a.size(), 1, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.checkpoint(on_done, &r);
    EXPECT_EQ(r.err, 0);
    good_end = dev.write_pointer();

    // Append garbage / half record after durable region.
    auto& storage = dev.storage();
    const uint64_t wp = good_end;
    if (wp + 4096 <= storage.size()) {
      std::memset(storage.data() + wp, 0x5a, 4096);
      // Pretend write pointer advanced past torn region.
      dev.set_write_pointer(wp + 4096);
    }
  }

  pg_store store2(&dev, id);
  sync_result rr;
  store2.recover(on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(store2.checkpoint_generation(), 1u);

  std::vector<char> out(4096, 0);
  rr = {};
  store2.read("t", 0, out.data(), 4096, on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(),
                          [](char ch) { return ch == 'T'; }));
  // Write pointer truncated to last good record end.
  EXPECT_EQ(dev.write_pointer(), good_end);
}

void test_segment_allocator_no_double_alloc() {
  // 4 x 1MiB segments
  segment_allocator alloc(4ull * 1024 * 1024, 1024 * 1024, 4096);
  EXPECT_EQ(alloc.segment_count(), 4u);
  EXPECT_EQ(alloc.free_count(), 4u);

  uint64_t p0 = 0, p1 = 0;
  uint32_t s0 = 0, s1 = 0;
  EXPECT_EQ(alloc.reserve(4096, &p0, &s0), 0);
  EXPECT_EQ(s0, 0u);
  EXPECT_EQ(p0, 0u);
  EXPECT_EQ(alloc.reserve(4096, &p1, &s1), 0);
  EXPECT_EQ(s1, 0u);
  EXPECT_EQ(p1, 4096u);
  EXPECT_TRUE(p1 != p0);

  // Fill almost entire segment then force seal + open next
  const uint64_t almost = 1024ull * 1024 - 8192;
  uint64_t px = 0;
  EXPECT_EQ(alloc.reserve(almost, &px, nullptr), 0);

  uint64_t p2 = 0;
  uint32_t s2 = 0;
  EXPECT_EQ(alloc.reserve(4096, &p2, &s2), 0);
  EXPECT_EQ(s2, 1u); // new segment
  EXPECT_EQ(p2, 1024ull * 1024);
  EXPECT_EQ(alloc.sealed_count(), 1u);
  EXPECT_EQ(alloc.state(0), segment_state::sealed);
  EXPECT_EQ(alloc.state(1), segment_state::open);

  // Exhaust all segments
  for (uint32_t i = 1; i < 4; ++i) {
    // fill remaining of current open then next
    while (alloc.open_segment() == i) {
      uint64_t p = 0;
      const int rc = alloc.reserve(4096, &p, nullptr);
      if (rc == -ENOSPC) {
        break;
      }
      EXPECT_EQ(rc, 0);
    }
  }
  // After filling, further reserve must ENOSPC (no free segment)
  // Fill segment 3 completely if open
  while (alloc.free_count() > 0 || alloc.open_segment() != segment_allocator::kInvalidSeg) {
    uint64_t p = 0;
    const int rc = alloc.reserve(4096, &p, nullptr);
    if (rc != 0) {
      EXPECT_EQ(rc, -ENOSPC);
      break;
    }
  }
  uint64_t pfail = 0;
  EXPECT_EQ(alloc.reserve(4096, &pfail, nullptr), -ENOSPC);
}

void test_note_written_does_not_rewind_cursor() {
  segment_allocator alloc(4ull * 1024 * 1024, 1024 * 1024, 4096);
  uint64_t p0 = 0, p1 = 0, p2 = 0;
  EXPECT_EQ(alloc.reserve(4096, &p0, nullptr), 0);
  EXPECT_EQ(alloc.reserve(4096, &p1, nullptr), 0);
  EXPECT_EQ(p1, 4096u);
  // Late/out-of-order completion of the first write must not rewind
  // open_cursor over the second reservation.
  alloc.note_written(p0, 4096);
  EXPECT_EQ(alloc.open_cursor(), 8192u);
  EXPECT_EQ(alloc.reserve(4096, &p2, nullptr), 0);
  EXPECT_EQ(p2, 8192u);
  EXPECT_TRUE(p2 != p1);
}

void test_pg_store_raft_snapshot_read() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});

  std::vector<char> a(4096, 'A');
  std::vector<char> b(4096, 'B');
  sync_result r;
  store.write("s", 0, a.data(), a.size(), /*raft*/ 10, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Pin snapshot at index 10 (sees 'A')
  r = {};
  store.create_raft_snapshot(10, /*term*/ 1, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.snapshot_pin_count(10), 1u);
  EXPECT_EQ(store.total_snapshot_pins(), 1u);

  // Overwrite head with 'B' at index 20
  r = {};
  store.write("s", 0, b.data(), b.size(), 20, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 0);
  r = {};
  store.read("s", 0, out.data(), 4096, on_done, &r); // head
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'B');

  r = {};
  out.assign(4096, 0);
  store.read("s", 0, out.data(), 4096, /*raft_read_index*/ 10, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');

  // Unpin; head still B
  r = {};
  store.release_raft_snapshot(10, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.snapshot_pin_count(10), 0u);
  EXPECT_EQ(store.total_snapshot_pins(), 0u);

  // Snapshot view still works from version log even after unpin
  r = {};
  out.assign(4096, 0);
  store.read("s", 0, out.data(), 4096, 10, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');

  // Recover rebuilds pin + versions
  {
    pg_store store2(&dev, pg_id{1, 0, 1});
    // Re-create pin was released on disk via unpin; total pins 0 after recover
    r = {};
    store2.recover(on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    out.assign(4096, 0);
    store2.read("s", 0, out.data(), 4096, 10, on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(out[0], 'A');
    r = {};
    out.assign(4096, 0);
    store2.read("s", 0, out.data(), 4096, on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(out[0], 'B');
  }
}

void test_pg_store_image_snapshot_read() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});

  std::vector<char> a(4096, 'A');
  std::vector<char> b(4096, 'B');
  std::vector<char> c(4096, 'C');
  sync_result r;

  // Write under image_snap_seq=5 (visible to snaps with seq >= 5)
  store.write("img", 0, a.data(), a.size(), /*raft*/ 1, /*image*/ 5, on_done,
              &r);
  EXPECT_EQ(r.err, 0);

  // Pin image snapshot at seq 5 (sees 'A')
  r = {};
  store.create_image_snapshot(5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.image_snapshot_pin_count(5), 1u);
  EXPECT_EQ(store.total_image_snapshot_pins(), 1u);

  // Clone-style re-pin
  r = {};
  store.create_image_snapshot(5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.image_snapshot_pin_count(5), 2u);

  // Later write under image_snap_seq=10 (not visible at snap 5)
  r = {};
  store.write("img", 0, b.data(), b.size(), 2, 10, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Head sees B
  std::vector<char> out(4096, 0);
  r = {};
  store.read("img", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'B');

  // Image view at seq 5 sees A (raft_read_index=0, image_read_seq=5)
  r = {};
  out.assign(4096, 0);
  store.read("img", 0, out.data(), 4096, /*raft*/ 0, /*image*/ 5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');

  // Image view at seq 10 sees B
  r = {};
  out.assign(4096, 0);
  store.read("img", 0, out.data(), 4096, 0, 10, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'B');

  // Unpin once (clone drop); pin remains for reader
  r = {};
  store.release_image_snapshot(5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.image_snapshot_pin_count(5), 1u);

  // Discard half under seq 15, then overwrite
  r = {};
  store.discard("img", 0, 4096, 3, 15, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("img", 0, c.data(), c.size(), 4, 15, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Snap 5 still A; head is C
  r = {};
  out.assign(4096, 0);
  store.read("img", 0, out.data(), 4096, 0, 5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');
  r = {};
  out.assign(4096, 0);
  store.read("img", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'C');

  // Full unpin
  r = {};
  store.release_image_snapshot(5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.image_snapshot_pin_count(5), 0u);
  EXPECT_EQ(store.total_image_snapshot_pins(), 0u);

  // Recover: version log still serves image view; pins were released
  {
    pg_store store2(&dev, pg_id{1, 0, 1});
    r = {};
    store2.recover(on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(store2.image_snapshot_pin_count(5), 0u);
    r = {};
    out.assign(4096, 0);
    store2.read("img", 0, out.data(), 4096, 0, 5, on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(out[0], 'A');
    r = {};
    out.assign(4096, 0);
    store2.read("img", 0, out.data(), 4096, on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(out[0], 'C');
  }
}

void test_segment_allocator_free_sealed() {
  segment_allocator alloc(4ull * 1024 * 1024, 1024 * 1024, 4096);
  uint64_t p = 0;
  EXPECT_EQ(alloc.reserve(4096, &p, nullptr), 0);
  // fill and roll to seal seg0
  const uint64_t almost = 1024ull * 1024 - 4096;
  EXPECT_EQ(alloc.reserve(almost, &p, nullptr), 0);
  EXPECT_EQ(alloc.reserve(4096, &p, nullptr), 0); // opens seg1
  EXPECT_EQ(alloc.state(0), segment_state::sealed);
  EXPECT_EQ(alloc.free_sealed(0), 0);
  EXPECT_EQ(alloc.state(0), segment_state::free);
  // 4 segs: 0 free, 1 open, 2 free, 3 free => free_count=3
  EXPECT_EQ(alloc.free_count(), 3u);
  EXPECT_EQ(alloc.free_sealed(1), -EBUSY); // open
}

void test_osd_store_gc_once() {
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  pg_store* pg = osd.get_latest_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);

  // Fill first segment with object "old", then write "new" into next segment.
  std::vector<char> buf(4096, 'X');
  int idx = 0;
  while (osd.allocator()->sealed_count() < 1 && idx < 40) {
    r = {};
    std::string name = "old" + std::to_string(idx);
    pg->write(name, 0, buf.data(), buf.size(), static_cast<uint64_t>(idx + 1),
              0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++idx;
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 1u);

  r = {};
  pg->write("keep", 0, buf.data(), buf.size(), 100, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Remove all old* so sealed early segments lose live refs.
  for (int i = 0; i < idx; ++i) {
    r = {};
    pg->remove("old" + std::to_string(i), 200 + static_cast<uint64_t>(i), 0,
               on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  const uint32_t free_before = osd.allocator()->free_count();
  const uint32_t sealed_before = osd.allocator()->sealed_count();
  const uint32_t freed = osd.gc_once(true);
  EXPECT_TRUE(freed >= 1u);
  EXPECT_TRUE(osd.allocator()->free_count() > free_before);
  EXPECT_TRUE(osd.allocator()->sealed_count() < sealed_before);

  // keep still readable
  std::vector<char> out(4096, 0);
  r = {};
  pg->read("keep", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'X');
}

void test_gc_pins_object_name_across_free() {
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  pg_store* pg = osd.get_latest_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);

  std::vector<char> buf(4096, 'K');
  r = {};
  pg->write("keep", 0, buf.data(), buf.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  int idx = 0;
  std::vector<char> junk(4096, 'Z');
  while (osd.allocator()->sealed_count() < 1 && idx < 40) {
    r = {};
    pg->write("tmp" + std::to_string(idx), 0, junk.data(), junk.size(),
              static_cast<uint64_t>(10 + idx), 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++idx;
  }
  for (int i = 0; i < idx; ++i) {
    r = {};
    pg->remove("tmp" + std::to_string(i), 100 + static_cast<uint64_t>(i), 0,
               on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  (void)osd.gc_once(true);

  std::vector<char> out(4096, 0);
  r = {};
  pg->read("keep", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'K');
}

void test_gc_pass_async_completes_before_free() {
  const uint64_t seg = 32 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  pg_store* pg = osd.get_latest_pg(1, 0);
  std::vector<char> a(4096, 'A');
  r = {};
  pg->write("keep0", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> filler(4096, 'Z');
  int idx = 0;
  while (osd.allocator()->sealed_count() < 1 && idx < 80) {
    r = {};
    pg->write("tmp" + std::to_string(idx), 0, filler.data(), filler.size(),
              static_cast<uint64_t>(10 + idx), 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++idx;
  }
  for (int i = 0; i < idx; ++i) {
    r = {};
    pg->remove("tmp" + std::to_string(i), 1000 + static_cast<uint64_t>(i), 0,
               on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  osd_store::gc_options opt;
  opt.max_rewrite_extents = 8;
  r = {};
  osd.gc_pass_async(opt, on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> out(4096, 0);
  r = {};
  pg->read("keep0", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');
}

void test_osd_store_apply_read_remove() {
  mem_device_backend dev(16 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, /*segment*/ 0);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(7, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'W');
  r = {};
  osd.apply(7, 1, "o", 0, data.data(), data.size(), /*raft*/ 42, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Idempotent re-apply same raft index + range
  r = {};
  osd.apply(7, 1, "o", 0, data.data(), data.size(), 42, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 0);
  r = {};
  osd.read(7, 1, "o", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'W');

  // Overwrite with newer raft index
  std::vector<char> data2(4096, 'X');
  r = {};
  osd.apply(7, 1, "o", 0, data2.data(), data2.size(), 43, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  out.assign(4096, 0);
  osd.read(7, 1, "o", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'X');

  r = {};
  osd.remove(7, 1, "o", 44, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  out.assign(4096, 0);
  osd.read(7, 1, "o", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 0);
}

void test_snapshot_ingest_state_machine() {
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* active0 = osd.get_active_pg(1, 0);
  EXPECT_TRUE(active0 != nullptr);
  std::vector<char> oldv(4096, 'O');
  r = {};
  active0->write("obj", 0, oldv.data(), oldv.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  snapshot_ingest ing(&osd, 1, 0);
  EXPECT_TRUE(ing.state() == ingest_state::idle);

  r = {};
  ing.begin(/*snap*/ 100, /*term*/ 3, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(ing.state() == ingest_state::ingesting);
  EXPECT_TRUE(ing.staging_epoch() > ing.active_epoch());

  // Active still serves old data during ingest.
  std::vector<char> out(4096, 0);
  r = {};
  osd.get_active_pg(1, 0)->read("obj", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'O');

  std::vector<char> newv(4096, 'N');
  const uint32_t crc = crc32(newv.data(), newv.size());
  r = {};
  ing.apply_chunk("obj", 0, newv.data(), newv.size(), crc, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Bad CRC rejected.
  r = {};
  ing.apply_chunk("obj", 0, newv.data(), newv.size(), crc ^ 1u, on_done, &r);
  EXPECT_EQ(r.err, -EBADMSG);

  r = {};
  ing.seal(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(ing.state() == ingest_state::sealed);
  EXPECT_TRUE(ing.staging_pg()->install_sealed());

  r = {};
  ing.activate(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(ing.state() == ingest_state::catchup);

  // Serving now sees snapshot data.
  r = {};
  out.assign(4096, 0);
  osd.get_active_pg(1, 0)->read("obj", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'N');

  r = {};
  ing.finish_catchup(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(ing.state() == ingest_state::idle);

  // Abort path: second install then abort restores old serving epoch.
  const uint64_t active_after = osd.serving_epoch(1, 0);
  snapshot_ingest ing2(&osd, 1, 0);
  r = {};
  ing2.begin(200, 4, on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> bad(4096, 'X');
  r = {};
  ing2.apply_chunk("obj", 0, bad.data(), bad.size(), 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  ing2.abort(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(ing2.state() == ingest_state::idle);
  EXPECT_EQ(osd.serving_epoch(1, 0), active_after);
  r = {};
  out.assign(4096, 0);
  osd.get_active_pg(1, 0)->read("obj", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'N');
}

void test_pg_snapshot_reader_stream() {
  mem_device_backend dev(16 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});

  std::vector<char> a(4096, 'A');
  std::vector<char> b(4096, 'B');
  sync_result r;
  store.write("o1", 0, a.data(), a.size(), 5, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("o2", 0, b.data(), b.size(), 6, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Overwrite o1 after snapshot index 6 — snapshot@6 still sees A.
  std::vector<char> c(4096, 'C');
  r = {};
  store.write("o1", 0, c.data(), c.size(), 10, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  struct open_pack {
    int err{-1};
    pg_snapshot_reader* reader{nullptr};
  } op;
  pg_snapshot_reader::open(
      &store, /*raft*/ 6, /*term*/ 1,
      [](void* a, int err, pg_snapshot_reader* reader) {
        auto* p = static_cast<open_pack*>(a);
        p->err = err;
        p->reader = reader;
      },
      &op);
  EXPECT_EQ(op.err, 0);
  EXPECT_TRUE(op.reader != nullptr);
  EXPECT_TRUE(store.snapshot_pin_count(6) >= 1u);
  EXPECT_TRUE(op.reader->total_chunks() >= 2u);

  std::vector<char> buf(4096, 0);
  int chunks = 0;
  char seen_o1 = 0;
  char seen_o2 = 0;
  for (;;) {
    struct {
      int err{-1};
      bool done{false};
      std::string name;
      char first{0};
    } ch;
    op.reader->next_chunk(
        buf.data(), buf.size(),
        [](void* a, int err, bool done, const char* name, uint64_t /*off*/,
           uint64_t /*len*/) {
          auto* p = static_cast<decltype(ch)*>(a);
          p->err = err;
          p->done = done;
          if (!done && name != nullptr) {
            p->name = name;
          }
        },
        &ch);
    // Copy first byte before next overwrites buf — next_chunk already filled buf
    if (ch.err == 0 && !ch.done) {
      ch.first = buf[0];
    }
    EXPECT_EQ(ch.err, 0);
    if (ch.done) {
      break;
    }
    ++chunks;
    if (ch.name == "o1") {
      seen_o1 = ch.first;
    }
    if (ch.name == "o2") {
      seen_o2 = ch.first;
    }
  }
  EXPECT_TRUE(chunks >= 2);
  EXPECT_EQ(seen_o1, 'A');
  EXPECT_EQ(seen_o2, 'B');

  r = {};
  op.reader->close(on_done, &r);
  EXPECT_EQ(r.err, 0);
  delete op.reader;
  EXPECT_EQ(store.snapshot_pin_count(6), 0u);
}

void test_pg_store_rewrite_extent() {
  mem_device_backend dev(16 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});

  std::vector<char> a(4096, 'A');
  sync_result r;
  store.write("rw", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Collect head extent and rewrite it to a new physical location.
  std::vector<std::tuple<std::string, uint64_t, uint64_t>> targets;
  store.collect_head_extents_in_range(0, dev.capacity(), &targets);
  EXPECT_EQ(targets.size(), 1u);

  r = {};
  store.rewrite_extent("rw", 0, 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 0);
  r = {};
  store.read("rw", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');

  // Recover must still see rewritten data via gc_rewrite record.
  {
    pg_store store2(&dev, pg_id{1, 0, 1});
    r = {};
    store2.recover(on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    out.assign(4096, 0);
    store2.read("rw", 0, out.data(), 4096, on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(out[0], 'A');
  }

  // Rewrite must append a fresh object_name so GC can reclaim the original
  // name+data segment without orphaning the name on recover.
  {
    const auto& img = dev.storage();
    uint32_t name_recs = 0;
    size_t off = 0;
    while (off + kRecordHeaderLenV1 < img.size()) {
      data_record rec;
      size_t consumed = 0;
      const decode_status st =
          decode_record(img.data() + off, img.size() - off, &rec, &consumed);
      if (st != decode_status::ok || consumed == 0) {
        off += 4096;
        continue;
      }
      if (rec.type == record_type::object_name && rec.payload == "rw") {
        ++name_recs;
      }
      const size_t step = (consumed + 4095) / 4096 * 4096;
      off += step == 0 ? 4096 : step;
    }
    EXPECT_TRUE(name_recs >= 2u);
  }
}

void test_osd_store_gc_rewrite_once() {
  // write() publishes immediately; recover of that path is flag=0.
  unified_log_guard off(false);
  // Small segments force early seal; survivors stay on sealed segment 0.
  const uint64_t seg = 32 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  pg_store* pg = osd.get_latest_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);

  std::vector<char> a(4096, 'A');
  r = {};
  pg->write("keep0", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Pad until segment 0 is sealed; keep0 remains live on sealed region.
  std::vector<char> filler(4096, 'Z');
  int idx = 0;
  while (osd.allocator()->sealed_count() < 1 && idx < 80) {
    r = {};
    pg->write("tmp" + std::to_string(idx), 0, filler.data(), filler.size(),
              static_cast<uint64_t>(10 + idx), 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++idx;
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 1u);

  for (int i = 0; i < idx; ++i) {
    r = {};
    pg->remove("tmp" + std::to_string(i), 1000 + static_cast<uint64_t>(i), 0,
               on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  // Direct rewrite of keep0 must succeed (validates rewrite path).
  r = {};
  pg->rewrite_extent("keep0", 0, 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // gc_rewrite_once should rewrite any remaining sealed live heads (or free dead).
  const uint32_t n = osd.gc_rewrite_once(true);
  (void)n;

  std::vector<char> out(4096, 0);
  r = {};
  pg->read("keep0", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');

  // Reclaim the original name+data segment. Recover must still resolve keep0
  // via the name record rewrite_extent re-emitted next to the new payload.
  (void)osd.gc_once(true);

  {
    osd_store osd2(&dev, buffer_mode::heap, seg);
    r = {};
    osd2.recover(on_done, &r);
    EXPECT_EQ(r.err, 0);
    pg_store* pg2 = osd2.get_latest_pg(1, 0);
    EXPECT_TRUE(pg2 != nullptr);
    if (pg2 == nullptr) {
      return;
    }
    EXPECT_TRUE(pg2->object_exists("keep0"));
    r = {};
    out.assign(4096, 0);
    pg2->read("keep0", 0, out.data(), 4096, on_done, &r);
    EXPECT_EQ(r.err, 0);
    EXPECT_EQ(out[0], 'A');
  }
}

void test_superblock_manifest_codec() {
  superblock sb;
  sb.generation = 3;
  sb.segment_size = 1024 * 1024;
  sb.data_offset = kMetaSize;
  sb.data_length = 8 * 1024 * 1024;
  sb.manifest_generation = 2;
  auto bytes = encode_superblock(sb);
  EXPECT_EQ(bytes.size(), static_cast<size_t>(kSuperblockBytes));
  superblock out;
  EXPECT_TRUE(decode_superblock(bytes.data(), bytes.size(), &out));
  EXPECT_EQ(out.generation, 3u);
  EXPECT_EQ(out.manifest_generation, 2u);
  bytes[20] ^= 0xff;
  EXPECT_TRUE(!decode_superblock(bytes.data(), bytes.size(), &out));

  manifest m;
  m.generation = 5;
  m.data_high_water = kMetaSize + 4096;
  m.open_segment_id = 0;
  m.open_cursor = 4096;
  m.pgs.push_back({1, 2, 7});
  auto mb = encode_manifest(m);
  manifest mo;
  EXPECT_TRUE(decode_manifest(mb.data(), mb.size(), &mo));
  EXPECT_EQ(mo.generation, 5u);
  EXPECT_EQ(mo.pgs.size(), 1u);
  EXPECT_EQ(mo.pgs[0].active_epoch, 7u);

  manifest v2;
  v2.generation = 9;
  v2.data_high_water = kMetaSize + 8192;
  v2.recover_scan_from = kMetaSize + 4096;
  v2.kv_checkpoint_phys = kMetaSize + 1024;
  v2.open_segment_id = 1;
  v2.open_cursor = 512;
  v2.pgs.push_back({2, 3, 4});
  v2.object_checkpoints.push_back({2, 3, 4, kMetaSize + 2048});
  v2.log_checkpoints.push_back({2, 3, 4, kMetaSize + 3072});
  v2.segment_states = {0, 1, 2, 0};
  auto vb = encode_manifest(v2);
  manifest vo;
  EXPECT_TRUE(decode_manifest(vb.data(), vb.size(), &vo));
  EXPECT_EQ(vo.recover_scan_from, kMetaSize + 4096);
  EXPECT_EQ(vo.kv_checkpoint_phys, kMetaSize + 1024);
  EXPECT_EQ(vo.object_checkpoints.size(), 1u);
  EXPECT_EQ(vo.log_checkpoints[0].phys, kMetaSize + 3072);
  EXPECT_EQ(vo.segment_states.size(), 4u);
  EXPECT_EQ(vo.segment_states[2], 2);
}

void test_osd_store_multi_pg_shared_allocator() {
  unified_log_guard off(false);
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);

  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(osd.superblock_generation(), 1u);
  EXPECT_TRUE(osd.data_offset() == kMetaSize);
  EXPECT_TRUE(osd.allocator()->region_base() == kMetaSize);

  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* a = osd.get_latest_pg(1, 0);
  pg_store* b = osd.get_latest_pg(1, 1);
  EXPECT_TRUE(a != nullptr && b != nullptr);
  EXPECT_TRUE(a->allocator() == b->allocator());
  EXPECT_EQ(osd.latest_epoch(1, 0), 1u);
  EXPECT_EQ(osd.latest_epoch(1, 1), 1u);

  std::vector<char> da(4096, 'A');
  std::vector<char> db(4096, 'B');
  r = {};
  a->write("oa", 0, da.data(), da.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  b->write("ob", 0, db.data(), db.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Cross-PG isolation on live stores
  std::vector<char> out(4096, 0);
  r = {};
  a->read("oa", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');
  r = {};
  a->read("ob", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));

  // Shared allocator should have consumed space once
  EXPECT_TRUE(osd.allocator()->open_cursor() > 0 ||
              osd.allocator()->sealed_count() > 0);

  // Persist latest high-water after writes
  r = {};
  osd.persist_manifest(on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Recover into a fresh osd_store on same device image
  osd_store osd2(&dev, buffer_mode::heap, seg);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(osd2.pg_count(), 2u);
  EXPECT_TRUE(osd2.superblock_generation() >= 1u);
  EXPECT_EQ(osd2.latest_epoch(1, 0), 1u);

  pg_store* a2 = osd2.get_latest_pg(1, 0);
  pg_store* b2 = osd2.get_latest_pg(1, 1);
  EXPECT_TRUE(a2 && b2);
  r = {};
  a2->read("oa", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');
  r = {};
  b2->read("ob", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'B');

  // delete_pg + recreate gets higher epoch; epoch survives restart via manifest
  r = {};
  osd2.delete_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(osd2.latest_epoch(1, 0), 1u); // remembered
  r = {};
  osd2.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(osd2.latest_epoch(1, 0), 2u);

  osd_store osd3(&dev, buffer_mode::heap, seg);
  r = {};
  osd3.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  // Manifest should restore active_epoch=2 even without epoch-2 data records
  EXPECT_EQ(osd3.latest_epoch(1, 0), 2u);
  r = {};
  osd3.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(osd3.latest_epoch(1, 0), 3u);
  pg_store* a3 = osd3.get_latest_pg(1, 0);
  EXPECT_TRUE(a3 != nullptr);
  EXPECT_EQ(a3->id().store_epoch, 3u);
  r = {};
  a3->read("oa", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));
}

void test_pg_store_multi_segment_write_recover() {
  // Small segments force seal across writes.
  const uint64_t seg = 256 * 1024; // 256KiB
  mem_device_backend dev(2 * 1024 * 1024); // 2MiB => 8 segments
  pg_id id{11, 0, 1};
  {
    pg_store store(&dev, id, buffer_mode::heap, seg);
    EXPECT_EQ(store.allocator()->segment_count(), 8u);

    // Several 4K objects / overwrites to cross segment boundary
    for (int i = 0; i < 40; ++i) {
      std::vector<char> buf(4096, static_cast<char>('A' + (i % 26)));
      sync_result r;
      store.write("ms", static_cast<uint64_t>(i % 8) * 4096, buf.data(),
                  buf.size(), static_cast<uint64_t>(i + 1), 0, on_done, &r);
      EXPECT_EQ(r.err, 0);
    }
    EXPECT_TRUE(store.allocator()->sealed_count() >= 1u);
  }

  pg_store store2(&dev, id, buffer_mode::heap, seg);
  sync_result rr;
  store2.recover(on_done, &rr);
  EXPECT_EQ(rr.err, 0);

  std::vector<char> out(4096, 0);
  rr = {};
  store2.read("ms", 0, out.data(), 4096, on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  // Last write that touched offset 0 was i=0,8,16,24,32 → 'A'+0 = 'A' for i=0
  // i=32 → 'A'+6 = 'G' (32%26=6)
  EXPECT_EQ(out[0], static_cast<char>('A' + (32 % 26)));
}

void test_pg_store_checkpoint_then_discard_recover() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_id id{8, 2, 1};
  {
    pg_store store(&dev, id);
    std::vector<char> a(8192, 'Z');
    sync_result r;
    store.write("zd", 0, a.data(), a.size(), 1, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.checkpoint(on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.discard("zd", 0, 4096, 2, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  pg_store store2(&dev, id);
  sync_result rr;
  store2.recover(on_done, &rr);
  EXPECT_EQ(rr.err, 0);

  std::vector<char> out(8192, 1);
  rr = {};
  store2.read("zd", 0, out.data(), 8192, on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.begin() + 4096,
                          [](char ch) { return ch == 0; }));
  EXPECT_TRUE(std::all_of(out.begin() + 4096, out.end(),
                          [](char ch) { return ch == 'Z'; }));
}

void test_parse_object_engine_mode() {
  EXPECT_TRUE(parse_object_engine_mode("blob") == object_engine_mode::blob);
  EXPECT_TRUE(parse_object_engine_mode("AOS") == object_engine_mode::aos);
  EXPECT_TRUE(parse_object_engine_mode("shadow") == object_engine_mode::shadow);
  EXPECT_TRUE(parse_object_engine_mode("append-only") ==
              object_engine_mode::aos);
  EXPECT_TRUE(parse_object_engine_mode("unknown") == object_engine_mode::blob);
  EXPECT_TRUE(std::string(object_engine_mode_name(object_engine_mode::shadow)) ==
              "shadow");
}

void test_compute_shard_region() {
  uint64_t off = 0;
  uint64_t len = 0;
  EXPECT_TRUE(compute_shard_region(256ull * 1024 * 1024, 2, 0, &off, &len));
  EXPECT_EQ(off, 0);
  EXPECT_EQ(len, 128ull * 1024 * 1024);
  EXPECT_TRUE(compute_shard_region(256ull * 1024 * 1024, 2, 1, &off, &len));
  EXPECT_EQ(off, 128ull * 1024 * 1024);
  EXPECT_EQ(len, 128ull * 1024 * 1024);
  EXPECT_TRUE(!compute_shard_region(1024, 2, 0, &off, &len));
}

void test_gc_rewrite_rate_limit() {
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(16 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Fill enough to seal a segment and leave garbage after overwrite.
  for (int i = 0; i < 8; ++i) {
    std::vector<char> data(4096, static_cast<char>('A' + (i % 26)));
    r = {};
    osd.apply(1, 0, "obj", 0, data.data(), data.size(),
              static_cast<uint64_t>(i + 1), 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  // Rate-limited rewrite should return at most max_extents (or free dead segs).
  const uint32_t n = osd.gc_rewrite_once(true, /*max_extents*/ 1);
  EXPECT_TRUE(n <= 1);
}

void test_log_store_append_read_trim() {
  mem_device_backend dev(8 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(),
                                                   /*seg*/ 0, dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);

  for (uint64_t i = 1; i <= 5; ++i) {
    log_entry e;
    e.term = 3;
    e.index = i;
    e.type = 1;
    e.meta = "m" + std::to_string(i);
    e.data = std::string(128, static_cast<char>('a' + static_cast<int>(i)));
    sync_result r;
    log.append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  EXPECT_EQ(log.base_index(), 1);
  EXPECT_EQ(log.last_index(), 5);
  EXPECT_EQ(log.entry_count(), 5);

  struct {
    int err{-1};
    std::vector<log_entry> entries;
  } rr;
  log.read(2, 4,
           [](void* a, std::vector<log_entry> entries, int err) {
             auto* p = static_cast<decltype(rr)*>(a);
             p->err = err;
             p->entries = std::move(entries);
           },
           &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 3);
  EXPECT_EQ(rr.entries[0].index, 2);
  EXPECT_EQ(rr.entries[2].index, 4);
  EXPECT_TRUE(rr.entries[0].meta == "m2");

  log.trim_to(3);
  EXPECT_EQ(log.base_index(), 4);
  EXPECT_EQ(log.last_index(), 5);
  EXPECT_EQ(log.entry_count(), 2);

  rr = {};
  log.read(1, 2,
           [](void* a, std::vector<log_entry> entries, int err) {
             auto* p = static_cast<decltype(rr)*>(a);
             p->err = err;
             p->entries = std::move(entries);
           },
           &rr);
  EXPECT_EQ(rr.err, -ENOENT);
}

void test_log_store_rebuild_from_image() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{9, 1, 1}, alloc, buffer_mode::heap);
  for (uint64_t i = 1; i <= 3; ++i) {
    log_entry e;
    e.term = 1;
    e.index = i;
    e.type = 0;
    e.meta = "x";
    e.data = "payload" + std::to_string(i);
    sync_result r;
    log.append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  // Capture written region via rebuild on a fresh log_store sharing image.
  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  // mem backend: read from start
  sync_result r;
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  log_store log2(&dev, pg_id{9, 1, 1}, alloc2, buffer_mode::heap);
  log2.rebuild_from_image(image.data(), image.size());
  EXPECT_EQ(log2.base_index(), 1);
  EXPECT_EQ(log2.last_index(), 3);

  struct {
    int err{-1};
    std::vector<log_entry> entries;
  } rr;
  log2.read(1, 3,
            [](void* a, std::vector<log_entry> entries, int err) {
              auto* p = static_cast<decltype(rr)*>(a);
              p->err = err;
              p->entries = std::move(entries);
            },
            &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 3);
  EXPECT_TRUE(rr.entries[2].data == "payload3");
}

void test_osd_store_list_pg_ids() {
  mem_device_backend dev(16 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto ids0 = osd.list_pg_ids();
  EXPECT_EQ(ids0.size(), 0u);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto ids = osd.list_pg_ids();
  EXPECT_EQ(ids.size(), 2u);
}

void test_kv_engine_put_durable_and_stop_flush() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  sync_result r;
  kv.put_durable("1.0.node_cfg", std::string("members"), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto v = kv.get("1.0.node_cfg");
  EXPECT_TRUE(v.has_value() && *v == "members");

  kv.put("1.0.term", "4");
  kv.put("1.0.vote_for", "2");
  r = {};
  kv.stop(on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv2(&dev, alloc, buffer_mode::heap);
  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  kv2.rebuild_from_image(image.data(), image.size());
  auto cfg = kv2.get("1.0.node_cfg");
  auto term = kv2.get("1.0.term");
  auto vote = kv2.get("1.0.vote_for");
  EXPECT_TRUE(cfg.has_value() && *cfg == "members");
  EXPECT_TRUE(term.has_value() && *term == "4");
  EXPECT_TRUE(vote.has_value() && *vote == "2");
}

void test_kv_engine_put_visible_before_commit() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  kv.put("1.0.node_cfg", "abc");
  auto v = kv.get("1.0.node_cfg");
  EXPECT_TRUE(v.has_value());
  EXPECT_TRUE(*v == "abc");

  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  sync_result r;
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  kv_engine kv2(&dev, alloc2, buffer_mode::heap);
  kv2.rebuild_from_image(image.data(), image.size());
  auto v2 = kv2.get("1.0.node_cfg");
  EXPECT_TRUE(v2.has_value());
  EXPECT_TRUE(*v2 == "abc");
}

void test_kv_engine_put_commit_get() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  // No poller in test — commit explicitly.
  kv.put("1.0.term", "7");
  kv.put("1.0.vote_for", "3");
  sync_result r;
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto t = kv.get("1.0.term");
  EXPECT_TRUE(t.has_value());
  EXPECT_TRUE(*t == "7");
  auto v = kv.get("1.0.vote_for");
  EXPECT_TRUE(v.has_value());
  EXPECT_TRUE(*v == "3");

  // Rebuild from image
  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  kv_engine kv2(&dev, alloc2, buffer_mode::heap);
  kv2.rebuild_from_image(image.data(), image.size());
  auto t2 = kv2.get("1.0.term");
  EXPECT_TRUE(t2.has_value());
  EXPECT_TRUE(*t2 == "7");
}

void test_kv_engine_delete_and_checkpoint_replay() {
  mem_device_backend dev(8 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  kv.put("k1", "v1");
  kv.put("k2", "v2");
  sync_result r;
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);
  kv.remove("k1");
  r = {};
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(!kv.get("k1").has_value());
  EXPECT_TRUE(kv.get("k2").has_value());

  // Force a full-table checkpoint via enough ops (kCheckpointEvery=64).
  for (int i = 0; i < 70; ++i) {
    kv.put("bulk" + std::to_string(i), "x");
  }
  r = {};
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  kv_engine kv2(&dev, alloc2, buffer_mode::heap);
  kv2.rebuild_from_image(image.data(), image.size());
  EXPECT_TRUE(!kv2.get("k1").has_value());
  auto k2 = kv2.get("k2");
  EXPECT_TRUE(k2.has_value());
  EXPECT_TRUE(*k2 == "v2");
  auto bulk0 = kv2.get("bulk0");
  EXPECT_TRUE(bulk0.has_value());
}

void test_log_store_install_snapshot_base() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{5, 1, 7}, alloc, buffer_mode::heap);

  // Empty epoch log: first durable entry is the snapshot base at a high index.
  sync_result r;
  log.install_snapshot_base(132, 8, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(log.contains(132));
  EXPECT_EQ(log.base_index(), 132);
  EXPECT_EQ(log.last_index(), 132);
  EXPECT_EQ(log.get_term(132), 8);

  // Idempotent.
  r = {};
  log.install_snapshot_base(132, 8, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(log.last_index(), 132);

  // Catchup continues at index+1.
  log_entry e;
  e.term = 8;
  e.index = 133;
  e.type = 0;
  e.meta = "w";
  e.data = "x";
  r = {};
  log.append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(log.last_index(), 133);

  // Rebuild as on restart: new log_store, same image, same epoch.
  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  log_store log2(&dev, pg_id{5, 1, 7}, alloc2, buffer_mode::heap);
  log2.rebuild_from_image(image.data(), image.size());
  EXPECT_TRUE(log2.contains(132));
  EXPECT_EQ(log2.base_index(), 132);
  EXPECT_EQ(log2.last_index(), 133);
  EXPECT_EQ(log2.get_term(132), 8);
  EXPECT_EQ(log2.get_term(133), 8);
}

void test_log_store_install_snapshot_base_replaces_shorter_log() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{5, 1, 7}, alloc, buffer_mode::heap);

  log_entry old_e;
  old_e.term = 1;
  old_e.index = 10;
  old_e.type = 0;
  old_e.meta = "old";
  sync_result r;
  log.append(old_e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(log.contains(10));

  // Same index is still a no-op, not a truncate.
  r = {};
  log.install_snapshot_base(10, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(log.contains(10));
  EXPECT_EQ(log.last_index(), 10);

  r = {};
  log.install_snapshot_base(50, 4, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(!log.contains(10));
  EXPECT_TRUE(log.contains(50));
  EXPECT_EQ(log.base_index(), 50);
  EXPECT_EQ(log.last_index(), 50);
  EXPECT_EQ(log.get_term(50), 4);

  log_entry next;
  next.term = 4;
  next.index = 51;
  next.type = 0;
  next.meta = "n";
  r = {};
  log.append(next, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(log.last_index(), 51);

  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  log_store log2(&dev, pg_id{5, 1, 7}, alloc2, buffer_mode::heap);
  log2.rebuild_from_image(image.data(), image.size());
  EXPECT_TRUE(!log2.contains(10));
  EXPECT_TRUE(log2.contains(50));
  EXPECT_TRUE(log2.contains(51));
  EXPECT_EQ(log2.base_index(), 50);
  EXPECT_EQ(log2.last_index(), 51);
  EXPECT_EQ(log2.get_term(50), 4);
}

void test_log_store_truncate_from() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{3, 1, 1}, alloc, buffer_mode::heap);
  for (uint64_t i = 1; i <= 5; ++i) {
    log_entry e;
    e.term = 2;
    e.index = i;
    e.type = 0;
    e.meta = "m";
    e.data = "d" + std::to_string(i);
    sync_result r;
    log.append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  EXPECT_EQ(log.last_index(), 5);
  sync_result r;
  log.truncate_from(3, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(log.last_index(), 2);
  EXPECT_EQ(log.entry_count(), 2);
  EXPECT_EQ(log.get_term(2), 2u);
  EXPECT_EQ(log.get_term(3), 0u);
}

void test_aos_dump_empty_and_formatted() {
  // Empty / garbage image fails cleanly.
  {
    std::vector<uint8_t> junk(kMetaSize, 0);
    auto rep = scan_image(junk.data(), junk.size(), 4096);
    EXPECT_TRUE(!rep.ok());
    EXPECT_TRUE(!rep.has_chosen);
  }

  // Format + create_pg → dump sees superblock, manifest, zero data records.
  mem_device_backend dev(16 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto& storage = dev.storage();
  auto rep = scan_image(storage.data(), storage.size(), dev.block_size());
  EXPECT_TRUE(rep.has_chosen);
  EXPECT_TRUE(rep.sb_a_ok || rep.sb_b_ok);
  EXPECT_TRUE(rep.manifest_ok);
  EXPECT_TRUE(rep.man.pgs.size() >= 1u);
  EXPECT_TRUE(rep.ok());
  auto text = format_report(rep);
  EXPECT_TRUE(text.find("AOS dump") != std::string::npos);
  EXPECT_TRUE(text.find("OK") != std::string::npos);
}

void test_pg_store_flatten_image() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> a(4096, 'A');
  std::vector<char> b(4096, 'B');
  sync_result r;

  store.write("o1", 0, a.data(), a.size(), 1, /*image*/ 5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("o2", 0, a.data(), a.size(), 2, 5, on_done, &r);
  EXPECT_EQ(r.err, 0);

  r = {};
  store.create_image_snapshot(5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.image_snapshot_pin_count(5), 1u);

  // Child image overwrites at seq 10; parent snap still A.
  r = {};
  store.write("o1", 0, b.data(), b.size(), 3, 10, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Flatten parent view 5 into seq 20 (independent materialization).
  r = {};
  store.flatten_image(/*parent*/ 5, /*new*/ 20, /*raft_base*/ 100, on_done, &r);
  EXPECT_EQ(r.err, 0);

  // Head after flatten: o1 still B (head was seq 10); flatten writes under 20
  // so head should become A for extents rewritten at 20...
  // write at raft 100+ with image 20 overwrites o1 head to A.
  std::vector<char> out(4096, 0);
  r = {};
  store.read("o1", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');

  r = {};
  out.assign(4096, 0);
  store.read("o2", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');

  // Parent pin still valid; release (clone drop).
  r = {};
  store.release_image_snapshot(5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.image_snapshot_pin_count(5), 0u);
}

void test_osd_store_image_snap_and_gc_pass() {
  mem_device_backend dev(32 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> a(4096, 'X');
  r = {};
  osd.apply(1, 0, "x", 0, a.data(), a.size(), 1, /*image*/ 7, on_done, &r);
  EXPECT_EQ(r.err, 0);

  r = {};
  osd.create_image_snapshot(1, 0, 7, on_done, &r);
  EXPECT_EQ(r.err, 0);

  r = {};
  osd.clone_image(1, 0, 7, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 0);
  r = {};
  osd.read(1, 0, "x", 0, out.data(), 4096, /*raft*/ 0, /*image*/ 7, on_done,
           &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'X');

  auto st = osd.gc_pass();
  (void)st;

  r = {};
  osd.release_image_snapshot(1, 0, 7, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.release_image_snapshot(1, 0, 7, on_done, &r);
  EXPECT_EQ(r.err, 0);
}

void test_gc_preserves_live_log_and_frees_after_trim() {
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto* lg = osd.get_or_create_log(1, 0, osd.serving_epoch(1, 0));
  EXPECT_TRUE(lg != nullptr);

  int idx = 0;
  while (osd.allocator()->sealed_count() < 1 && idx < 40) {
    log_entry e;
    e.term = 1;
    e.index = static_cast<uint64_t>(idx + 1);
    e.type = 0;
    e.meta = "m";
    e.data = std::string(64, 'L');
    r = {};
    lg->append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++idx;
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 1u);
  EXPECT_TRUE(idx >= 2);

  const uint32_t sealed_before = osd.allocator()->sealed_count();
  const uint32_t freed_live = osd.gc_once(true);
  EXPECT_EQ(freed_live, 0u);
  EXPECT_EQ(osd.allocator()->sealed_count(), sealed_before);

  struct {
    int err{-1};
    std::vector<log_entry> entries;
  } rr;
  lg->read(1, 1,
           [](void* a, std::vector<log_entry> entries, int err) {
             auto* p = static_cast<decltype(rr)*>(a);
             p->err = err;
             p->entries = std::move(entries);
           },
           &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1u);

  lg->trim_to(static_cast<uint64_t>(idx - 1));
  const uint32_t freed = osd.gc_once(true);
  EXPECT_TRUE(freed >= 1u);
  EXPECT_TRUE(osd.allocator()->sealed_count() < sealed_before);

  rr = {};
  lg->read(static_cast<uint64_t>(idx), static_cast<uint64_t>(idx),
           [](void* a, std::vector<log_entry> entries, int err) {
             auto* p = static_cast<decltype(rr)*>(a);
             p->err = err;
             p->entries = std::move(entries);
           },
           &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1u);
}

void test_gc_preserves_live_kv_and_frees_after_checkpoint() {
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);

  int n = 0;
  while (osd.allocator()->sealed_count() < 1 && n < 40) {
    kv.put("k" + std::to_string(n), "v" + std::to_string(n));
    r = {};
    kv.commit(on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++n;
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 1u);

  const uint32_t sealed_before = osd.allocator()->sealed_count();
  EXPECT_EQ(osd.gc_once(true), 0u);
  EXPECT_EQ(osd.allocator()->sealed_count(), sealed_before);

  auto kept = kv.get("k0");
  EXPECT_TRUE(kept.has_value());
  EXPECT_TRUE(*kept == "v0");

  r = {};
  kv.checkpoint(on_done, &r);
  EXPECT_EQ(r.err, 0);

  const uint32_t freed = osd.gc_once(true);
  EXPECT_TRUE(freed >= 1u);
  EXPECT_TRUE(osd.allocator()->sealed_count() < sealed_before);

  auto after = kv.get("k0");
  EXPECT_TRUE(after.has_value());
  EXPECT_TRUE(*after == "v0");
}

void test_log_store_torn_tail() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{4, 1, 1}, alloc, buffer_mode::heap);
  for (uint64_t i = 1; i <= 3; ++i) {
    log_entry e;
    e.term = 1;
    e.index = i;
    e.type = 0;
    e.meta = "m";
    e.data = "p" + std::to_string(i);
    sync_result r;
    log.append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  const uint64_t good_end = dev.write_pointer();
  auto& storage = dev.storage();
  if (good_end + 4096 <= storage.size()) {
    std::memset(storage.data() + good_end, 0x5a, 4096);
    dev.set_write_pointer(good_end + 4096);
  }

  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  log_store log2(&dev, pg_id{4, 1, 1}, alloc2, buffer_mode::heap);
  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  sync_result r;
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  log2.rebuild_from_image(image.data(), image.size());
  EXPECT_EQ(log2.base_index(), 1);
  EXPECT_EQ(log2.last_index(), 3);
  EXPECT_EQ(log2.entry_count(), 3u);
}

void test_kv_engine_torn_wal_keeps_prior() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  kv.put("k1", "v1");
  sync_result r;
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);
  const uint64_t after_first = dev.write_pointer();

  kv.put("k2", "v2");
  r = {};
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto& storage = dev.storage();
  std::memset(storage.data() + after_first, 0x5a,
              static_cast<size_t>(dev.write_pointer() - after_first));

  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  kv_engine kv2(&dev, alloc2, buffer_mode::heap);
  kv2.rebuild_from_image(image.data(), image.size());
  auto v1 = kv2.get("k1");
  EXPECT_TRUE(v1.has_value());
  EXPECT_TRUE(*v1 == "v1");
  EXPECT_TRUE(!kv2.get("k2").has_value());
}

void test_kv_engine_torn_checkpoint_keeps_prior() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  kv.put("keep", "yes");
  sync_result r;
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  kv.checkpoint(on_done, &r);
  EXPECT_EQ(r.err, 0);
  const uint64_t good_end = dev.write_pointer();

  auto& storage = dev.storage();
  if (good_end + 4096 <= storage.size()) {
    std::memset(storage.data() + good_end, 0x5a, 4096);
    dev.set_write_pointer(good_end + 4096);
  }

  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  kv_engine kv2(&dev, alloc2, buffer_mode::heap);
  kv2.rebuild_from_image(image.data(), image.size());
  auto v = kv2.get("keep");
  EXPECT_TRUE(v.has_value());
  EXPECT_TRUE(*v == "yes");
}

void test_osd_store_empty_recover_then_append() {
  // After mkfs + recover on a zeroed device, sealed_count must stay 0 so
  // the first raft log append is not ENOSPC (vstart regression).
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  {
    osd_store osd(&dev, buffer_mode::heap, seg);
    sync_result r;
    osd.format(on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(osd.allocator()->sealed_count(), 0u);
  EXPECT_TRUE(osd.allocator()->free_count() >= 1u);

  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto* lg = osd.get_or_create_log(1, 0, osd.serving_epoch(1, 0));
  EXPECT_TRUE(lg != nullptr);
  log_entry e;
  e.term = 1;
  e.index = 1;
  e.type = 0;
  e.meta = "cfg";
  e.data = "x";
  r = {};
  lg->append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(lg->last_index(), 1);
}

void test_recover_rebinds_kv_allocator() {
  // aos_runtime::open() wires kv to the pre-recover allocator. recover()
  // used to replace alloc_ and leave KV reserving from a stale cursor
  // (data/KV overlap on vstart).
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024, 512);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);
  const auto* before = kv.allocator();
  EXPECT_TRUE(before == osd.allocator());

  r = {};
  osd.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(kv.allocator() == osd.allocator());
  EXPECT_TRUE(kv.allocator() != nullptr);

  uint64_t p_obj = 0;
  uint64_t p_kv = 0;
  EXPECT_EQ(osd.allocator()->reserve(4096, &p_obj, nullptr), 0);
  EXPECT_EQ(kv.allocator()->reserve(4096, &p_kv, nullptr), 0);
  EXPECT_EQ(p_kv, p_obj + 4096);
}

void test_osd_store_recover_keeps_existing_epoch_512() {
  unified_log_guard off(false);
  // Live vstart aio bdev is 512-byte blocks; recover must apply 512-aligned
  // object + kv + log records into the same serving epoch.
  mem_device_backend dev(16 * 1024 * 1024, 512);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  const uint64_t epoch = osd.serving_epoch(1, 0);
  std::vector<char> a(4096, 'Q');
  r = {};
  osd.apply(1, 0, "keep512", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto alloc = osd.shared_allocator();
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  osd.set_kv_engine(&kv);
  kv.put("1.0.node_cfg", "cfg-512");
  r = {};
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto* lg = osd.get_or_create_log(1, 0, epoch);
  EXPECT_TRUE(lg != nullptr);
  log_entry e;
  e.term = 1;
  e.index = 1;
  e.type = 0;
  e.meta = "m";
  e.data = "log-512";
  r = {};
  lg->append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, 1024 * 1024);
  kv_engine kv2(&dev, osd2.shared_allocator(), buffer_mode::heap);
  osd2.set_kv_engine(&kv2);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto* pg = osd2.find_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_EQ(pg->id().store_epoch, epoch);
  EXPECT_EQ(osd2.serving_epoch(1, 0), epoch);
  EXPECT_TRUE(osd2.bind_existing_pg(1, 0) != nullptr);
  EXPECT_EQ(osd2.serving_epoch(1, 0), epoch);
  std::vector<char> out(4096, 0);
  r = {};
  osd2.read(1, 0, "keep512", 0, out.data(), 4096, 0, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'Q');
  auto cfg = kv2.get("1.0.node_cfg");
  EXPECT_TRUE(cfg.has_value());
  EXPECT_TRUE(*cfg == "cfg-512");
  auto* lg2 = osd2.get_log(1, 0, epoch);
  EXPECT_TRUE(lg2 != nullptr);
  EXPECT_EQ(lg2->last_index(), 1);
}

void test_osd_store_recover_keeps_kv_across_pad_hole() {
  mem_device_backend dev(16 * 1024 * 1024, 512);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);
  kv.put("1.0.node_cfg", "cfg-a");
  kv.put("1.0.term", "3");
  r = {};
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);

  const uint64_t hole = dev.write_pointer();
  std::vector<uint8_t> pad(2560, 0x55);
  r = {};
  dev.write_at(hole, pad.data(), pad.size(),
               [](void* a, uint64_t, int err) {
                 static_cast<sync_result*>(a)->err = err;
               },
               &r);
  EXPECT_EQ(r.err, 0);
  if (osd.allocator() != nullptr) {
    osd.allocator()->set_open_cursor_abs(dev.write_pointer());
  }

  kv.put("1.0.lapply_idx", "9");
  r = {};
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, 1024 * 1024);
  kv_engine kv2(&dev, osd2.shared_allocator(), buffer_mode::heap);
  osd2.set_kv_engine(&kv2);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto cfg = kv2.get("1.0.node_cfg");
  auto term = kv2.get("1.0.term");
  auto lapply = kv2.get("1.0.lapply_idx");
  EXPECT_TRUE(cfg.has_value() && *cfg == "cfg-a");
  EXPECT_TRUE(term.has_value() && *term == "3");
  EXPECT_TRUE(lapply.has_value() && *lapply == "9");
}

void test_osd_store_recover_skips_nonzero_pad_hole() {
  unified_log_guard off(false);
  // Production recover used to jump to the next 64MiB segment on the first
  // bad_magic. A 0x55 pad hole after early kv records then hid all later
  // object/log records (vstart restart-verify mismatch).
  mem_device_backend dev(16 * 1024 * 1024, 512);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> before(4096, 'B');
  r = {};
  osd.apply(1, 0, "before", 0, before.data(), before.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  const uint64_t hole = dev.write_pointer();
  std::vector<uint8_t> pad(2560, 0x55);
  r = {};
  dev.write_at(hole, pad.data(), pad.size(),
               [](void* a, uint64_t, int err) {
                 static_cast<sync_result*>(a)->err = err;
               },
               &r);
  EXPECT_EQ(r.err, 0);
  if (osd.allocator() != nullptr) {
    osd.allocator()->set_open_cursor_abs(dev.write_pointer());
  }

  std::vector<char> after(4096, 'A');
  r = {};
  osd.apply(1, 0, "after", 0, after.data(), after.size(), 2, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, 1024 * 1024);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> out(4096, 0);
  r = {};
  osd2.read(1, 0, "before", 0, out.data(), 4096, 0, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'B');
  std::fill(out.begin(), out.end(), 0);
  r = {};
  osd2.read(1, 0, "after", 0, out.data(), 4096, 0, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');
}

void test_osd_store_recover_vstart_image_if_present() {
  const char* path = std::getenv("AOS_RECOVER_IMAGE");
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "AOS_RECOVER_IMAGE open failed: %s\n", path);
    ++g_failures;
    return;
  }
  const uint64_t cap = 8ull * 1024 * 1024;
  mem_device_backend dev(cap, 512);
  in.read(reinterpret_cast<char*>(dev.storage().data()),
          static_cast<std::streamsize>(cap));
  const auto got = static_cast<uint64_t>(in.gcount());
  EXPECT_TRUE(got >= kMetaSize + 4096);
  dev.set_write_pointer((got / 512) * 512);

  osd_store osd(&dev, buffer_mode::heap, 64ull * 1024 * 1024);
  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);
  sync_result r;
  osd.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto* pg = osd.find_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_TRUE(osd.bind_existing_pg(1, 0) != nullptr);
  auto cfg = kv.get("1.0.node_cfg");
  EXPECT_TRUE(cfg.has_value());
  std::printf("vstart image recover: pg=%s objects=%lu serving=%lu kv_cfg=%s\n",
              pg ? "yes" : "no",
              static_cast<unsigned long>(pg ? pg->object_count() : 0),
              osd.serving_epoch(1, 0),
              cfg.has_value() ? "yes" : "no");
}

void test_osd_store_recover_keeps_existing_epoch() {
  unified_log_guard off(false);
  mem_device_backend dev(16 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  const uint64_t epoch = osd.serving_epoch(1, 0);
  std::vector<char> a(4096, 'Z');
  r = {};
  osd.apply(1, 0, "keep", 0, a.data(), a.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, 1024 * 1024);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto* pg = osd2.find_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_EQ(pg->id().store_epoch, epoch);
  EXPECT_EQ(osd2.serving_epoch(1, 0), epoch);
  std::vector<char> out(4096, 0);
  r = {};
  osd2.read(1, 0, "keep", 0, out.data(), 4096, 0, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'Z');
}

void test_osd_store_recover_skips_zero_hole() {
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto* lg = osd.get_or_create_log(1, 0, osd.serving_epoch(1, 0));
  EXPECT_TRUE(lg != nullptr);

  int n = 0;
  while (osd.allocator()->sealed_count() < 2 && n < 80) {
    log_entry e;
    e.term = 1;
    e.index = static_cast<uint64_t>(n + 1);
    e.type = 0;
    e.meta = "m";
    e.data = std::string(64, 'H');
    r = {};
    lg->append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++n;
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 2u);
  EXPECT_TRUE(n >= 4);
  const uint64_t last_idx = static_cast<uint64_t>(n);

  // Wipe the first data segment (simulate a GC hole / unused range).
  const uint64_t hole = osd.data_offset();
  auto& storage = dev.storage();
  EXPECT_TRUE(hole + seg <= storage.size());
  std::memset(storage.data() + hole, 0, static_cast<size_t>(seg));

  osd_store osd2(&dev, buffer_mode::heap, seg);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  // Hole must not seal the rest of the device.
  EXPECT_TRUE(osd2.allocator()->free_count() >= 1u);

  auto* lg2 = osd2.get_log(1, 0, osd2.serving_epoch(1, 0));
  EXPECT_TRUE(lg2 != nullptr);
  EXPECT_EQ(lg2->last_index(), last_idx);

  struct {
    int err{-1};
    std::vector<log_entry> entries;
  } rr;
  lg2->read(last_idx, last_idx,
            [](void* a, std::vector<log_entry> entries, int err) {
              auto* p = static_cast<decltype(rr)*>(a);
              p->err = err;
              p->entries = std::move(entries);
            },
            &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1u);
}

void test_aos_dump_counts_kv_and_log_records() {
  mem_device_backend dev(16 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(2, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto alloc = osd.shared_allocator();
  kv_engine kv(&dev, alloc, buffer_mode::heap);
  kv.put("term", "1");
  r = {};
  kv.commit(on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto* lg = osd.get_or_create_log(2, 0, osd.serving_epoch(2, 0));
  EXPECT_TRUE(lg != nullptr);
  log_entry e;
  e.term = 1;
  e.index = 1;
  e.type = 0;
  e.meta = "m";
  e.data = "payload";
  r = {};
  lg->append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto rep = scan_image(dev.storage().data(), dev.storage().size(),
                        dev.block_size());
  EXPECT_TRUE(rep.has_chosen);
  EXPECT_TRUE(rep.record_count >= 2u);
  EXPECT_TRUE(rep.records_by_type.count(
                  static_cast<uint8_t>(record_type::kv_put)) > 0);
  EXPECT_TRUE(rep.records_by_type.count(
                  static_cast<uint8_t>(record_type::raft_log_entry)) > 0);
}

void test_aos_dump_collect_live_lww() {
  mem_device_backend dev(16 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 1024 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(3, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> first(4096, 'A');
  std::vector<char> second(4096, 'B');
  r = {};
  osd.apply(3, 1, "obj", 0, first.data(), first.size(), /*raft*/ 10, 0,
            on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.apply(3, 1, "obj", 0, second.data(), second.size(), /*raft*/ 11, 0,
            on_done, &r);
  EXPECT_EQ(r.err, 0);

  auto rep = scan_image(dev.storage().data(), dev.storage().size(),
                        dev.block_size(), /*collect_live=*/true);
  EXPECT_TRUE(rep.ok());
  EXPECT_TRUE(rep.live_records.size() >= 2u);

  uint64_t data_recs = 0;
  uint64_t last_apply = 0;
  char last_byte = 0;
  for (const auto& rec : rep.live_records) {
    if (rec.type != record_type::data) {
      continue;
    }
    ++data_recs;
    if (rec.raft_apply_index >= last_apply && !rec.payload.empty()) {
      last_apply = rec.raft_apply_index;
      last_byte = rec.payload[0];
    }
  }
  EXPECT_TRUE(data_recs >= 2u);
  EXPECT_EQ(last_apply, 11u);
  EXPECT_EQ(last_byte, 'B');
}

void test_log_store_checkpoint_rebuild() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 1, 1}, alloc, buffer_mode::heap);
  for (uint64_t i = 1; i <= 4; ++i) {
    log_entry e;
    e.term = 2;
    e.index = i;
    e.type = 0;
    e.meta = "m";
    e.data = "d";
    sync_result r;
    log.append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  sync_result r;
  log.checkpoint(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(log.last_checkpoint_phys() != 0);

  log_entry e;
  e.term = 2;
  e.index = 5;
  e.type = 0;
  r = {};
  log.append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  log_store log2(&dev, pg_id{1, 1, 1}, alloc2, buffer_mode::heap);
  // Apply only the checkpoint + tail entry 5 via rebuild (full image).
  log2.rebuild_from_image(image.data(), image.size());
  EXPECT_EQ(log2.last_index(), 5);
  EXPECT_EQ(log2.get_term(3), 2);
  EXPECT_EQ(log2.get_term(5), 2);
}

void test_osd_checkpoint_pass_then_recover() {
  unified_log_guard off(false);
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  const char payload[4096] = {0};
  r = {};
  osd.apply(1, 0, "obj-a", 0, payload, 4096, /*raft*/ 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.checkpoint_pass(on_done, &r);
  EXPECT_EQ(r.err, 0);

  char second[4096];
  std::memset(second, 'Z', sizeof(second));
  r = {};
  osd.apply(1, 0, "obj-a", 4096, second, 4096, /*raft*/ 2, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, seg);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  char buf[4096];
  r = {};
  osd2.read(1, 0, "obj-a", 0, buf, 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd2.read(1, 0, "obj-a", 4096, buf, 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(buf[0], 'Z');
}

struct raw_rec {
  uint64_t phys{0};
  uint64_t aligned{0};
  int err{-1};
};

raw_rec write_raw_record(device_backend* dev, segment_allocator* alloc,
                         const data_record& rec) {
  segment writer(dev, alloc);
  auto encoded = encode_record(rec);
  raw_rec out;
  out.aligned = writer.align_up(encoded.size());
  std::vector<uint8_t> dma(static_cast<size_t>(out.aligned), 0);
  std::memcpy(dma.data(), encoded.data(), encoded.size());
  writer.append_record(
      rec, dma.data(), out.aligned,
      [](void* a, uint64_t phys, int err) {
        auto* p = static_cast<raw_rec*>(a);
        p->phys = phys;
        p->err = err;
      },
      &out);
  return out;
}

struct log_rr {
  int err{-1};
  std::vector<log_entry> entries;
};

log_rr read_log(log_store& log, uint64_t start, uint64_t end) {
  log_rr rr;
  log.read(start, end,
           [](void* a, std::vector<log_entry> entries, int err) {
             auto* p = static_cast<log_rr*>(a);
             p->err = err;
             p->entries = std::move(entries);
           },
           &rr);
  return rr;
}

bool proto_bytes_field(const std::string& s, uint32_t field, std::string* out) {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t tag = static_cast<uint8_t>(s[i++]);
    const uint32_t f = tag >> 3;
    const uint32_t wt = tag & 7;
    if (wt == 2) {
      uint64_t len = 0;
      int shift = 0;
      while (i < s.size()) {
        const uint8_t b = static_cast<uint8_t>(s[i++]);
        len |= static_cast<uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
          break;
        }
        shift += 7;
      }
      if (i + static_cast<size_t>(len) > s.size()) {
        return false;
      }
      if (f == field) {
        if (out != nullptr) {
          out->assign(s.data() + i, static_cast<size_t>(len));
        }
        return true;
      }
      i += static_cast<size_t>(len);
    } else if (wt == 0) {
      while (i < s.size() && (static_cast<uint8_t>(s[i++]) & 0x80) != 0) {
      }
    } else {
      return false;
    }
  }
  return false;
}

bool proto_u64_field(const std::string& s, uint32_t field, uint64_t* out) {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t tag = static_cast<uint8_t>(s[i++]);
    const uint32_t f = tag >> 3;
    const uint32_t wt = tag & 7;
    if (wt == 0) {
      uint64_t v = 0;
      int shift = 0;
      while (i < s.size()) {
        const uint8_t b = static_cast<uint8_t>(s[i++]);
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
          break;
        }
        shift += 7;
      }
      if (f == field) {
        if (out != nullptr) {
          *out = v;
        }
        return true;
      }
    } else if (wt == 2) {
      uint64_t len = 0;
      int shift = 0;
      while (i < s.size()) {
        const uint8_t b = static_cast<uint8_t>(s[i++]);
        len |= static_cast<uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
          break;
        }
        shift += 7;
      }
      if (i + static_cast<size_t>(len) > s.size()) {
        return false;
      }
      i += static_cast<size_t>(len);
    } else {
      return false;
    }
  }
  return false;
}

void test_log_store_append_type14_still_readable() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);
  log_entry e;
  e.term = 7;
  e.index = 1;
  e.type = 0;
  e.meta = "type14-meta";
  e.data = "type14-data";
  sync_result r;
  log.append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_EQ(rr.entries[0].term, 7);
  EXPECT_EQ(rr.entries[0].index, 1);
  EXPECT_TRUE(rr.entries[0].meta == "type14-meta");
  EXPECT_TRUE(rr.entries[0].data == "type14-data");
}

void test_log_store_register_data_as_log() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);
  log.set_name_lookup([](uint64_t oid, std::string* name) {
    if (oid != 42 || name == nullptr) {
      return -ENOENT;
    }
    *name = "img.0001";
    return 0;
  });

  data_record rec;
  rec.type = record_type::data;
  rec.pool_id = 1;
  rec.pg_id = 2;
  rec.store_epoch = 1;
  rec.object_id = 42;
  rec.object_offset = 4096;
  rec.raft_apply_index = 1;
  rec.image_snap_seq = 9;
  rec.payload.assign(4096, 'W');
  auto wr = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr.err, 0);

  EXPECT_EQ(log.register_index(0, 3, wr.phys, wr.aligned, record_type::data),
            -EINVAL);
  EXPECT_EQ(log.register_index(1, 3, wr.phys, wr.aligned, record_type::data),
            0);
  EXPECT_EQ(log.register_index(1, 3, wr.phys, wr.aligned, record_type::data),
            0);
  EXPECT_EQ(log.register_index(1, 3, wr.phys + 4096, wr.aligned,
                               record_type::data),
            -EEXIST);
  EXPECT_EQ(log.register_index(3, 3, wr.phys, wr.aligned, record_type::data),
            -EINVAL);
  EXPECT_EQ(log.last_index(), 1);
  EXPECT_EQ(log.get_term(1), 3);

  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_EQ(rr.entries[0].term, 3);
  EXPECT_EQ(rr.entries[0].index, 1);
  EXPECT_EQ(rr.entries[0].type, 0); // RAFT_LOGTYPE_WRITE
  EXPECT_EQ(rr.entries[0].data.size(), 4096);
  EXPECT_TRUE(rr.entries[0].data == rec.payload);
  EXPECT_TRUE(!rr.entries[0].meta.empty());
  std::string name;
  EXPECT_TRUE(proto_bytes_field(rr.entries[0].meta, 1, &name));
  EXPECT_TRUE(name == "img.0001");
  uint64_t off = 0;
  uint64_t img = 0;
  EXPECT_TRUE(proto_u64_field(rr.entries[0].meta, 2, &off));
  EXPECT_EQ(off, 4096);
  EXPECT_TRUE(proto_u64_field(rr.entries[0].meta, 3, &img));
  EXPECT_EQ(img, 9);
}

void test_log_store_read_requires_name_lookup() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);

  data_record rec;
  rec.type = record_type::data;
  rec.object_id = 7;
  rec.object_offset = 0;
  rec.payload.assign(4096, 'X');
  auto wr = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr.err, 0);
  EXPECT_EQ(log.register_index(1, 1, wr.phys, wr.aligned, record_type::data),
            0);

  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, -ENOENT);
  EXPECT_EQ(rr.entries.size(), 0);

  log.set_name_lookup([](uint64_t, std::string*) { return -ENOENT; });
  rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, -ENOENT);
  EXPECT_EQ(rr.entries.size(), 0);

  log.set_name_lookup([](uint64_t, std::string* name) {
    if (name != nullptr) {
      name->clear();
    }
    return 0;
  });
  rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, -ENOENT);
  EXPECT_EQ(rr.entries.size(), 0);

  rec.payload.assign(128, 'y');
  auto wr2 = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr2.err, 0);
  EXPECT_EQ(
      log.register_index(2, 1, wr2.phys, wr2.aligned, record_type::data), 0);
  log.set_name_lookup([](uint64_t, std::string* name) {
    *name = "n";
    return 0;
  });
  rr = read_log(log, 2, 2);
  EXPECT_EQ(rr.err, -EIO);
  EXPECT_EQ(rr.entries.size(), 0);
}

void test_log_store_old_checkpoint_default_raft() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 1, 1}, alloc, buffer_mode::heap);

  log_entry e;
  e.term = 4;
  e.index = 1;
  e.type = 0;
  e.meta = "old-ckpt";
  e.data = "payload";
  data_record rec;
  rec.type = record_type::raft_log_entry;
  rec.pool_id = 1;
  rec.pg_id = 1;
  rec.store_epoch = 1;
  rec.raft_apply_index = 1;
  rec.image_snap_seq = 4;
  rec.payload = log_store::encode_payload(e);
  auto wr = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr.err, 0);

  // 32B-per-entry payload: no rec_type field.
  std::string old;
  auto put64 = [&](uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      old.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
  };
  auto put32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      old.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
  };
  put64(1);
  put64(1);
  put32(1);
  put64(1);
  put64(4);
  put64(wr.phys);
  put64(wr.aligned);
  EXPECT_EQ(old.size(), 52u);

  data_record ckpt;
  ckpt.type = record_type::log_checkpoint;
  ckpt.pool_id = 1;
  ckpt.pg_id = 1;
  ckpt.store_epoch = 1;
  ckpt.payload = old;
  EXPECT_EQ(log.apply_checkpoint_record(ckpt), 0);
  EXPECT_EQ(log.last_index(), 1);
  EXPECT_EQ(log.get_term(1), 4);

  // Unset name_lookup: raft_log_entry still reads; data-as-log would fail.
  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_TRUE(rr.entries[0].meta == "old-ckpt");
  EXPECT_TRUE(rr.entries[0].data == "payload");
}

void test_log_store_checkpoint_preserves_data_rec_type() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);

  data_record rec;
  rec.type = record_type::data;
  rec.pool_id = 1;
  rec.pg_id = 2;
  rec.store_epoch = 1;
  rec.object_id = 11;
  rec.object_offset = 4096;
  rec.image_snap_seq = 6;
  rec.payload.assign(4096, 'C');
  auto wr = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr.err, 0);
  EXPECT_EQ(log.register_index(1, 8, wr.phys, wr.aligned, record_type::data),
            0);
  sync_result r;
  log.checkpoint(on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<uint8_t> image(static_cast<size_t>(dev.write_pointer()));
  r = {};
  dev.read(0, image.data(), image.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  auto alloc2 = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                    dev.block_size());
  log_store log2(&dev, pg_id{1, 2, 1}, alloc2, buffer_mode::heap);
  log2.rebuild_from_image(image.data(), image.size());
  EXPECT_EQ(log2.last_index(), 1);
  EXPECT_EQ(log2.get_term(1), 8);
  log2.set_name_lookup([](uint64_t oid, std::string* name) {
    if (oid != 11 || name == nullptr) {
      return -ENOENT;
    }
    *name = "ckpt-obj";
    return 0;
  });
  auto rr = read_log(log2, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_EQ(rr.entries[0].type, 0); // RAFT_LOGTYPE_WRITE
  EXPECT_EQ(rr.entries[0].data.size(), 4096);
  EXPECT_TRUE(rr.entries[0].data == rec.payload);
  std::string name;
  EXPECT_TRUE(proto_bytes_field(rr.entries[0].meta, 1, &name));
  EXPECT_TRUE(name == "ckpt-obj");
  uint64_t off = 0;
  EXPECT_TRUE(proto_u64_field(rr.entries[0].meta, 2, &off));
  EXPECT_EQ(off, 4096);
}

void test_log_store_range_tombstone_read_notsup() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);
  log.set_name_lookup([](uint64_t, std::string* name) {
    *name = "discard-obj";
    return 0;
  });

  data_record rec;
  rec.type = record_type::range_tombstone;
  rec.object_id = 1;
  rec.object_offset = 0;
  rec.payload.assign(8, '\0');
  auto wr = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr.err, 0);
  EXPECT_EQ(log.register_index(1, 1, wr.phys, wr.aligned,
                               record_type::range_tombstone),
            0);
  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, -ENOTSUP);
  EXPECT_EQ(rr.entries.size(), 0);
}

void test_log_store_gc_rewrite_as_data() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);
  log.set_name_lookup([](uint64_t, std::string* name) {
    *name = "gc-obj";
    return 0;
  });

  data_record rec;
  rec.type = record_type::gc_rewrite;
  rec.object_id = 3;
  rec.object_offset = 8192;
  rec.image_snap_seq = 2;
  rec.payload.assign(4096, 'G');
  auto wr = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr.err, 0);
  EXPECT_EQ(log.register_index(1, 5, wr.phys, wr.aligned,
                               record_type::gc_rewrite),
            0);
  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_EQ(rr.entries[0].type, 0);
  EXPECT_EQ(rr.entries[0].data.size(), 4096);
  EXPECT_TRUE(rr.entries[0].data == rec.payload);
  std::string name;
  EXPECT_TRUE(proto_bytes_field(rr.entries[0].meta, 1, &name));
  EXPECT_TRUE(name == "gc-obj");
  uint64_t off = 0;
  EXPECT_TRUE(proto_u64_field(rr.entries[0].meta, 2, &off));
  EXPECT_EQ(off, 8192);
}

void test_log_store_update_phys() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);
  log.set_name_lookup([](uint64_t, std::string* name) {
    *name = "u";
    return 0;
  });

  data_record a;
  a.type = record_type::data;
  a.object_id = 1;
  a.payload.assign(4096, 'A');
  data_record b = a;
  b.type = record_type::gc_rewrite;
  b.payload.assign(4096, 'B');
  auto wa = write_raw_record(&dev, alloc.get(), a);
  auto wb = write_raw_record(&dev, alloc.get(), b);
  EXPECT_EQ(wa.err, 0);
  EXPECT_EQ(wb.err, 0);
  EXPECT_EQ(log.register_index(1, 1, wa.phys, wa.aligned, record_type::data),
            0);
  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries[0].data[0], 'A');

  log.update_phys(1, wb.phys, wb.aligned);
  rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_EQ(rr.entries[0].data[0], 'B');

  log.update_phys(99, wb.phys, wb.aligned);
  EXPECT_TRUE(!log.contains(99));
  EXPECT_EQ(log.last_index(), 1);
}

void test_log_store_delete_tombstone_as_log() {
  mem_device_backend dev(4 * 1024 * 1024);
  auto alloc = std::make_shared<segment_allocator>(0, dev.capacity(), 0,
                                                   dev.block_size());
  EXPECT_EQ(alloc->ensure_open(), 0);
  log_store log(&dev, pg_id{1, 2, 1}, alloc, buffer_mode::heap);
  log.set_name_lookup([](uint64_t, std::string* name) {
    *name = "gone";
    return 0;
  });
  data_record rec;
  rec.type = record_type::delete_tombstone;
  rec.object_id = 8;
  rec.image_snap_seq = 4;
  auto wr = write_raw_record(&dev, alloc.get(), rec);
  EXPECT_EQ(wr.err, 0);
  EXPECT_EQ(log.register_index(1, 2, wr.phys, wr.aligned,
                               record_type::delete_tombstone),
            0);
  auto rr = read_log(log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_EQ(rr.entries[0].type, 1); // RAFT_LOGTYPE_DELETE
  EXPECT_EQ(rr.entries[0].data.size(), 0);
  std::string name;
  EXPECT_TRUE(proto_bytes_field(rr.entries[0].meta, 1, &name));
  EXPECT_TRUE(name == "gone");
  uint64_t img = 0;
  EXPECT_TRUE(proto_u64_field(rr.entries[0].meta, 2, &img));
  EXPECT_EQ(img, 4);
}

void test_osd_store_injects_name_lookup() {
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> payload(4096, 'A');
  r = {};
  osd.apply(1, 0, "wired-obj", 0, payload.data(), 4096, /*raft*/ 1, 0, on_done,
            &r);
  EXPECT_EQ(r.err, 0);

  pg_store* pg = osd.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  std::string n;
  EXPECT_EQ(pg->name_of(1, &n), 0);
  EXPECT_TRUE(n == "wired-obj");
  EXPECT_EQ(pg->name_of(99, &n), -ENOENT);

  log_store* log = osd.get_active_log(1, 0);
  EXPECT_TRUE(log != nullptr);
  data_record rec;
  rec.type = record_type::data;
  rec.pool_id = 1;
  rec.pg_id = 0;
  rec.store_epoch = pg->id().store_epoch;
  rec.object_id = 1;
  rec.object_offset = 0;
  rec.payload.assign(4096, 'Z');
  auto wr = write_raw_record(osd.backend(), osd.allocator(), rec);
  EXPECT_EQ(wr.err, 0);
  EXPECT_EQ(log->register_index(1, 1, wr.phys, wr.aligned, record_type::data),
            0);
  auto rr = read_log(*log, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_TRUE(!rr.entries[0].meta.empty());
  std::string name;
  EXPECT_TRUE(proto_bytes_field(rr.entries[0].meta, 1, &name));
  EXPECT_TRUE(name == "wired-obj");
  EXPECT_EQ(rr.entries[0].data.size(), 4096);
  EXPECT_EQ(rr.entries[0].data[0], 'Z');
}

void test_format_zeros_leftover_then_recover_empty() {
  mem_device_backend dev(2 * 1024 * 1024);
  // Dirty the data region with 0x55.
  std::vector<uint8_t> junk(4096, 0x55);
  sync_result r;
  dev.write_at(kMetaSize, junk.data(), junk.size(),
               [](void* a, uint64_t, int err) {
                 static_cast<sync_result*>(a)->err = err;
               },
               &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  r = {};
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, 256 * 1024);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(osd2.pg_count(), 0u);
}

bool find_typed_record(const mem_device_backend& dev, record_type ty,
                       uint64_t raft_index, uint64_t* start, uint64_t* aligned,
                       uint16_t* hlen) {
  if (start == nullptr || aligned == nullptr || hlen == nullptr) {
    return false;
  }
  const auto& s = dev.storage();
  const uint64_t wp = dev.write_pointer();
  const uint32_t bs = dev.block_size();
  uint64_t pos = 0;
  while (pos + kRecordHeaderLenV1 <= wp) {
    data_record rec;
    size_t consumed = 0;
    const decode_status st =
        decode_record(s.data() + static_cast<size_t>(pos),
                      static_cast<size_t>(wp - pos), &rec, &consumed);
    if (st != decode_status::ok || consumed == 0) {
      if (bs == 0) {
        break;
      }
      pos += bs;
      continue;
    }
    const uint64_t al = ((consumed + bs - 1) / bs) * bs;
    if (rec.type == ty && rec.raft_apply_index == raft_index) {
      *start = pos;
      *aligned = al == 0 ? bs : al;
      *hlen = rec.header_len != 0 ? rec.header_len : kRecordHeaderLenV1;
      return true;
    }
    pos += al == 0 ? bs : al;
  }
  return false;
}

bool find_first_data_record(const mem_device_backend& dev, uint64_t* start,
                            uint16_t* hlen) {
  if (start == nullptr || hlen == nullptr) {
    return false;
  }
  const auto& s = dev.storage();
  const uint64_t wp = dev.write_pointer();
  const uint32_t bs = dev.block_size();
  uint64_t pos = 0;
  while (pos + kRecordHeaderLenV1 <= wp) {
    data_record rec;
    size_t consumed = 0;
    const decode_status st =
        decode_record(s.data() + static_cast<size_t>(pos),
                      static_cast<size_t>(wp - pos), &rec, &consumed);
    if (st != decode_status::ok || consumed == 0) {
      break;
    }
    if (rec.type == record_type::data) {
      *start = pos;
      *hlen = static_cast<uint16_t>(s[static_cast<size_t>(pos) + 8] |
                                    (s[static_cast<size_t>(pos) + 9] << 8));
      return true;
    }
    pos += ((consumed + bs - 1) / bs) * bs;
  }
  return false;
}

void test_pg_store_append_uncommitted_not_readable() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> data(4096, 'U');
  sync_result r;
  store.append_uncommitted("pend", 0, data.data(), data.size(), /*raft*/ 7,
                           /*term*/ 1, /*image*/ 0, on_done, &r);
  EXPECT_TRUE(r.done);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(store.object_exists("pend"));

  std::vector<char> out(4096, 1);
  r = {};
  store.read("pend", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));

  // Unpublished payload must stay live for GC.
  uint64_t live = 0;
  store.for_each_live_extent([&](uint64_t phys, uint64_t len) {
    if (len == 4096) {
      live = phys;
    }
  });
  EXPECT_TRUE(live != 0);
}

void test_pg_store_publish_commit_readable() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> data(4096, 'P');
  sync_result r;
  store.append_uncommitted("pub", 0, data.data(), data.size(), 11, 2, 0,
                           on_done, &r);
  EXPECT_EQ(r.err, 0);

  r = {};
  store.publish_commit(11, on_done, &r);
  EXPECT_TRUE(r.done);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 0);
  r = {};
  store.read("pub", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == data);

  // Missing pending index is a no-op success.
  r = {};
  store.publish_commit(99, on_done, &r);
  EXPECT_EQ(r.err, 0);
}

void test_pg_store_drop_pending_isolated_name() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> data(4096, 'G');
  sync_result r;
  store.append_uncommitted("ghost", 0, data.data(), data.size(), 3, 1, 0,
                           on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(store.object_exists("ghost"));
  const uint64_t nid = store.next_object_id();
  const uint64_t nobj = store.object_count();
  EXPECT_TRUE(nid > 1);
  EXPECT_TRUE(nobj >= 1);

  store.drop_pending_from(3);
  EXPECT_TRUE(!store.object_exists("ghost"));
  EXPECT_EQ(store.next_object_id(), nid);
  EXPECT_EQ(store.object_count(), nobj - 1);

  // Erase (not mark-deleted): same name must allocate a fresh id.
  r = {};
  store.append_uncommitted("ghost", 0, data.data(), data.size(), 4, 1, 0,
                           on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(store.next_object_id(), nid + 1);
  EXPECT_TRUE(store.object_exists("ghost"));
  store.drop_pending_from(4);
  EXPECT_TRUE(!store.object_exists("ghost"));

  // Remaining pending ref on the same name keeps the create.
  r = {};
  store.append_uncommitted("pair", 0, data.data(), data.size(), 10, 1, 0,
                           on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.append_uncommitted("pair", 4096, data.data(), data.size(), 11, 1, 0,
                           on_done, &r);
  EXPECT_EQ(r.err, 0);
  const uint64_t npair = store.object_count();
  store.drop_pending_from(11);
  EXPECT_TRUE(store.object_exists("pair"));
  EXPECT_EQ(store.object_count(), npair);
  store.drop_pending_from(10);
  EXPECT_TRUE(!store.object_exists("pair"));
  EXPECT_EQ(store.object_count(), npair - 1);

  // Committed names are not dropped with later unpublished suffix.
  r = {};
  store.write("keep", 0, data.data(), data.size(), 1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.append_uncommitted("keep", 4096, data.data(), data.size(), 20, 1, 0,
                           on_done, &r);
  EXPECT_EQ(r.err, 0);
  store.drop_pending_from(20);
  EXPECT_TRUE(store.object_exists("keep"));
}

void test_pg_store_drop_keeps_discarded_committed() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_id id{1, 0, 1};
  std::vector<char> data(4096, 'D');
  {
    pg_store store(&dev, id);
    sync_result r;
    store.write("hole", 0, data.data(), data.size(), 1, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.discard("hole", 0, 4096, 2, 0, on_done, &r);
    EXPECT_EQ(r.err, 0);
    r = {};
    store.checkpoint(on_done, &r);
    EXPECT_EQ(r.err, 0);
  }

  pg_store store2(&dev, id);
  sync_result rr;
  store2.recover(on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  EXPECT_TRUE(store2.object_exists("hole"));
  const uint64_t nobj = store2.object_count();
  const uint64_t nid = store2.next_object_id();

  rr = {};
  store2.append_uncommitted("hole", 0, data.data(), data.size(), 30, 1, 0,
                            on_done, &rr);
  EXPECT_EQ(rr.err, 0);
  store2.drop_pending_from(30);
  EXPECT_TRUE(store2.object_exists("hole"));
  EXPECT_EQ(store2.object_count(), nobj);
  EXPECT_EQ(store2.next_object_id(), nid);
}

class defer_flush_backend final : public device_backend {
public:
  explicit defer_flush_backend(uint64_t cap) : inner_(cap) {}

  uint32_t block_size() const override { return inner_.block_size(); }
  uint64_t write_pointer() const override { return inner_.write_pointer(); }
  void set_write_pointer(uint64_t bytes) override {
    inner_.set_write_pointer(bytes);
  }
  uint64_t capacity() const override { return inner_.capacity(); }

  void append(void* data, uint64_t nbytes, append_complete cb,
              void* arg) override {
    inner_.append(data, nbytes, std::move(cb), arg);
  }
  void write_at(uint64_t phys_offset, void* data, uint64_t nbytes,
                append_complete cb, void* arg) override {
    inner_.write_at(phys_offset, data, nbytes, std::move(cb), arg);
  }
  void read(uint64_t phys_offset, void* data, uint64_t nbytes, op_complete cb,
            void* arg) override {
    inner_.read(phys_offset, data, nbytes, std::move(cb), arg);
  }
  void flush(op_complete cb, void* arg) override {
    flush_cb_ = std::move(cb);
    flush_arg_ = arg;
    has_flush_ = true;
  }

  bool has_deferred_flush() const { return has_flush_; }
  void fire_flush(int err = 0) {
    if (!has_flush_) {
      return;
    }
    has_flush_ = false;
    auto cb = std::move(flush_cb_);
    void* a = flush_arg_;
    flush_arg_ = nullptr;
    if (cb) {
      cb(a, err);
    }
  }

private:
  mem_device_backend inner_;
  op_complete flush_cb_;
  void* flush_arg_{nullptr};
  bool has_flush_{false};
};

void test_pg_store_drop_inflight_uncommitted() {
  defer_flush_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> data(4096, 'R');
  sync_result r;
  store.append_uncommitted("race", 0, data.data(), data.size(), 7, 1, 0,
                           on_done, &r);
  EXPECT_TRUE(!r.done);
  EXPECT_TRUE(store.busy());
  EXPECT_TRUE(dev.has_deferred_flush());
  EXPECT_TRUE(store.object_exists("race"));
  const uint64_t nid = store.next_object_id();

  store.drop_pending_from(7);
  EXPECT_TRUE(!store.object_exists("race"));
  EXPECT_EQ(store.object_count(), 0u);
  EXPECT_EQ(store.next_object_id(), nid);

  dev.fire_flush();
  EXPECT_TRUE(r.done);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(!store.object_exists("race"));
  EXPECT_EQ(store.object_count(), 0u);

  uint64_t live = 0;
  store.for_each_live_extent([&](uint64_t /*phys*/, uint64_t len) {
    if (len == 4096) {
      ++live;
    }
  });
  EXPECT_EQ(live, 0u);

  // Same index after truncate is a new generation and may publish.
  r = {};
  store.append_uncommitted("reused", 0, data.data(), data.size(), 7, 2, 0,
                           on_done, &r);
  EXPECT_TRUE(dev.has_deferred_flush());
  dev.fire_flush();
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(store.object_exists("reused"));
  r = {};
  store.publish_commit(7, on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> out(4096, 0);
  r = {};
  store.read("reused", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == data);
}

void test_pg_store_write_still_immediately_readable() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> data(4096, 'W');
  sync_result r;
  store.write("hot", 0, data.data(), data.size(), 4, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> out(4096, 0);
  r = {};
  store.read("hot", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == data);
}

void test_pg_store_write_phys_uses_header_len() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> data(4096, 'H');
  sync_result r;
  store.write("hlen", 0, data.data(), data.size(), 5, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  uint64_t rec_start = 0;
  uint16_t hlen = 0;
  EXPECT_TRUE(find_first_data_record(dev, &rec_start, &hlen));
  EXPECT_TRUE(hlen != 0);

  uint64_t live_phys = 0;
  store.for_each_live_extent([&](uint64_t phys, uint64_t len) {
    if (len == 4096) {
      live_phys = phys;
    }
  });
  EXPECT_EQ(live_phys, rec_start + hlen);
  EXPECT_EQ(live_phys, payload_phys(rec_start, hlen));
}

void persist_lapply(kv_engine* kv, uint64_t pool, uint64_t pg, uint64_t lapply) {
  kv->put(std::to_string(pool) + "." + std::to_string(pg) + ".lapply_idx",
          std::to_string(lapply));
  sync_result r;
  kv->commit(on_done, &r);
  EXPECT_EQ(r.err, 0);
}

// P6 single data record: append (no head.put) + register_index.
void write_p6_data(osd_store& osd, mem_device_backend& dev, uint64_t pool,
                   uint64_t pg, const std::string& name, uint64_t index,
                   uint64_t term, const std::vector<char>& data, uint64_t* phys,
                   uint64_t* aligned, uint16_t* hlen) {
  pg_store* store = osd.get_active_pg(pool, pg);
  EXPECT_TRUE(store != nullptr);
  sync_result r;
  store->append_uncommitted(name, 0, data.data(), data.size(), index, term, 0,
                            on_done, &r);
  EXPECT_EQ(r.err, 0);
  uint64_t start = 0;
  uint64_t al = 0;
  uint16_t hl = 0;
  EXPECT_TRUE(find_typed_record(dev, record_type::data, index, &start, &al,
                                &hl));
  log_store* lg = osd.get_or_create_log(pool, pg, osd.serving_epoch(pool, pg));
  EXPECT_TRUE(lg != nullptr);
  EXPECT_EQ(lg->register_index(index, term, start, al, record_type::data), 0);
  if (phys != nullptr) {
    *phys = start;
  }
  if (aligned != nullptr) {
    *aligned = al;
  }
  if (hlen != nullptr) {
    *hlen = hl;
  }
}

void test_recover_p6_unpublished_no_ckpt() {
  unified_log_guard on(true);
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);

  std::vector<char> data(4096, 'N');
  write_p6_data(osd, dev, 1, 0, "nockpt", /*index*/ 5, /*term*/ 1, data, nullptr,
                nullptr, nullptr);
  persist_lapply(&kv, 1, 0, /*lapply*/ 4);

  osd_store osd2(&dev, buffer_mode::heap, seg);
  kv_engine kv2(&dev, osd2.shared_allocator(), buffer_mode::heap);
  osd2.set_kv_engine(&kv2);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* pg = osd2.bind_existing_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  if (pg == nullptr) {
    return;
  }
  EXPECT_TRUE(pg->pending_count() >= 1u);
  EXPECT_TRUE(pg->has_pending(5));
  log_store* lg = osd2.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  EXPECT_TRUE(lg->contains(5));
  EXPECT_EQ(static_cast<unsigned>(lg->rec_type_at(5)),
            static_cast<unsigned>(record_type::data));

  std::vector<char> out(4096, 1);
  r = {};
  osd2.read(1, 0, "nockpt", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));
}

void test_recover_p6_unpublished_pending_after_ckpt() {
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);

  std::vector<char> data(4096, 'P');
  uint64_t phys = 0;
  uint64_t al = 0;
  uint16_t hl = 0;
  write_p6_data(osd, dev, 1, 0, "pend", /*index*/ 5, /*term*/ 1, data, &phys,
                &al, &hl);
  persist_lapply(&kv, 1, 0, /*lapply*/ 4);
  r = {};
  osd.checkpoint_pass(on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, seg);
  kv_engine kv2(&dev, osd2.shared_allocator(), buffer_mode::heap);
  osd2.set_kv_engine(&kv2);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* pg = osd2.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_TRUE(pg->pending_count() >= 1u);
  EXPECT_TRUE(pg->has_pending(5));
  log_store* lg = osd2.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  EXPECT_TRUE(lg->contains(5));
  EXPECT_EQ(static_cast<unsigned>(lg->rec_type_at(5)),
            static_cast<unsigned>(record_type::data));

  std::vector<char> out(4096, 1);
  r = {};
  osd2.read(1, 0, "pend", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));

  r = {};
  pg->publish_commit(5, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(pg->pending_count(), 0u);
  out.assign(4096, 0);
  r = {};
  osd2.read(1, 0, "pend", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == data);
}

void test_recover_p6_lapply_promotes_to_head() {
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);

  std::vector<char> data(4096, 'H');
  write_p6_data(osd, dev, 1, 0, "hit", /*index*/ 3, /*term*/ 2, data, nullptr,
                nullptr, nullptr);
  persist_lapply(&kv, 1, 0, /*lapply*/ 3);
  r = {};
  osd.checkpoint_pass(on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, seg);
  kv_engine kv2(&dev, osd2.shared_allocator(), buffer_mode::heap);
  osd2.set_kv_engine(&kv2);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* pg = osd2.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_EQ(pg->pending_count(), 0u);
  EXPECT_TRUE(!pg->has_pending(3));

  std::vector<char> out(4096, 0);
  r = {};
  osd2.read(1, 0, "hit", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == data);
}

void test_recover_old_pair_type1_to_head() {
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  log_store* lg = osd.get_or_create_log(1, 0, osd.serving_epoch(1, 0));
  EXPECT_TRUE(lg != nullptr);
  log_entry e;
  e.term = 1;
  e.index = 1;
  e.type = 0;
  e.meta = "old-pair";
  e.data = "log-copy";
  r = {};
  lg->append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, '1');
  r = {};
  osd.apply(1, 0, "pair", 0, data.data(), data.size(), /*raft*/ 1, 0, on_done,
            &r);
  EXPECT_EQ(r.err, 0);

  uint64_t phys14 = 0;
  uint64_t al14 = 0;
  uint16_t hl14 = 0;
  uint64_t phys1 = 0;
  uint64_t al1 = 0;
  uint16_t hl1 = 0;
  EXPECT_TRUE(find_typed_record(dev, record_type::raft_log_entry, 1, &phys14,
                                &al14, &hl14));
  EXPECT_TRUE(find_typed_record(dev, record_type::data, 1, &phys1, &al1, &hl1));
  EXPECT_TRUE(phys1 != phys14);

  osd_store osd2(&dev, buffer_mode::heap, seg);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* pg = osd2.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_EQ(pg->pending_count(), 0u);
  log_store* lg2 = osd2.get_active_log(1, 0);
  EXPECT_TRUE(lg2 != nullptr);
  EXPECT_EQ(static_cast<unsigned>(lg2->rec_type_at(1)),
            static_cast<unsigned>(record_type::raft_log_entry));

  bool found_payload = false;
  const uint64_t want = payload_phys(phys1, hl1);
  pg->for_each_live_extent([&](uint64_t phys, uint64_t /*len*/) {
    if (phys == want) {
      found_payload = true;
    }
  });
  EXPECT_TRUE(found_payload);
  EXPECT_TRUE(want != phys14);

  std::vector<char> out(4096, 0);
  r = {};
  osd2.read(1, 0, "pair", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == data);
}

void test_recover_p6_pending_live_for_gc() {
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  kv_engine kv(&dev, osd.shared_allocator(), buffer_mode::heap);
  osd.set_kv_engine(&kv);

  std::vector<char> data(4096, 'G');
  uint64_t phys = 0;
  uint16_t hl = 0;
  write_p6_data(osd, dev, 1, 0, "gclive", /*index*/ 8, /*term*/ 1, data, &phys,
                nullptr, &hl);
  persist_lapply(&kv, 1, 0, /*lapply*/ 0);

  // Seal the victim with type=14 padding so recover imports a sealed segment.
  log_store* lg = osd.get_or_create_log(1, 0, osd.serving_epoch(1, 0));
  EXPECT_TRUE(lg != nullptr);
  uint64_t idx = lg->last_index() + 1;
  if (idx == 0) {
    idx = 9;
  }
  int n = 0;
  while (osd.allocator()->sealed_count() < 1 && n < 200) {
    log_entry e;
    e.term = 1;
    e.index = idx;
    e.type = 0;
    e.meta = "pad";
    e.data = std::string(64, 'L');
    r = {};
    lg->append(e, on_done, &r);
    EXPECT_EQ(r.err, 0);
    ++idx;
    ++n;
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 1u);

  r = {};
  osd.checkpoint_pass(on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, seg);
  kv_engine kv2(&dev, osd2.shared_allocator(), buffer_mode::heap);
  osd2.set_kv_engine(&kv2);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* pg = osd2.bind_existing_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  if (pg == nullptr) {
    return;
  }
  EXPECT_TRUE(pg->pending_count() >= 1u);

  std::vector<char> out(4096, 1);
  r = {};
  osd2.read(1, 0, "gclive", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));

  uint64_t live_phys = 0;
  pg->for_each_live_extent([&](uint64_t p, uint64_t len) {
    if (len == 4096) {
      live_phys = p;
    }
  });
  EXPECT_EQ(live_phys, payload_phys(phys, hl));

  // Drop index_map so the pending payload is the only live ref on that extent.
  log_store* lg2 = osd2.get_active_log(1, 0);
  EXPECT_TRUE(lg2 != nullptr);
  if (lg2 != nullptr && lg2->last_index() != 0) {
    lg2->trim_to(lg2->last_index());
  }
  EXPECT_TRUE(osd2.allocator() != nullptr);
  EXPECT_TRUE(osd2.allocator()->sealed_count() >= 1u);
  const uint32_t sealed_before = osd2.allocator()->sealed_count();
  EXPECT_EQ(osd2.gc_once(true), 0u);
  EXPECT_EQ(osd2.allocator()->sealed_count(), sealed_before);
  EXPECT_TRUE(pg->pending_count() >= 1u);

  live_phys = 0;
  pg->for_each_live_extent([&](uint64_t p, uint64_t len) {
    if (len == 4096) {
      live_phys = p;
    }
  });
  EXPECT_EQ(live_phys, payload_phys(phys, hl));

  r = {};
  pg->publish_commit(8, on_done, &r);
  EXPECT_EQ(r.err, 0);
  out.assign(4096, 0);
  r = {};
  osd2.read(1, 0, "gclive", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(out == data);
}

void put_varint_test(std::string& o, uint64_t v) {
  while (v >= 0x80) {
    o.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  o.push_back(static_cast<char>(v));
}

std::string test_write_cmd(const std::string& name, uint64_t off, uint64_t img) {
  std::string o;
  if (!name.empty()) {
    o.push_back(static_cast<char>((1 << 3) | 2));
    put_varint_test(o, name.size());
    o.append(name);
  }
  if (off != 0) {
    o.push_back(static_cast<char>((2 << 3) | 0));
    put_varint_test(o, off);
  }
  if (img != 0) {
    o.push_back(static_cast<char>((3 << 3) | 0));
    put_varint_test(o, img);
  }
  return o;
}

std::string test_delete_cmd(const std::string& name, uint64_t img) {
  std::string o;
  if (!name.empty()) {
    o.push_back(static_cast<char>((1 << 3) | 2));
    put_varint_test(o, name.size());
    o.append(name);
  }
  if (img != 0) {
    o.push_back(static_cast<char>((2 << 3) | 0));
    put_varint_test(o, img);
  }
  return o;
}

void dump_allocator(const char* tag, const segment_allocator* alloc) {
  if (alloc == nullptr) {
    std::printf("[gc-eval] %s allocator=null\n", tag);
    return;
  }
  std::printf("[gc-eval] %s segs=%u free=%u sealed=%u open=%u cursor=%lu\n",
              tag, alloc->segment_count(), alloc->free_count(),
              alloc->sealed_count(), alloc->open_segment(),
              static_cast<unsigned long>(alloc->open_cursor()));
  const uint32_t n = alloc->segment_count();
  for (uint32_t id = 0; id < n && id < 32; ++id) {
    const auto st = alloc->state(id);
    const char* name = "unknown";
    switch (st) {
    case segment_state::free:
      name = "free";
      break;
    case segment_state::open:
      name = "open";
      break;
    case segment_state::sealed:
      name = "sealed";
      break;
    case segment_state::gc_candidate:
      name = "gc_candidate";
      break;
    case segment_state::cleaning:
      name = "cleaning";
      break;
    case segment_state::bad:
      name = "bad";
      break;
    }
    std::printf("[gc-eval]   seg[%u]=%s\n", id, name);
  }
}

uint32_t count_typed_records(const mem_device_backend& dev, record_type ty) {
  uint32_t n = 0;
  const auto& s = dev.storage();
  const uint64_t wp = dev.write_pointer();
  const uint32_t bs = dev.block_size();
  uint64_t pos = 0;
  while (pos + kRecordHeaderLenV1 <= wp) {
    data_record rec;
    size_t consumed = 0;
    const decode_status st =
        decode_record(s.data() + static_cast<size_t>(pos),
                      static_cast<size_t>(wp - pos), &rec, &consumed);
    if (st != decode_status::ok || consumed == 0) {
      if (bs == 0) {
        break;
      }
      pos += bs;
      continue;
    }
    if (rec.type == ty) {
      ++n;
    }
    const uint64_t al = ((consumed + bs - 1) / bs) * bs;
    pos += al == 0 ? bs : al;
  }
  return n;
}

void test_record_default_encode_follows_flag() {
  data_record in;
  in.type = record_type::data;
  in.raft_term = 11;
  in.payload.assign(64, 'E');
  {
    unified_log_guard off(false);
    auto bytes = encode_record(in);
    EXPECT_EQ(bytes.size(), static_cast<size_t>(kRecordHeaderLenV1 + 64));
  }
  {
    unified_log_guard on(true);
    auto bytes = encode_record(in);
    EXPECT_EQ(bytes.size(), static_cast<size_t>(kRecordHeaderLenV3 + 64));
    data_record out;
    size_t consumed = 0;
    auto st = decode_record(bytes.data(), bytes.size(), &out, &consumed);
    EXPECT_TRUE(st == decode_status::ok);
    EXPECT_EQ(out.header_len, kRecordHeaderLenV3);
    EXPECT_EQ(out.raft_term, 11u);
  }
}

void test_flag0_write_still_dual_v2() {
  unified_log_guard off(false);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'D');
  r = {};
  osd.apply(1, 0, "dual", 0, data.data(), data.size(), /*raft*/ 1, 0, on_done,
            &r);
  EXPECT_EQ(r.err, 0);

  log_store* lg = osd.get_or_create_log(1, 0, osd.serving_epoch(1, 0));
  EXPECT_TRUE(lg != nullptr);
  log_entry e;
  e.term = 2;
  e.index = 1;
  e.type = 0;
  e.meta = test_write_cmd("dual", 0, 0);
  e.data.assign(data.begin(), data.end());
  r = {};
  lg->append(e, on_done, &r);
  EXPECT_EQ(r.err, 0);

  uint64_t start14 = 0;
  uint64_t al14 = 0;
  uint16_t hl14 = 0;
  EXPECT_TRUE(find_typed_record(dev, record_type::raft_log_entry, 1, &start14,
                                &al14, &hl14));
  EXPECT_EQ(hl14, kRecordHeaderLenV1);
  uint64_t start1 = 0;
  uint64_t al1 = 0;
  uint16_t hl1 = 0;
  EXPECT_TRUE(find_typed_record(dev, record_type::data, 1, &start1, &al1, &hl1));
  EXPECT_EQ(hl1, kRecordHeaderLenV1);
  EXPECT_TRUE(count_typed_records(dev, record_type::raft_log_entry) >= 1u);
  EXPECT_TRUE(count_typed_records(dev, record_type::data) >= 1u);

  auto bytes = encode_record(data_record{});
  EXPECT_EQ(bytes.size(), static_cast<size_t>(kRecordHeaderLenV1));
}

void test_unified_log_load_commit_idx() {
  // flag=1 load: never raise commit above current; unpublished suffix stays
  // pending until a current-term entry (election noop) is majority-committed.
  EXPECT_EQ(unified_log_load_commit_idx(90, 100), 90ull);
  EXPECT_EQ(unified_log_load_commit_idx(100, 100), 100ull);
  EXPECT_EQ(unified_log_load_commit_idx(110, 100), 100ull);
  EXPECT_EQ(unified_log_load_commit_idx(0, 0), 0ull);
  EXPECT_EQ(kRaftLogTypeNoop, 5ull);
}

void test_append_as_log_then_publish_no_second_data_record() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'P');
  log_entry e;
  e.term = 1;
  e.index = 3;
  e.type = 0;
  e.meta = test_write_cmd("pubonly", 0, 0);
  e.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);

  const uint32_t data_before = count_typed_records(dev, record_type::data);
  EXPECT_TRUE(data_before >= 1u);

  std::vector<char> out(4096, 0);
  r = {};
  osd.read(1, 0, "pubonly", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 0); // unpublished

  r = {};
  osd.publish_commit(1, 0, 3, on_done, &r);
  EXPECT_EQ(r.err, 0);

  r = {};
  out.assign(4096, 0);
  osd.read(1, 0, "pubonly", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'P');
  EXPECT_EQ(count_typed_records(dev, record_type::data), data_before);
}

void test_rewrite_updates_log_phys_disk_read() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'R');
  log_entry e;
  e.term = 2;
  e.index = 1;
  e.type = 0;
  e.meta = test_write_cmd("rwobj", 0, 0);
  e.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.publish_commit(1, 0, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);

  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  uint64_t old_phys = 0;
  lg->for_each_index([&](const log_store::index_meta& m) {
    if (m.index == 1) {
      old_phys = m.phys;
    }
  });
  EXPECT_TRUE(old_phys != 0);

  r = {};
  osd.rewrite_extent(1, 0, "rwobj", 0, 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  pg_store* pg = osd.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_EQ(pg->last_rewrite_raft_index(), 1ull);
  EXPECT_TRUE(pg->last_rewrite_record_start() != 0);
  EXPECT_TRUE(pg->last_rewrite_record_start() != old_phys);

  uint64_t new_phys = 0;
  lg->for_each_index([&](const log_store::index_meta& m) {
    if (m.index == 1) {
      new_phys = m.phys;
    }
  });
  EXPECT_EQ(new_phys, pg->last_rewrite_record_start());

  auto rr = read_log(*lg, 1, 1);
  EXPECT_EQ(rr.err, 0);
  EXPECT_EQ(rr.entries.size(), 1);
  EXPECT_EQ(rr.entries[0].data.size(), 4096);
  EXPECT_EQ(rr.entries[0].data[0], 'R');
}

void test_ckpt_recover_snapshot_ships_head() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> a(4096, 'A');
  log_entry e;
  e.term = 1;
  e.index = 4;
  e.type = 0;
  e.meta = test_write_cmd("snapobj", 0, 0);
  e.data.assign(a.begin(), a.end());
  r = {};
  osd.append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.publish_commit(1, 0, 4, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(osd.get_active_pg(1, 0)->has_version_log());

  r = {};
  osd.checkpoint_pass(on_done, &r);
  EXPECT_EQ(r.err, 0);

  osd_store osd2(&dev, buffer_mode::heap, 256 * 1024);
  r = {};
  osd2.recover(on_done, &r);
  EXPECT_EQ(r.err, 0);
  pg_store* pg2 = osd2.get_active_pg(1, 0);
  EXPECT_TRUE(pg2 != nullptr);
  EXPECT_TRUE(!pg2->has_version_log());

  struct open_pack {
    int err{-1};
    pg_snapshot_reader* reader{nullptr};
  } op;
  pg_snapshot_reader::open(
      pg2, /*last_applied*/ 4, /*term*/ 1,
      [](void* a, int err, pg_snapshot_reader* reader) {
        auto* p = static_cast<open_pack*>(a);
        p->err = err;
        p->reader = reader;
      },
      &op);
  EXPECT_EQ(op.err, 0);
  EXPECT_TRUE(op.reader != nullptr);
  EXPECT_TRUE(op.reader->total_chunks() >= 1u);

  std::vector<char> buf(4096, 0);
  struct {
    int err{-1};
    bool done{false};
    char first{0};
  } ch;
  op.reader->next_chunk(
      buf.data(), buf.size(),
      [](void* a, int err, bool done, const char* /*name*/, uint64_t /*off*/,
         uint64_t /*len*/) {
        auto* c = static_cast<decltype(ch)*>(a);
        c->err = err;
        c->done = done;
      },
      &ch);
  EXPECT_EQ(ch.err, 0);
  EXPECT_TRUE(!ch.done);
  EXPECT_EQ(buf[0], 'A');

  r = {};
  op.reader->close(on_done, &r);
  EXPECT_EQ(r.err, 0);
  delete op.reader;

  snapshot_ingest ing(&osd2, 1, 0);
  r = {};
  ing.begin(4, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  ing.apply_chunk("snapobj", 0, buf.data(), buf.size(), 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  ing.seal(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  ing.activate(on_done, &r);
  EXPECT_EQ(r.err, 0);
  std::vector<char> out(4096, 0);
  r = {};
  osd2.read(1, 0, "snapobj", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'A');
}

void test_snapshot_reader_after_log_trim() {
  // Store-level stand-in for raft §3: after prefix trim, snapshot still ships
  // committed head (log index is gone, object phys stays live).
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'T');
  log_entry e;
  e.term = 2;
  e.index = 6;
  e.type = 0;
  e.meta = test_write_cmd("trimsnap", 0, 0);
  e.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.publish_commit(1, 0, 6, on_done, &r);
  EXPECT_EQ(r.err, 0);

  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  lg->trim_to(6);
  EXPECT_TRUE(!lg->contains(6));

  pg_store* pg = osd.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  struct open_pack {
    int err{-1};
    pg_snapshot_reader* reader{nullptr};
  } op;
  pg_snapshot_reader::open(
      pg, /*last_applied*/ 6, /*term*/ 2,
      [](void* a, int err, pg_snapshot_reader* reader) {
        auto* p = static_cast<open_pack*>(a);
        p->err = err;
        p->reader = reader;
      },
      &op);
  EXPECT_EQ(op.err, 0);
  EXPECT_TRUE(op.reader != nullptr);
  EXPECT_TRUE(op.reader->total_chunks() >= 1u);

  std::vector<char> buf(4096, 0);
  struct {
    int err{-1};
    bool done{false};
  } ch;
  op.reader->next_chunk(
      buf.data(), buf.size(),
      [](void* a, int err, bool done, const char* /*name*/, uint64_t /*off*/,
         uint64_t /*len*/) {
        auto* c = static_cast<decltype(ch)*>(a);
        c->err = err;
        c->done = done;
      },
      &ch);
  EXPECT_EQ(ch.err, 0);
  EXPECT_TRUE(!ch.done);
  EXPECT_EQ(buf[0], 'T');

  r = {};
  op.reader->close(on_done, &r);
  EXPECT_EQ(r.err, 0);
  delete op.reader;
}

void test_empty_ingest_abort_keeps_serving() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'K');
  log_entry e;
  e.term = 1;
  e.index = 2;
  e.type = 0;
  e.meta = test_write_cmd("keep", 0, 0);
  e.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.publish_commit(1, 0, 2, on_done, &r);
  EXPECT_EQ(r.err, 0);

  snapshot_ingest ing(&osd, 1, 0);
  r = {};
  ing.begin(2, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(ing.chunk_count(), 0u);
  r = {};
  ing.abort(on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> out(4096, 0);
  r = {};
  osd.read(1, 0, "keep", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'K');
}

void append_write_and_publish(osd_store* osd, uint64_t index, uint64_t term,
                              const std::string& name, char fill) {
  std::vector<char> data(4096, fill);
  log_entry e;
  e.term = term;
  e.index = index;
  e.type = 0; // RAFT_LOGTYPE_WRITE
  e.meta = test_write_cmd(name, 0, 0);
  e.data.assign(data.begin(), data.end());
  sync_result r;
  osd->append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd->publish_commit(1, 0, index, on_done, &r);
  EXPECT_EQ(r.err, 0);
}

void append_delete_and_publish(osd_store* osd, uint64_t index, uint64_t term,
                               const std::string& name) {
  log_entry e;
  e.term = term;
  e.index = index;
  e.type = 1; // RAFT_LOGTYPE_DELETE
  e.meta = test_delete_cmd(name, 0);
  sync_result r;
  osd->append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd->publish_commit(1, 0, index, on_done, &r);
  EXPECT_EQ(r.err, 0);
}

// Unified-log (log=STM) GC: delete + prefix trim must recycle sealed
// segments that only held dead raft-log / object records, then reuse them.
void test_unified_log_delete_trim_recycles_segments() {
  unified_log_guard on(true);
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  uint64_t idx = 0;
  int victims = 0;
  while (osd.allocator()->sealed_count() < 2 && victims < 80) {
    ++idx;
    ++victims;
    append_write_and_publish(&osd, idx, /*term*/ 1,
                             "victim" + std::to_string(victims), 'V');
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 2u);
  std::printf("[gc-eval] wrote %d victim objects, last_idx=%lu\n", victims,
              static_cast<unsigned long>(idx));
  dump_allocator("after-victim-writes", osd.allocator());

  ++idx;
  const uint64_t keep_idx = idx;
  append_write_and_publish(&osd, keep_idx, 1, "keep", 'K');

  const uint32_t sealed_after_write = osd.allocator()->sealed_count();
  const uint32_t free_after_write = osd.allocator()->free_count();
  const uint32_t freed_while_live = osd.gc_once(true);
  std::printf("[gc-eval] gc while live log+head: freed=%u (expect 0)\n",
              freed_while_live);
  EXPECT_EQ(freed_while_live, 0u);
  EXPECT_EQ(osd.allocator()->sealed_count(), sealed_after_write);

  for (int i = 1; i <= victims; ++i) {
    ++idx;
    append_delete_and_publish(&osd, idx, 1, "victim" + std::to_string(i));
  }
  dump_allocator("after-deletes-before-trim", osd.allocator());

  const uint32_t freed_after_delete = osd.gc_once(true);
  std::printf("[gc-eval] gc after delete, log still live: freed=%u (expect 0)\n",
              freed_after_delete);
  EXPECT_EQ(freed_after_delete, 0u);
  EXPECT_EQ(osd.allocator()->sealed_count(), sealed_after_write);

  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  // Drop prefix through last victim write. keep + delete tombstones remain.
  lg->trim_to(keep_idx - 1);
  EXPECT_TRUE(lg->contains(keep_idx));
  EXPECT_TRUE(!lg->contains(1));
  std::printf("[gc-eval] trimmed log to %lu; base=%lu last=%lu count=%zu\n",
              static_cast<unsigned long>(keep_idx - 1),
              static_cast<unsigned long>(lg->base_index()),
              static_cast<unsigned long>(lg->last_index()), lg->entry_count());

  const uint32_t freed = osd.gc_once(true);
  dump_allocator("after-trim-gc", osd.allocator());
  std::printf("[gc-eval] gc after delete+trim: freed=%u free %u->%u sealed %u->%u\n",
              freed, free_after_write, osd.allocator()->free_count(),
              sealed_after_write, osd.allocator()->sealed_count());
  EXPECT_TRUE(freed >= 1u);
  EXPECT_TRUE(osd.allocator()->free_count() > free_after_write);
  EXPECT_TRUE(osd.allocator()->sealed_count() < sealed_after_write);

  std::vector<char> out(4096, 0);
  r = {};
  osd.read(1, 0, "keep", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'K');

  r = {};
  out.assign(4096, 0);
  osd.read(1, 0, "victim1", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 0);

  // Freed segments must be reusable for later appends.
  std::vector<uint32_t> free_ids;
  const uint32_t nseg = osd.allocator()->segment_count();
  for (uint32_t id = 0; id < nseg; ++id) {
    if (osd.allocator()->state(id) == segment_state::free) {
      free_ids.push_back(id);
    }
  }
  EXPECT_TRUE(!free_ids.empty());
  const uint32_t free_before_reuse = osd.allocator()->free_count();
  int extra = 0;
  while (osd.allocator()->free_count() == free_before_reuse && extra < 40) {
    ++idx;
    ++extra;
    append_write_and_publish(&osd, idx, 2, "reuse" + std::to_string(extra),
                             'R');
  }
  dump_allocator("after-reuse-writes", osd.allocator());
  std::printf("[gc-eval] reuse writes=%d free %u->%u\n", extra,
              free_before_reuse, osd.allocator()->free_count());
  EXPECT_TRUE(osd.allocator()->free_count() < free_before_reuse);
  bool reused = false;
  for (uint32_t id : free_ids) {
    if (osd.allocator()->state(id) != segment_state::free) {
      reused = true;
      std::printf("[gc-eval] reused previously-free seg[%u]\n", id);
    }
  }
  EXPECT_TRUE(reused);

  r = {};
  out.assign(4096, 0);
  osd.read(1, 0, "keep", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'K');
}

// Snapshot pin must block recycle of deleted extents until unpin.
void test_unified_log_snapshot_pin_blocks_gc() {
  unified_log_guard on(true);
  const uint64_t seg = 64 * 1024;
  mem_device_backend dev(4 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  uint64_t idx = 0;
  int victims = 0;
  while (osd.allocator()->sealed_count() < 2 && victims < 80) {
    ++idx;
    ++victims;
    append_write_and_publish(&osd, idx, 1, "pinobj" + std::to_string(victims),
                             'P');
  }
  EXPECT_TRUE(osd.allocator()->sealed_count() >= 2u);
  const uint64_t snap_idx = idx;

  pg_store* pg = osd.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  r = {};
  pg->create_raft_snapshot(snap_idx, /*term*/ 1, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(pg->snapshot_pin_count(snap_idx) >= 1u);

  ++idx;
  append_write_and_publish(&osd, idx, 1, "keep2", 'Q');
  for (int i = 1; i <= victims; ++i) {
    ++idx;
    append_delete_and_publish(&osd, idx, 1, "pinobj" + std::to_string(i));
  }
  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  lg->trim_to(snap_idx);
  dump_allocator("pinned-after-delete-trim", osd.allocator());

  const uint32_t sealed_pinned = osd.allocator()->sealed_count();
  const uint32_t freed_pinned = osd.gc_once(true);
  std::printf("[gc-eval] gc while snapshot pinned: freed=%u (expect 0)\n",
              freed_pinned);
  EXPECT_EQ(freed_pinned, 0u);
  EXPECT_EQ(osd.allocator()->sealed_count(), sealed_pinned);

  std::vector<char> hist(4096, 0);
  r = {};
  pg->read("pinobj1", 0, hist.data(), 4096, snap_idx, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(hist[0], 'P');

  r = {};
  pg->release_raft_snapshot(snap_idx, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(pg->snapshot_pin_count(snap_idx), 0u);

  const uint32_t freed_unpinned = osd.gc_once(true);
  dump_allocator("after-unpin-gc", osd.allocator());
  std::printf("[gc-eval] gc after unpin: freed=%u\n", freed_unpinned);
  EXPECT_TRUE(freed_unpinned >= 1u);
  EXPECT_TRUE(osd.allocator()->sealed_count() < sealed_pinned);

  std::vector<char> out(4096, 0);
  r = {};
  osd.read(1, 0, "keep2", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'Q');
}

void test_trim_gc_keeps_head_after_log_trim() {
  unified_log_guard on(true);
  const uint64_t seg = 256 * 1024;
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, seg);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'H');
  log_entry e;
  e.term = 1;
  e.index = 1;
  e.type = 0;
  e.meta = test_write_cmd("keephead", 0, 0);
  e.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.publish_commit(1, 0, 1, on_done, &r);
  EXPECT_EQ(r.err, 0);

  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  lg->trim_to(1);
  EXPECT_TRUE(!lg->contains(1));

  const uint32_t freed = osd.gc_once(false);
  (void)freed;
  std::vector<char> out(4096, 0);
  r = {};
  osd.read(1, 0, "keephead", 0, out.data(), 4096, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_EQ(out[0], 'H');
}

void test_append_as_log_write_pending_v3() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'V');
  log_entry e;
  e.term = 7;
  e.index = 1;
  e.type = 0;
  e.meta = test_write_cmd("v3obj", 0, 0);
  e.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, e, on_done, &r);
  EXPECT_EQ(r.err, 0);

  pg_store* pg = osd.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_TRUE(pg->has_pending(1));
  EXPECT_TRUE(pg->pending_count() >= 1u);

  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  EXPECT_TRUE(lg->contains(1));
  EXPECT_EQ(static_cast<unsigned>(lg->rec_type_at(1)),
            static_cast<unsigned>(record_type::data));
  EXPECT_EQ(lg->last_index(), 1);

  std::vector<char> out(4096, 1);
  r = {};
  osd.read(1, 0, "v3obj", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));

  uint64_t start = 0;
  uint64_t al = 0;
  uint16_t hl = 0;
  EXPECT_TRUE(find_typed_record(dev, record_type::data, 1, &start, &al, &hl));
  EXPECT_EQ(hl, kRecordHeaderLenV3);
  data_record rec;
  size_t consumed = 0;
  const auto& s = dev.storage();
  auto st = decode_record(s.data() + static_cast<size_t>(start),
                          static_cast<size_t>(al), &rec, &consumed);
  EXPECT_TRUE(st == decode_status::ok);
  EXPECT_EQ(rec.raft_term, 7u);
  EXPECT_EQ(rec.header_len, kRecordHeaderLenV3);
  EXPECT_EQ(count_typed_records(dev, record_type::raft_log_entry), 0u);
  EXPECT_EQ(count_typed_records(dev, record_type::data), 1u);
}

void test_append_as_log_write_config_sequential() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  const uint32_t reserved_before =
      osd.allocator() != nullptr ? osd.allocator()->open_segment()
                                 : segment_allocator::kInvalidSeg;
  (void)reserved_before;

  std::vector<char> data(4096, 'S');
  log_entry w;
  w.term = 3;
  w.index = 1;
  w.type = 0;
  w.meta = test_write_cmd("seq", 0, 0);
  w.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, w, on_done, &r);
  EXPECT_EQ(r.err, 0);

  log_entry cfg;
  cfg.term = 3;
  cfg.index = 2;
  cfg.type = 3; // RAFT_LOGTYPE_CONFIGURATION
  cfg.meta = "cfg-members";
  r = {};
  osd.append_as_log(1, 0, cfg, on_done, &r);
  EXPECT_EQ(r.err, 0);

  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  EXPECT_EQ(lg->last_index(), 2);
  EXPECT_TRUE(lg->contains(1));
  EXPECT_TRUE(lg->contains(2));
  EXPECT_EQ(static_cast<unsigned>(lg->rec_type_at(1)),
            static_cast<unsigned>(record_type::data));
  EXPECT_EQ(static_cast<unsigned>(lg->rec_type_at(2)),
            static_cast<unsigned>(record_type::raft_log_entry));
  EXPECT_EQ(lg->get_term(1), 3);
  EXPECT_EQ(lg->get_term(2), 3);

  uint64_t p1 = 0;
  uint64_t a1 = 0;
  uint16_t h1 = 0;
  uint64_t p2 = 0;
  uint64_t a2 = 0;
  uint16_t h2 = 0;
  EXPECT_TRUE(find_typed_record(dev, record_type::data, 1, &p1, &a1, &h1));
  EXPECT_TRUE(
      find_typed_record(dev, record_type::raft_log_entry, 2, &p2, &a2, &h2));
  EXPECT_EQ(h1, kRecordHeaderLenV3);
  EXPECT_EQ(h2, kRecordHeaderLenV3);
  EXPECT_TRUE(p2 != p1);
  EXPECT_EQ(count_typed_records(dev, record_type::data), 1u);
  EXPECT_EQ(count_typed_records(dev, record_type::raft_log_entry), 1u);
  EXPECT_TRUE(osd.allocator() != nullptr);
  EXPECT_TRUE(osd.allocator()->open_segment() !=
              segment_allocator::kInvalidSeg);
}

void test_append_as_log_missing_name_not_published() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  log_entry bad;
  bad.term = 1;
  bad.index = 1;
  bad.type = 0;
  bad.meta.clear();
  bad.data.assign(4096, 'X');
  r = {};
  osd.append_as_log(1, 0, bad, on_done, &r);
  EXPECT_EQ(r.err, -EINVAL);

  pg_store* pg = osd.get_active_pg(1, 0);
  EXPECT_TRUE(pg != nullptr);
  EXPECT_TRUE(!pg->has_pending(1));
  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(lg != nullptr);
  EXPECT_TRUE(!lg->contains(1));

  std::vector<char> data(4096, 'M');
  log_entry ok;
  ok.term = 1;
  ok.index = 1;
  ok.type = 0;
  ok.meta = test_write_cmd("named", 0, 0);
  ok.data.assign(data.begin(), data.end());
  r = {};
  osd.append_as_log(1, 0, ok, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(pg->has_pending(1));
  std::vector<char> out(4096, 2);
  r = {};
  osd.read(1, 0, "named", 0, out.data(), out.size(), on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](char c) { return c == 0; }));
}

void test_truncate_log_and_pending() {
  unified_log_guard on(true);
  mem_device_backend dev(8 * 1024 * 1024);
  osd_store osd(&dev, buffer_mode::heap, 256 * 1024);
  sync_result r;
  osd.format(on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  osd.create_pg(1, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  std::vector<char> data(4096, 'T');
  for (uint64_t i = 1; i <= 3; ++i) {
    log_entry e;
    e.term = 1;
    e.index = i;
    e.type = 0;
    e.meta = test_write_cmd("trunc", (i - 1) * 4096, 0);
    e.data.assign(data.begin(), data.end());
    r = {};
    osd.append_as_log(1, 0, e, on_done, &r);
    EXPECT_EQ(r.err, 0);
  }
  pg_store* pg = osd.get_active_pg(1, 0);
  log_store* lg = osd.get_active_log(1, 0);
  EXPECT_TRUE(pg != nullptr && lg != nullptr);
  EXPECT_TRUE(pg->has_pending(1));
  EXPECT_TRUE(pg->has_pending(2));
  EXPECT_TRUE(pg->has_pending(3));
  EXPECT_EQ(lg->last_index(), 3);

  r = {};
  osd.truncate_log_and_pending(1, 0, 2, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(pg->has_pending(1));
  EXPECT_TRUE(!pg->has_pending(2));
  EXPECT_TRUE(!pg->has_pending(3));
  EXPECT_TRUE(lg->contains(1));
  EXPECT_TRUE(!lg->contains(2));
  EXPECT_TRUE(!lg->contains(3));
  EXPECT_EQ(lg->last_index(), 1);
}

void test_pg_store_list_live_names_snapshot_view() {
  mem_device_backend dev(32 * 1024 * 1024);
  pg_store store(&dev, pg_id{1, 0, 1});
  std::vector<char> data(4096, 'L');
  sync_result r;
  std::vector<std::string> names;

  store.append_uncommitted("ghost", 0, data.data(), data.size(), 3, 1, 0,
                           on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(store.object_exists("ghost"));
  store.list_live_object_names(&names);
  EXPECT_EQ(names.size(), 0u);

  r = {};
  store.write("keep", 0, data.data(), data.size(), 5, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.write("gone", 0, data.data(), data.size(), 6, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);
  r = {};
  store.remove("gone", 8, 0, on_done, &r);
  EXPECT_EQ(r.err, 0);

  store.list_live_object_names(&names);
  EXPECT_EQ(names.size(), 1u);
  EXPECT_TRUE(names.size() == 1 && names[0] == "keep");

  store.list_live_object_names_at(6, 0, &names);
  EXPECT_EQ(names.size(), 2u);
  bool saw_keep = false;
  bool saw_gone = false;
  bool saw_ghost = false;
  for (const auto& n : names) {
    if (n == "keep") {
      saw_keep = true;
    }
    if (n == "gone") {
      saw_gone = true;
    }
    if (n == "ghost") {
      saw_ghost = true;
    }
  }
  EXPECT_TRUE(saw_keep);
  EXPECT_TRUE(saw_gone);
  EXPECT_TRUE(!saw_ghost);

  store.list_live_object_names_at(4, 0, &names);
  EXPECT_EQ(names.size(), 0u);

  r = {};
  store.publish_commit(3, on_done, &r);
  EXPECT_EQ(r.err, 0);
  EXPECT_TRUE(store.object_exists("ghost"));
  store.list_live_object_names(&names);
  EXPECT_EQ(names.size(), 2u);
  store.list_live_object_names_at(4, 0, &names);
  EXPECT_EQ(names.size(), 1u);
  EXPECT_TRUE(names.size() == 1 && names[0] == "ghost");
}

// open() used to close() a never-started runtime. That latched stop_finished_
// and the OSD --mkfs shutdown never invoked the stop callback.
void test_runtime_close_before_open_does_not_swallow_stop() {
  struct stop_result {
    int calls{0};
    int err{1};
  };
  aos_runtime rt;
  rt.close();
  stop_result first;
  rt.stop(
      [](void* arg, int e) {
        auto* r = static_cast<stop_result*>(arg);
        r->calls += 1;
        r->err = e;
      },
      &first);
  EXPECT_EQ(first.calls, 1);
  EXPECT_EQ(first.err, 0);
  stop_result second;
  rt.stop(
      [](void* arg, int e) {
        auto* r = static_cast<stop_result*>(arg);
        r->calls += 1;
        r->err = e;
      },
      &second);
  EXPECT_EQ(second.calls, 1);
  EXPECT_EQ(second.err, 0);
}

} // namespace

int main() {
  test_crc32_smoke();
  test_record_roundtrip();
  test_record_v3_roundtrip();
  test_record_default_encode_v2();
  test_record_illegal_version_and_header_len();
  test_record_v3_short_header_need_more();
  test_record_need_more();
  test_record_bad_payload_crc();
  test_record_bad_magic();
  test_extent_map_overwrite();
  test_extent_map_split_right();
  test_extent_map_discard();
  test_alignment_helpers();
  test_checkpoint_payload_roundtrip();
  test_pg_store_write_read();
  test_pg_store_overwrite_and_idempotent();
  test_pg_store_remove();
  test_pg_store_discard();
  test_pg_store_discard_recover();
  test_pg_store_replay_image();
  test_pg_store_async_recover();
  test_pg_store_queue_serialized();
  test_pg_store_checkpoint_and_recover();
  test_pg_store_torn_checkpoint_fallback();
  test_pg_store_checkpoint_then_discard_recover();
  test_segment_allocator_no_double_alloc();
  test_note_written_does_not_rewind_cursor();
  test_pg_store_multi_segment_write_recover();
  test_pg_store_raft_snapshot_read();
  test_pg_store_image_snapshot_read();
  test_segment_allocator_free_sealed();
  test_osd_store_gc_once();
  test_gc_pins_object_name_across_free();
  test_gc_pass_async_completes_before_free();
  test_osd_store_apply_read_remove();
  test_snapshot_ingest_state_machine();
  test_pg_snapshot_reader_stream();
  test_pg_store_rewrite_extent();
  test_osd_store_gc_rewrite_once();
  test_superblock_manifest_codec();
  test_log_store_checkpoint_rebuild();
  test_osd_checkpoint_pass_then_recover();
  test_format_zeros_leftover_then_recover_empty();
  test_osd_store_multi_pg_shared_allocator();
  test_parse_object_engine_mode();
  test_compute_shard_region();
  test_gc_rewrite_rate_limit();
  test_log_store_append_read_trim();
  test_log_store_append_type14_still_readable();
  test_log_store_register_data_as_log();
  test_log_store_read_requires_name_lookup();
  test_log_store_old_checkpoint_default_raft();
  test_log_store_checkpoint_preserves_data_rec_type();
  test_log_store_range_tombstone_read_notsup();
  test_log_store_gc_rewrite_as_data();
  test_log_store_update_phys();
  test_log_store_delete_tombstone_as_log();
  test_osd_store_injects_name_lookup();
  test_log_store_rebuild_from_image();
  test_log_store_torn_tail();
  test_kv_engine_put_durable_and_stop_flush();
  test_kv_engine_put_visible_before_commit();
  test_kv_engine_put_commit_get();
  test_kv_engine_delete_and_checkpoint_replay();
  test_kv_engine_torn_wal_keeps_prior();
  test_kv_engine_torn_checkpoint_keeps_prior();
  test_log_store_install_snapshot_base();
  test_log_store_install_snapshot_base_replaces_shorter_log();
  test_log_store_truncate_from();
  test_gc_preserves_live_log_and_frees_after_trim();
  test_gc_preserves_live_kv_and_frees_after_checkpoint();
  test_osd_store_list_pg_ids();
  test_osd_store_empty_recover_then_append();
  test_recover_rebinds_kv_allocator();
  test_osd_store_recover_keeps_existing_epoch();
  test_osd_store_recover_keeps_existing_epoch_512();
  test_osd_store_recover_keeps_kv_across_pad_hole();
  test_osd_store_recover_skips_nonzero_pad_hole();
  test_osd_store_recover_vstart_image_if_present();
  test_osd_store_recover_skips_zero_hole();
  test_aos_dump_empty_and_formatted();
  test_aos_dump_counts_kv_and_log_records();
  test_aos_dump_collect_live_lww();
  test_pg_store_flatten_image();
  test_osd_store_image_snap_and_gc_pass();
  test_pg_store_append_uncommitted_not_readable();
  test_pg_store_publish_commit_readable();
  test_pg_store_drop_pending_isolated_name();
  test_pg_store_drop_keeps_discarded_committed();
  test_pg_store_drop_inflight_uncommitted();
  test_pg_store_write_still_immediately_readable();
  test_pg_store_write_phys_uses_header_len();
  test_recover_p6_unpublished_pending_after_ckpt();
  test_recover_p6_unpublished_no_ckpt();
  test_recover_p6_lapply_promotes_to_head();
  test_recover_old_pair_type1_to_head();
  test_recover_p6_pending_live_for_gc();
  test_record_default_encode_follows_flag();
  test_flag0_write_still_dual_v2();
  test_unified_log_load_commit_idx();
  test_append_as_log_then_publish_no_second_data_record();
  test_rewrite_updates_log_phys_disk_read();
  test_ckpt_recover_snapshot_ships_head();
  test_snapshot_reader_after_log_trim();
  test_empty_ingest_abort_keeps_serving();
  test_trim_gc_keeps_head_after_log_trim();
  test_unified_log_delete_trim_recycles_segments();
  test_unified_log_snapshot_pin_blocks_gc();
  test_append_as_log_write_pending_v3();
  test_append_as_log_write_config_sequential();
  test_append_as_log_missing_name_not_published();
  test_truncate_log_and_pending();
  test_pg_store_list_live_names_snapshot_view();
  test_runtime_close_before_open_does_not_swallow_stop();

  if (g_failures != 0) {
    std::fprintf(stderr, "aos_m1_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("aos_m1_test: ok\n");
  return 0;
}
