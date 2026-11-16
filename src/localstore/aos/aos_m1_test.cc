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

#include "checkpoint.h"
#include "crc32.h"
#include "extent_map.h"
#include "layout.h"
#include "mem_device_backend.h"
#include "osd_store.h"
#include "pg_store.h"
#include "record.h"
#include "segment_allocator.h"
#include "superblock.h"
#include "types.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
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
  test_segment_allocator_free_sealed();
  test_osd_store_gc_once();
  test_superblock_manifest_codec();
  test_osd_store_multi_pg_shared_allocator();

  if (g_failures != 0) {
    std::fprintf(stderr, "aos_m1_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("aos_m1_test: ok\n");
  return 0;
}
