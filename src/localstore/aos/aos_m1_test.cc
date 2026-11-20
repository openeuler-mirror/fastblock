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
#include "segment_allocator.h"
#include "snapshot_ingest.h"
#include "snapshot_reader.h"
#include "superblock.h"
#include "types.h"

#include <algorithm>
#include <cassert>
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

  auto bytes = encode_record(in);
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
  EXPECT_EQ(out.payload.size(), 4096u);
  EXPECT_TRUE(out.payload == in.payload);
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
}

void test_osd_store_gc_rewrite_once() {
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

void test_osd_store_recover_keeps_existing_epoch_512() {
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
  EXPECT_TRUE(nid > 1);

  store.drop_pending_from(3);
  EXPECT_TRUE(!store.object_exists("ghost"));
  EXPECT_EQ(store.next_object_id(), nid);

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

} // namespace

int main() {
  test_crc32_smoke();
  test_record_roundtrip();
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
  test_pg_store_multi_segment_write_recover();
  test_pg_store_raft_snapshot_read();
  test_pg_store_image_snapshot_read();
  test_segment_allocator_free_sealed();
  test_osd_store_gc_once();
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
  test_log_store_rebuild_from_image();
  test_log_store_torn_tail();
  test_kv_engine_put_durable_and_stop_flush();
  test_kv_engine_put_visible_before_commit();
  test_kv_engine_put_commit_get();
  test_kv_engine_delete_and_checkpoint_replay();
  test_kv_engine_torn_wal_keeps_prior();
  test_kv_engine_torn_checkpoint_keeps_prior();
  test_log_store_install_snapshot_base();
  test_log_store_truncate_from();
  test_gc_preserves_live_log_and_frees_after_trim();
  test_gc_preserves_live_kv_and_frees_after_checkpoint();
  test_osd_store_list_pg_ids();
  test_osd_store_empty_recover_then_append();
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
  test_pg_store_write_still_immediately_readable();
  test_pg_store_write_phys_uses_header_len();

  if (g_failures != 0) {
    std::fprintf(stderr, "aos_m1_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("aos_m1_test: ok\n");
  return 0;
}
