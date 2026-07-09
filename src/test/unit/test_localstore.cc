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

/**
 * @file test_localstore.cc
 * @brief Unit tests for localstore module (types, blob, log_entry, spdk_buffer)
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include "localstore/types.h"
#include "localstore/log_entry.h"
#include "localstore/spdk_buffer.h"
#include "raft/raft.h"

#include <string>
#include <cstring>
#include <limits>

// ============================================================================
// Test Suite: blob_type (Blob Type Enumeration Tests)
// ============================================================================

FB_SUITE_SETUP(blob_type) {
    // Setup code here
}

FB_SUITE_TEARDOWN(blob_type) {
    // Teardown code here
}

FB_TEST(blob_type, log_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::log), 0);
}

FB_TEST(blob_type, object_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object), 1);
}

FB_TEST(blob_type, object_snap_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_snap), 2);
}

FB_TEST(blob_type, object_recover_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_recover), 3);
}

FB_TEST(blob_type, kv_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv), 4);
}

FB_TEST(blob_type, kv_checkpoint_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint), 5);
}

FB_TEST(blob_type, kv_checkpoint_new_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint_new), 6);
}

FB_TEST(blob_type, super_blob_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::super_blob), 7);
}

FB_TEST(blob_type, free_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::free), 8);
}

// ============================================================================
// Test Suite: blob_type_string (Blob Type String Conversion Tests)
// ============================================================================

FB_SUITE_SETUP(blob_type_string) {
    // Setup code here
}

FB_SUITE_TEARDOWN(blob_type_string) {
    // Teardown code here
}

FB_TEST(blob_type_string, log_string) {
    FB_ASSERT_EQ(type_string(blob_type::log), "blob_type::log");
}

FB_TEST(blob_type_string, object_string) {
    FB_ASSERT_EQ(type_string(blob_type::object), "blob_type::object");
}

FB_TEST(blob_type_string, object_snap_string) {
    FB_ASSERT_EQ(type_string(blob_type::object_snap), "blob_type::object_snap");
}

FB_TEST(blob_type_string, object_recover_string) {
    FB_ASSERT_EQ(type_string(blob_type::object_recover), "blob_type::object_recover");
}

FB_TEST(blob_type_string, kv_string) {
    FB_ASSERT_EQ(type_string(blob_type::kv), "blob_type::kv");
}

FB_TEST(blob_type_string, kv_checkpoint_string) {
    FB_ASSERT_EQ(type_string(blob_type::kv_checkpoint), "blob_type::kv_checkpoint");
}

FB_TEST(blob_type_string, kv_checkpoint_new_string) {
    FB_ASSERT_EQ(type_string(blob_type::kv_checkpoint_new), "blob_type::kv_checkpoint_new");
}

FB_TEST(blob_type_string, super_blob_string) {
    FB_ASSERT_EQ(type_string(blob_type::super_blob), "blob_type::super_blob");
}

FB_TEST(blob_type_string, free_string) {
    FB_ASSERT_EQ(type_string(blob_type::free), "blob_type::free");
}

// ============================================================================
// Test Suite: fb_blob (Blob Structure Tests)
// ============================================================================

FB_SUITE_SETUP(fb_blob) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fb_blob) {
    // Teardown code here
}

FB_TEST(fb_blob, default_values) {
    fb_blob blob;
    FB_ASSERT_EQ(blob.blob, nullptr);
    FB_ASSERT_EQ(blob.blobid, 0);
}

FB_TEST(fb_blob, initialized_values) {
    fb_blob blob;
    blob.blob = reinterpret_cast<struct spdk_blob*>(0x12345678);
    blob.blobid = 12345;
    FB_ASSERT_EQ(blob.blob, reinterpret_cast<struct spdk_blob*>(0x12345678));
    FB_ASSERT_EQ(blob.blobid, 12345);
}

FB_TEST(fb_blob, size_check) {
    FB_ASSERT_TRUE(sizeof(fb_blob) >= sizeof(void*) + sizeof(spdk_blob_id));
}

FB_TEST(fb_blob, assignment) {
    fb_blob blob1;
    blob1.blobid = 100;

    fb_blob blob2;
    blob2 = blob1;
    FB_ASSERT_EQ(blob2.blobid, 100);
}

// ============================================================================
// Test Suite: spdk_buffer (SPDK Buffer Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer) {
    // Teardown code here
}

FB_TEST(spdk_buffer, default_constructor) {
    spdk_buffer sbuf;
    FB_ASSERT_EQ(sbuf.size(), 0);
    FB_ASSERT_EQ(sbuf.used(), 0);
    FB_ASSERT_EQ(sbuf.remain(), 0);
}

FB_TEST(spdk_buffer, parameterized_constructor) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    FB_ASSERT_EQ(sbuf.size(), 100);
    FB_ASSERT_EQ(sbuf.used(), 0);
    FB_ASSERT_EQ(sbuf.remain(), 100);
}

FB_TEST(spdk_buffer, append_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    const char* data = "hello";
    size_t written = sbuf.append(data, 5);
    FB_ASSERT_EQ(written, 5);
    FB_ASSERT_EQ(sbuf.used(), 5);
    FB_ASSERT_EQ(sbuf.remain(), 95);
}

FB_TEST(spdk_buffer, append_string) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string str = "world";
    size_t written = sbuf.append(str);
    FB_ASSERT_EQ(written, 5);
    FB_ASSERT_EQ(sbuf.used(), 5);
}

FB_TEST(spdk_buffer, append_overflow) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    const char* data = "123456789012345";  // 15 chars
    size_t written = sbuf.append(data, 15);
    FB_ASSERT_EQ(written, 10);  // Only 10 bytes written
    FB_ASSERT_EQ(sbuf.used(), 10);
    FB_ASSERT_EQ(sbuf.remain(), 0);
}

// ============================================================================
// Test Suite: spdk_buffer_advanced (Advanced SPDK Buffer Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer_advanced) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer_advanced) {
    // Teardown code here
}

FB_TEST(spdk_buffer_advanced, inc_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    size_t inc = sbuf.inc(10);
    FB_ASSERT_EQ(inc, 10);
    FB_ASSERT_EQ(sbuf.used(), 10);
}

FB_TEST(spdk_buffer_advanced, inc_overflow) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    size_t inc = sbuf.inc(100);
    FB_ASSERT_EQ(inc, 10);
    FB_ASSERT_EQ(sbuf.used(), 10);
}

FB_TEST(spdk_buffer_advanced, reset) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    sbuf.append("test", 4);
    FB_ASSERT_EQ(sbuf.used(), 4);

    sbuf.reset();
    FB_ASSERT_EQ(sbuf.used(), 0);
    FB_ASSERT_EQ(sbuf.remain(), 100);
}

FB_TEST(spdk_buffer_advanced, set_used) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    sbuf.set_used(50);
    FB_ASSERT_EQ(sbuf.used(), 50);
    FB_ASSERT_EQ(sbuf.remain(), 50);
}

FB_TEST(spdk_buffer_advanced, set_used_overflow) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    sbuf.set_used(100);
    FB_ASSERT_EQ(sbuf.used(), 10);  // Capped at size
}

FB_TEST(spdk_buffer_advanced, get_append) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    char* append_ptr = sbuf.get_append();
    FB_ASSERT_EQ(append_ptr, buffer);

    sbuf.inc(10);
    append_ptr = sbuf.get_append();
    FB_ASSERT_EQ(append_ptr, buffer + 10);
}

// ============================================================================
// Test Suite: log_entry (Log Entry Tests)
// ============================================================================

FB_SUITE_SETUP(log_entry) {
    // Setup code here
}

FB_SUITE_TEARDOWN(log_entry) {
    // Teardown code here
}

FB_TEST(log_entry, default_values) {
    log_entry_t entry;
    FB_ASSERT_EQ(entry.term_id, std::numeric_limits<uint64_t>::max());
    FB_ASSERT_EQ(entry.index, std::numeric_limits<uint64_t>::max());
    FB_ASSERT_EQ(entry.size, std::numeric_limits<uint64_t>::max());
    FB_ASSERT_EQ(entry.type, std::numeric_limits<uint64_t>::max());
}

FB_TEST(log_entry, initialization) {
    log_entry_t entry;
    entry.term_id = 1;
    entry.index = 100;
    entry.size = 256;
    entry.type = 1;  // log type
    entry.meta = "test_meta";

    FB_ASSERT_EQ(entry.term_id, 1);
    FB_ASSERT_EQ(entry.index, 100);
    FB_ASSERT_EQ(entry.size, 256);
    FB_ASSERT_EQ(entry.type, 1);
    FB_ASSERT_EQ(entry.meta, "test_meta");
}

FB_TEST(log_entry, header_size) {
    FB_ASSERT_EQ(entry_header_size, sizeof(uint64_t) * 3);
}

FB_TEST(log_entry, multiple_entries) {
    log_entry_t entries[10];
    for (int i = 0; i < 10; i++) {
        entries[i].term_id = i;
        entries[i].index = i * 10;
        entries[i].size = i * 100;
    }

    for (int i = 0; i < 10; i++) {
        FB_ASSERT_EQ(entries[i].term_id, static_cast<uint64_t>(i));
        FB_ASSERT_EQ(entries[i].index, static_cast<uint64_t>(i * 10));
        FB_ASSERT_EQ(entries[i].size, static_cast<uint64_t>(i * 100));
    }
}

FB_TEST(log_entry, meta_string_operations) {
    log_entry_t entry;
    entry.meta = "hello";
    FB_ASSERT_EQ(entry.meta.size(), 5);

    entry.meta = "";
    FB_ASSERT_EQ(entry.meta.size(), 0);

    entry.meta = "a very long metadata string for testing";
    FB_ASSERT_TRUE(entry.meta.size() > 30);
}

FB_TEST(log_entry, term_progression) {
    log_entry_t entry;
    for (uint64_t term = 1; term <= 100; term++) {
        entry.term_id = term;
        FB_ASSERT_EQ(entry.term_id, term);
    }
}

FB_TEST(log_entry, index_progression) {
    log_entry_t entry;
    for (uint64_t idx = 0; idx < 1000; idx++) {
        entry.index = idx;
        FB_ASSERT_EQ(entry.index, idx);
    }
}

// ============================================================================
// Test Suite: log_entry_types (Log Entry Type Tests)
// ============================================================================

FB_SUITE_SETUP(log_entry_types) {
    // Setup code here
}

FB_SUITE_TEARDOWN(log_entry_types) {
    // Teardown code here
}

FB_TEST(log_entry_types, write_type) {
    log_entry_t entry;
    entry.type = static_cast<uint64_t>(RAFT_LOGTYPE_WRITE);
    FB_ASSERT_EQ(entry.type, 0);
}

FB_TEST(log_entry_types, delete_type) {
    log_entry_t entry;
    entry.type = static_cast<uint64_t>(RAFT_LOGTYPE_DELETE);
    FB_ASSERT_EQ(entry.type, 1);
}

FB_TEST(log_entry_types, add_nonvoting_type) {
    log_entry_t entry;
    entry.type = static_cast<uint64_t>(RAFT_LOGTYPE_ADD_NONVOTING_NODE);
    FB_ASSERT_EQ(entry.type, 2);
}

FB_TEST(log_entry_types, configuration_type) {
    log_entry_t entry;
    entry.type = static_cast<uint64_t>(RAFT_LOGTYPE_CONFIGURATION);
    FB_ASSERT_EQ(entry.type, 3);
}

FB_TEST(log_entry_types, type_comparison) {
    log_entry_t entry1, entry2;
    entry1.type = static_cast<uint64_t>(RAFT_LOGTYPE_WRITE);
    entry2.type = static_cast<uint64_t>(RAFT_LOGTYPE_CONFIGURATION);
    FB_ASSERT_TRUE(entry1.type < entry2.type);
}

// ============================================================================
// Test Suite: buffer_list (Buffer List Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list) {
    // Teardown code here
}

FB_TEST(buffer_list, empty_list) {
    buffer_list bl;
    FB_ASSERT_EQ(bl.bytes(), 0);
    FB_ASSERT_TRUE(bl.empty());
}

FB_TEST(buffer_list, single_buffer) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    buffer_list bl;
    bl.append_buffer(sbuf);
    FB_ASSERT_EQ(bl.bytes(), 100);
    FB_ASSERT_FALSE(bl.empty());
}

FB_TEST(buffer_list, multiple_buffers) {
    char buffer1[100], buffer2[200], buffer3[300];
    spdk_buffer sbuf1(buffer1, 100);
    spdk_buffer sbuf2(buffer2, 200);
    spdk_buffer sbuf3(buffer3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    FB_ASSERT_EQ(bl.bytes(), 600);
}

FB_TEST(buffer_list, prepend_buffer) {
    char buffer1[100], buffer2[200];
    spdk_buffer sbuf1(buffer1, 100);
    spdk_buffer sbuf2(buffer2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.prepend_buffer(sbuf2);

    FB_ASSERT_EQ(bl.bytes(), 300);
}

FB_TEST(buffer_list, clear_list) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    buffer_list bl;
    bl.append_buffer(sbuf);
    FB_ASSERT_EQ(bl.bytes(), 100);

    bl.clear();
    FB_ASSERT_EQ(bl.bytes(), 0);
    FB_ASSERT_TRUE(bl.empty());
}

// ============================================================================
// Test Suite: buffer_list_advanced (Advanced Buffer List Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_advanced) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_advanced) {
    // Teardown code here
}

FB_TEST(buffer_list_advanced, append_buffer_list) {
    char buffer1[100], buffer2[200];
    spdk_buffer sbuf1(buffer1, 100);
    spdk_buffer sbuf2(buffer2, 200);

    buffer_list bl1, bl2;
    bl1.append_buffer(sbuf1);
    bl2.append_buffer(sbuf2);

    bl1.append_buffer(bl2);
    FB_ASSERT_EQ(bl1.bytes(), 300);
}

FB_TEST(buffer_list_advanced, move_append) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    buffer_list bl1;
    bl1.append_buffer(sbuf);

    buffer_list bl2;
    bl2.append_buffer(std::move(bl1));
    FB_ASSERT_EQ(bl2.bytes(), 100);
}

FB_TEST(buffer_list_advanced, trim_front_single) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    buffer_list bl;
    bl.append_buffer(sbuf);

    bl.trim_front();
    FB_ASSERT_EQ(bl.bytes(), 0);
}

FB_TEST(buffer_list_advanced, trim_front_multiple) {
    char buffer1[100], buffer2[100], buffer3[100];
    spdk_buffer sbuf1(buffer1, 100);
    spdk_buffer sbuf2(buffer2, 100);
    spdk_buffer sbuf3(buffer3, 100);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    bl.trim_front();
    FB_ASSERT_EQ(bl.bytes(), 200);
}

FB_TEST(buffer_list_advanced, trim_back) {
    char buffer1[100], buffer2[100];
    spdk_buffer sbuf1(buffer1, 100);
    spdk_buffer sbuf2(buffer2, 100);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    bl.trim_back();
    FB_ASSERT_EQ(bl.bytes(), 100);
}

FB_TEST(buffer_list_advanced, iteration) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    buffer_list bl;
    bl.append_buffer(sbuf);

    int count = 0;
    for (auto& buf : bl) {
        (void)buf;  // Suppress unused warning
        count++;
    }
    FB_ASSERT_EQ(count, 1);
}

FB_TEST(buffer_list_advanced, const_iteration) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);
    buffer_list bl;
    bl.append_buffer(sbuf);

    const buffer_list& cbl = bl;
    int count = 0;
    for (const auto& buf : cbl) {
        (void)buf;
        count++;
    }
    FB_ASSERT_EQ(count, 1);
}

// ============================================================================
// Test Suite: xattr_types (Extended Attribute Types Tests)
// ============================================================================

FB_SUITE_SETUP(xattr_types) {
    // Setup code here
}

FB_SUITE_TEARDOWN(xattr_types) {
    // Teardown code here
}

FB_TEST(xattr_types, log_xattr_names) {
    FB_ASSERT_EQ(log_xattr::xattr_count, 3);
}

FB_TEST(xattr_types, object_xattr_names) {
    FB_ASSERT_EQ(object_xattr::xattr_count, 4);
}

FB_TEST(xattr_types, object_snap_xattr_names) {
    FB_ASSERT_EQ(object_snap_xattr::xattr_count, 5);
}

FB_TEST(xattr_types, object_recover_xattr_names) {
    FB_ASSERT_EQ(object_recover_xattr::xattr_count, 4);
}

FB_TEST(xattr_types, kv_xattr_names) {
    FB_ASSERT_EQ(kv_xattr::xattr_count, 2);
}

FB_TEST(xattr_types, kv_checkpoint_xattr_names) {
    FB_ASSERT_EQ(kv_checkpoint_xattr::xattr_count, 2);
}

FB_TEST(xattr_types, kv_checkpoint_new_xattr_names) {
    FB_ASSERT_EQ(kv_checkpoint_new_xattr::xattr_count, 2);
}

FB_TEST(xattr_types, super_xattr_names) {
    FB_ASSERT_EQ(super_xattr::xattr_count, 1);
}

FB_TEST(xattr_types, free_xattr_names) {
    FB_ASSERT_EQ(free_xattr::xattr_count, 1);
}

// ============================================================================
// Test Suite: xattr_structure (Extended Attribute Structure Tests)
// ============================================================================

FB_SUITE_SETUP(xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(xattr_structure) {
    // Teardown code here
}

FB_TEST(xattr_structure, log_xattr_default) {
    log_xattr xattr;
    FB_ASSERT_EQ(xattr.type, blob_type::log);
}

FB_TEST(xattr_structure, log_xattr_values) {
    log_xattr xattr;
    xattr.shard_id = 42;
    xattr.pg = "test_pg";
    FB_ASSERT_EQ(xattr.shard_id, 42);
    FB_ASSERT_EQ(xattr.pg, "test_pg");
}

FB_TEST(xattr_structure, object_xattr_default) {
    object_xattr xattr;
    FB_ASSERT_EQ(xattr.type, blob_type::object);
}

FB_TEST(xattr_structure, object_xattr_values) {
    object_xattr xattr;
    xattr.shard_id = 1;
    xattr.pg = "pg_1";
    xattr.obj_name = "object_1";
    FB_ASSERT_EQ(xattr.shard_id, 1);
    FB_ASSERT_EQ(xattr.pg, "pg_1");
    FB_ASSERT_EQ(xattr.obj_name, "object_1");
}

FB_TEST(xattr_structure, kv_xattr_default) {
    kv_xattr xattr;
    FB_ASSERT_EQ(xattr.type, blob_type::kv);
}

FB_TEST(xattr_structure, kv_xattr_values) {
    kv_xattr xattr;
    xattr.shard_id = 5;
    FB_ASSERT_EQ(xattr.shard_id, 5);
}

// ============================================================================
// Test Suite: serialization (Serialization Tests)
// ============================================================================

FB_SUITE_SETUP(serialization) {
    // Setup code here
}

FB_SUITE_TEARDOWN(serialization) {
    // Teardown code here
}

FB_TEST(serialization, fixed32_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint32_t original = 0x12345678;
    FB_ASSERT_TRUE(PutFixed32(sbuf, original));
    sbuf.reset();

    uint32_t decoded;
    FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));
    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(serialization, fixed64_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint64_t original = 0x123456789ABCDEF0ULL;
    FB_ASSERT_TRUE(PutFixed64(sbuf, original));
    sbuf.reset();

    uint64_t decoded;
    FB_ASSERT_TRUE(GetFixed64(sbuf, decoded));
    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(serialization, string_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original = "hello world";
    FB_ASSERT_TRUE(PutString(sbuf, original));
    sbuf.reset();

    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(serialization, empty_string_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original = "";
    FB_ASSERT_TRUE(PutString(sbuf, original));
    sbuf.reset();

    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(serialization, optional_string_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::optional<std::string> original = "test";
    FB_ASSERT_TRUE(PutOptString(sbuf, original));
    sbuf.reset();

    std::optional<std::string> decoded;
    FB_ASSERT_TRUE(GetOptString(sbuf, decoded));
    FB_ASSERT_TRUE(decoded.has_value());
    FB_ASSERT_EQ(decoded.value(), "test");
}

FB_TEST(serialization, optional_string_nullopt) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::optional<std::string> original = std::nullopt;
    FB_ASSERT_TRUE(PutOptString(sbuf, original));
    sbuf.reset();

    std::optional<std::string> decoded;
    FB_ASSERT_TRUE(GetOptString(sbuf, decoded));
    FB_ASSERT_FALSE(decoded.has_value());
}

FB_TEST(serialization, length_calculation) {
    std::string str = "hello";
    uint64_t len = LengthString(str);
    FB_ASSERT_EQ(len, sizeof(uint64_t) + 5);

    std::optional<std::string> opt_str = "world";
    uint64_t opt_len = LengthOptString(opt_str);
    FB_ASSERT_EQ(opt_len, sizeof(uint64_t) + 5);

    std::optional<std::string> empty_opt = std::nullopt;
    uint64_t empty_len = LengthOptString(empty_opt);
    FB_ASSERT_EQ(empty_len, sizeof(uint64_t));
}

// ============================================================================
// Test Suite: encoding_edge_cases (Encoding Edge Cases Tests)
// ============================================================================

FB_SUITE_SETUP(encoding_edge_cases) {
    // Setup code here
}

FB_SUITE_TEARDOWN(encoding_edge_cases) {
    // Teardown code here
}

FB_TEST(encoding_edge_cases, buffer_insufficient) {
    char buffer[1];  // Too small
    spdk_buffer sbuf(buffer, 1);

    uint32_t val = 0x12345678;
    FB_ASSERT_FALSE(PutFixed32(sbuf, val));
}

FB_TEST(encoding_edge_cases, buffer_insufficient_64) {
    char buffer[4];  // Too small for 64-bit
    spdk_buffer sbuf(buffer, 4);

    uint64_t val = 0x123456789ABCDEF0ULL;
    FB_ASSERT_FALSE(PutFixed64(sbuf, val));
}

FB_TEST(encoding_edge_cases, buffer_insufficient_string) {
    char buffer[5];  // Too small
    spdk_buffer sbuf(buffer, 5);

    std::string str = "hello world";  // 11 chars
    FB_ASSERT_FALSE(PutString(sbuf, str));
}

FB_TEST(encoding_edge_cases, multiple_values) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 2));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 3));
    FB_ASSERT_EQ(sbuf.used(), 12);
}

FB_TEST(encoding_edge_cases, mixed_types) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 100));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 1000));
    FB_ASSERT_TRUE(PutString(sbuf, "test"));

    FB_ASSERT_TRUE(sbuf.used() > 0);
}

FB_TEST(encoding_edge_cases, reset_and_reuse) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 123));
    sbuf.reset();

    FB_ASSERT_TRUE(PutFixed32(sbuf, 456));
    sbuf.reset();

    FB_ASSERT_TRUE(PutFixed64(sbuf, 789));
    FB_ASSERT_EQ(sbuf.used(), 8);
}

// ============================================================================
// Test Suite: log_entry_serialization (Log Entry Serialization Tests)
// ============================================================================

FB_SUITE_SETUP(log_entry_serialization) {
    // Setup code here
}

FB_SUITE_TEARDOWN(log_entry_serialization) {
    // Teardown code here
}

FB_TEST(log_entry_serialization, encode_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    log_entry_t entry;
    entry.term_id = 1;
    entry.index = 10;
    entry.size = 256;
    entry.type = 0;
    entry.meta = "test";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
}

FB_TEST(log_entry_serialization, encode_decode_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    log_entry_t original;
    original.term_id = 5;
    original.index = 100;
    original.size = 512;
    original.type = 1;
    original.meta = "metadata";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, original));
    sbuf.reset();

    log_entry_t decoded;
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));
    FB_ASSERT_EQ(decoded.term_id, original.term_id);
    FB_ASSERT_EQ(decoded.index, original.index);
    FB_ASSERT_EQ(decoded.size, original.size);
    FB_ASSERT_EQ(decoded.type, original.type);
    FB_ASSERT_EQ(decoded.meta, original.meta);
}

FB_TEST(log_entry_serialization, encode_multiple) {
    char buffer[500];
    spdk_buffer sbuf(buffer, 500);

    for (int i = 0; i < 5; i++) {
        log_entry_t entry;
        entry.term_id = i;
        entry.index = i * 10;
        entry.size = i * 100;
        entry.type = 0;
        entry.meta = "entry_" + std::to_string(i);

        FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    }

    FB_ASSERT_TRUE(sbuf.used() > 0);
}

FB_TEST(log_entry_serialization, empty_meta) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    log_entry_t entry;
    entry.term_id = 1;
    entry.index = 1;
    entry.size = 0;
    entry.type = 0;
    entry.meta = "";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    sbuf.reset();

    log_entry_t decoded;
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));
    FB_ASSERT_EQ(decoded.meta, "");
}

FB_TEST(log_entry_serialization, long_meta) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    log_entry_t entry;
    entry.term_id = 1;
    entry.index = 1;
    entry.size = 0;
    entry.type = 0;
    entry.meta = "this is a very long metadata string for testing purposes";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
}

// ============================================================================
// Test Suite: log_xattr_structure (Log Xattr Structure Tests)
// ============================================================================

FB_SUITE_SETUP(log_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(log_xattr_structure) {
    // Teardown code here
}

FB_TEST(log_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(log_xattr::xattr_count, 3);
}

FB_TEST(log_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(log_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(log_xattr_structure, xattr_names_shard) {
    FB_ASSERT_EQ(strcmp(log_xattr::xattr_names[1], "shard"), 0);
}

FB_TEST(log_xattr_structure, xattr_names_pg) {
    FB_ASSERT_EQ(strcmp(log_xattr::xattr_names[2], "pg"), 0);
}

FB_TEST(log_xattr_structure, type_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(log_xattr::type), 0);
}

FB_TEST(log_xattr_structure, default_shard_id) {
    log_xattr xattr;
    FB_ASSERT_EQ(xattr.shard_id, 0u);
}

FB_TEST(log_xattr_structure, shard_id_assignment) {
    log_xattr xattr;
    xattr.shard_id = 42;
    FB_ASSERT_EQ(xattr.shard_id, 42u);
}

FB_TEST(log_xattr_structure, pg_default_empty) {
    log_xattr xattr;
    FB_ASSERT_TRUE(xattr.pg.empty());
}

FB_TEST(log_xattr_structure, pg_string_assignment) {
    log_xattr xattr;
    xattr.pg = "pool1.pg42";
    FB_ASSERT_EQ(xattr.pg, "pool1.pg42");
}

FB_TEST(log_xattr_structure, pg_string_size) {
    log_xattr xattr;
    xattr.pg = "test_pg";
    FB_ASSERT_EQ(xattr.pg.size(), 7);
}

// ============================================================================
// Test Suite: object_xattr_structure (Object Xattr Structure Tests)
// ============================================================================

FB_SUITE_SETUP(object_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(object_xattr_structure) {
    // Teardown code here
}

FB_TEST(object_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(object_xattr::xattr_count, 4);
}

FB_TEST(object_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(object_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(object_xattr_structure, xattr_names_shard) {
    FB_ASSERT_EQ(strcmp(object_xattr::xattr_names[1], "shard"), 0);
}

FB_TEST(object_xattr_structure, xattr_names_pg) {
    FB_ASSERT_EQ(strcmp(object_xattr::xattr_names[2], "pg"), 0);
}

FB_TEST(object_xattr_structure, xattr_names_name) {
    FB_ASSERT_EQ(strcmp(object_xattr::xattr_names[3], "name"), 0);
}

FB_TEST(object_xattr_structure, type_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(object_xattr::type), 1);
}

FB_TEST(object_xattr_structure, default_shard_id) {
    object_xattr xattr;
    FB_ASSERT_EQ(xattr.shard_id, 0u);
}

FB_TEST(object_xattr_structure, shard_id_assignment) {
    object_xattr xattr;
    xattr.shard_id = 123;
    FB_ASSERT_EQ(xattr.shard_id, 123u);
}

FB_TEST(object_xattr_structure, pg_default_empty) {
    object_xattr xattr;
    FB_ASSERT_TRUE(xattr.pg.empty());
}

FB_TEST(object_xattr_structure, pg_string_assignment) {
    object_xattr xattr;
    xattr.pg = "pool2.pg100";
    FB_ASSERT_EQ(xattr.pg, "pool2.pg100");
}

FB_TEST(object_xattr_structure, obj_name_default_empty) {
    object_xattr xattr;
    FB_ASSERT_TRUE(xattr.obj_name.empty());
}

FB_TEST(object_xattr_structure, obj_name_assignment) {
    object_xattr xattr;
    xattr.obj_name = "object_123";
    FB_ASSERT_EQ(xattr.obj_name, "object_123");
}

FB_TEST(object_xattr_structure, obj_name_size) {
    object_xattr xattr;
    xattr.obj_name = "test_object";
    FB_ASSERT_EQ(xattr.obj_name.size(), 11);
}

FB_TEST(object_xattr_structure, all_fields_assignment) {
    object_xattr xattr;
    xattr.shard_id = 5;
    xattr.pg = "pool.pg";
    xattr.obj_name = "obj";
    FB_ASSERT_EQ(xattr.shard_id, 5u);
    FB_ASSERT_EQ(xattr.pg, "pool.pg");
    FB_ASSERT_EQ(xattr.obj_name, "obj");
}

// ============================================================================
// Test Suite: object_snap_xattr_structure (Object Snap Xattr Structure Tests)
// ============================================================================

FB_SUITE_SETUP(object_snap_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(object_snap_xattr_structure) {
    // Teardown code here
}

FB_TEST(object_snap_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(object_snap_xattr::xattr_count, 5);
}

FB_TEST(object_snap_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(object_snap_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(object_snap_xattr_structure, xattr_names_shard) {
    FB_ASSERT_EQ(strcmp(object_snap_xattr::xattr_names[1], "shard"), 0);
}

FB_TEST(object_snap_xattr_structure, xattr_names_pg) {
    FB_ASSERT_EQ(strcmp(object_snap_xattr::xattr_names[2], "pg"), 0);
}

FB_TEST(object_snap_xattr_structure, xattr_names_name) {
    FB_ASSERT_EQ(strcmp(object_snap_xattr::xattr_names[3], "name"), 0);
}

FB_TEST(object_snap_xattr_structure, xattr_names_snap_name) {
    FB_ASSERT_EQ(strcmp(object_snap_xattr::xattr_names[4], "snap_name"), 0);
}

FB_TEST(object_snap_xattr_structure, type_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(object_snap_xattr::type), 2);
}

FB_TEST(object_snap_xattr_structure, default_shard_id) {
    object_snap_xattr xattr;
    FB_ASSERT_EQ(xattr.shard_id, 0u);
}

FB_TEST(object_snap_xattr_structure, shard_id_assignment) {
    object_snap_xattr xattr;
    xattr.shard_id = 99;
    FB_ASSERT_EQ(xattr.shard_id, 99u);
}

FB_TEST(object_snap_xattr_structure, pg_default_empty) {
    object_snap_xattr xattr;
    FB_ASSERT_TRUE(xattr.pg.empty());
}

FB_TEST(object_snap_xattr_structure, pg_string_assignment) {
    object_snap_xattr xattr;
    xattr.pg = "pool3.pg50";
    FB_ASSERT_EQ(xattr.pg, "pool3.pg50");
}

FB_TEST(object_snap_xattr_structure, obj_name_default_empty) {
    object_snap_xattr xattr;
    FB_ASSERT_TRUE(xattr.obj_name.empty());
}

FB_TEST(object_snap_xattr_structure, obj_name_assignment) {
    object_snap_xattr xattr;
    xattr.obj_name = "snap_object_1";
    FB_ASSERT_EQ(xattr.obj_name, "snap_object_1");
}

FB_TEST(object_snap_xattr_structure, snap_name_default_empty) {
    object_snap_xattr xattr;
    FB_ASSERT_TRUE(xattr.snap_name.empty());
}

FB_TEST(object_snap_xattr_structure, snap_name_assignment) {
    object_snap_xattr xattr;
    xattr.snap_name = "snapshot_20240101";
    FB_ASSERT_EQ(xattr.snap_name, "snapshot_20240101");
}

FB_TEST(object_snap_xattr_structure, all_fields_assignment) {
    object_snap_xattr xattr;
    xattr.shard_id = 10;
    xattr.pg = "pool.pg";
    xattr.obj_name = "obj";
    xattr.snap_name = "snap";
    FB_ASSERT_EQ(xattr.shard_id, 10u);
    FB_ASSERT_EQ(xattr.pg, "pool.pg");
    FB_ASSERT_EQ(xattr.obj_name, "obj");
    FB_ASSERT_EQ(xattr.snap_name, "snap");
}

// ============================================================================
// Test Suite: object_recover_xattr_structure (Object Recover Xattr Tests)
// ============================================================================

FB_SUITE_SETUP(object_recover_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(object_recover_xattr_structure) {
    // Teardown code here
}

FB_TEST(object_recover_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(object_recover_xattr::xattr_count, 4);
}

FB_TEST(object_recover_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(object_recover_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(object_recover_xattr_structure, xattr_names_shard) {
    FB_ASSERT_EQ(strcmp(object_recover_xattr::xattr_names[1], "shard"), 0);
}

FB_TEST(object_recover_xattr_structure, xattr_names_pg) {
    FB_ASSERT_EQ(strcmp(object_recover_xattr::xattr_names[2], "pg"), 0);
}

FB_TEST(object_recover_xattr_structure, xattr_names_name) {
    FB_ASSERT_EQ(strcmp(object_recover_xattr::xattr_names[3], "name"), 0);
}

FB_TEST(object_recover_xattr_structure, type_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(object_recover_xattr::type), 3);
}

FB_TEST(object_recover_xattr_structure, default_shard_id) {
    object_recover_xattr xattr{};
    FB_ASSERT_EQ(xattr.shard_id, 0u);
}

FB_TEST(object_recover_xattr_structure, shard_id_assignment) {
    object_recover_xattr xattr;
    xattr.shard_id = 255;
    FB_ASSERT_EQ(xattr.shard_id, 255u);
}

FB_TEST(object_recover_xattr_structure, pg_default_empty) {
    object_recover_xattr xattr;
    FB_ASSERT_TRUE(xattr.pg.empty());
}

FB_TEST(object_recover_xattr_structure, pg_string_assignment) {
    object_recover_xattr xattr;
    xattr.pg = "recovery_pool.pg1";
    FB_ASSERT_EQ(xattr.pg, "recovery_pool.pg1");
}

FB_TEST(object_recover_xattr_structure, obj_name_default_empty) {
    object_recover_xattr xattr;
    FB_ASSERT_TRUE(xattr.obj_name.empty());
}

FB_TEST(object_recover_xattr_structure, obj_name_assignment) {
    object_recover_xattr xattr;
    xattr.obj_name = "recovered_object";
    FB_ASSERT_EQ(xattr.obj_name, "recovered_object");
}

FB_TEST(object_recover_xattr_structure, all_fields_assignment) {
    object_recover_xattr xattr;
    xattr.shard_id = 7;
    xattr.pg = "pool.pg";
    xattr.obj_name = "recovery_obj";
    FB_ASSERT_EQ(xattr.shard_id, 7u);
    FB_ASSERT_EQ(xattr.pg, "pool.pg");
    FB_ASSERT_EQ(xattr.obj_name, "recovery_obj");
}

// ============================================================================
// Test Suite: kv_xattr_structure (KV Xattr Structure Tests)
// ============================================================================

FB_SUITE_SETUP(kv_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(kv_xattr_structure) {
    // Teardown code here
}

FB_TEST(kv_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(kv_xattr::xattr_count, 2);
}

FB_TEST(kv_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(kv_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(kv_xattr_structure, xattr_names_shard) {
    FB_ASSERT_EQ(strcmp(kv_xattr::xattr_names[1], "shard"), 0);
}

FB_TEST(kv_xattr_structure, type_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(kv_xattr::type), 4);
}

FB_TEST(kv_xattr_structure, default_shard_id) {
    kv_xattr xattr{};
    FB_ASSERT_EQ(xattr.shard_id, 0u);
}

FB_TEST(kv_xattr_structure, shard_id_assignment) {
    kv_xattr xattr;
    xattr.shard_id = 77;
    FB_ASSERT_EQ(xattr.shard_id, 77u);
}

FB_TEST(kv_xattr_structure, shard_id_boundary) {
    kv_xattr xattr;
    xattr.shard_id = std::numeric_limits<uint32_t>::max();
    FB_ASSERT_EQ(xattr.shard_id, std::numeric_limits<uint32_t>::max());
}

// ============================================================================
// Test Suite: kv_checkpoint_xattr_structure (KV Checkpoint Xattr Tests)
// ============================================================================

FB_SUITE_SETUP(kv_checkpoint_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(kv_checkpoint_xattr_structure) {
    // Teardown code here
}

FB_TEST(kv_checkpoint_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(kv_checkpoint_xattr::xattr_count, 2);
}

FB_TEST(kv_checkpoint_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(kv_checkpoint_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(kv_checkpoint_xattr_structure, xattr_names_shard) {
    FB_ASSERT_EQ(strcmp(kv_checkpoint_xattr::xattr_names[1], "shard"), 0);
}

FB_TEST(kv_checkpoint_xattr_structure, type_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(kv_checkpoint_xattr::type), 5);
}

FB_TEST(kv_checkpoint_xattr_structure, default_shard_id) {
    kv_checkpoint_xattr xattr{};
    FB_ASSERT_EQ(xattr.shard_id, 0u);
}

FB_TEST(kv_checkpoint_xattr_structure, shard_id_assignment) {
    kv_checkpoint_xattr xattr;
    xattr.shard_id = 88;
    FB_ASSERT_EQ(xattr.shard_id, 88u);
}

// ============================================================================
// Test Suite: kv_checkpoint_new_xattr_structure (KV Checkpoint New Xattr Tests)
// ============================================================================

FB_SUITE_SETUP(kv_checkpoint_new_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(kv_checkpoint_new_xattr_structure) {
    // Teardown code here
}

FB_TEST(kv_checkpoint_new_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(kv_checkpoint_new_xattr::xattr_count, 2);
}

FB_TEST(kv_checkpoint_new_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(kv_checkpoint_new_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(kv_checkpoint_new_xattr_structure, xattr_names_shard) {
    FB_ASSERT_EQ(strcmp(kv_checkpoint_new_xattr::xattr_names[1], "shard"), 0);
}

FB_TEST(kv_checkpoint_new_xattr_structure, type_value) {
    FB_ASSERT_EQ(static_cast<uint32_t>(kv_checkpoint_new_xattr::type), 6);
}

FB_TEST(kv_checkpoint_new_xattr_structure, default_shard_id) {
    kv_checkpoint_new_xattr xattr{};
    FB_ASSERT_EQ(xattr.shard_id, 0u);
}

FB_TEST(kv_checkpoint_new_xattr_structure, shard_id_assignment) {
    kv_checkpoint_new_xattr xattr;
    xattr.shard_id = 99;
    FB_ASSERT_EQ(xattr.shard_id, 99u);
}

// ============================================================================
// Test Suite: super_xattr_structure (Super Blob Xattr Structure Tests)
// ============================================================================

FB_SUITE_SETUP(super_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(super_xattr_structure) {
    // Teardown code here
}

FB_TEST(super_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(super_xattr::xattr_count, 1);
}

FB_TEST(super_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(super_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(super_xattr_structure, default_construct) {
    super_xattr xattr{};
    // Verify structure can be default constructed and type is correct
    FB_ASSERT_EQ(static_cast<uint32_t>(xattr.type), 7u);
}

// ============================================================================
// Test Suite: free_xattr_structure (Free Blob Xattr Structure Tests)
// ============================================================================

FB_SUITE_SETUP(free_xattr_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(free_xattr_structure) {
    // Teardown code here
}

FB_TEST(free_xattr_structure, xattr_names_count) {
    FB_ASSERT_EQ(free_xattr::xattr_count, 1);
}

FB_TEST(free_xattr_structure, xattr_names_type) {
    FB_ASSERT_EQ(strcmp(free_xattr::xattr_names[0], "type"), 0);
}

FB_TEST(free_xattr_structure, default_construct) {
    free_xattr xattr{};
    // Verify structure can be default constructed and type is correct
    FB_ASSERT_EQ(static_cast<uint32_t>(xattr.type), 8u);
}

// ============================================================================
// Test Suite: fb_blob_structure (FB Blob Structure Tests)
// ============================================================================

FB_SUITE_SETUP(fb_blob_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fb_blob_structure) {
    // Teardown code here
}

FB_TEST(fb_blob_structure, default_blob_null) {
    fb_blob blob{};
    FB_ASSERT_TRUE(blob.blob == nullptr);
}

FB_TEST(fb_blob_structure, default_blobid_zero) {
    fb_blob blob{};
    FB_ASSERT_EQ(blob.blobid, 0ull);
}

FB_TEST(fb_blob_structure, blobid_assignment) {
    fb_blob blob{};
    blob.blobid = 12345;
    FB_ASSERT_EQ(blob.blobid, 12345ull);
}

FB_TEST(fb_blob_structure, blobid_large_value) {
    fb_blob blob{};
    blob.blobid = 0xFFFFFFFFFFFFFFFFULL;
    FB_ASSERT_EQ(blob.blobid, 0xFFFFFFFFFFFFFFFFULL);
}

FB_TEST(fb_blob_structure, blob_pointer_default) {
    fb_blob blob{};
    FB_ASSERT_EQ(blob.blob, nullptr);
}

FB_TEST(fb_blob_structure, copy_blob) {
    fb_blob blob1{};
    blob1.blobid = 999;
    fb_blob blob2 = blob1;
    FB_ASSERT_EQ(blob2.blobid, 999ull);
}

// ============================================================================
// Test Suite: blob_type_mapping (Blob Type to Xattr Mapping Tests)
// ============================================================================

FB_SUITE_SETUP(blob_type_mapping) {
    // Setup code here
}

FB_SUITE_TEARDOWN(blob_type_mapping) {
    // Teardown code here
}

FB_TEST(blob_type_mapping, log_maps_to_log_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::log),
                 static_cast<uint32_t>(log_xattr::type));
}

FB_TEST(blob_type_mapping, object_maps_to_object_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object),
                 static_cast<uint32_t>(object_xattr::type));
}

FB_TEST(blob_type_mapping, object_snap_maps_to_snap_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_snap),
                 static_cast<uint32_t>(object_snap_xattr::type));
}

FB_TEST(blob_type_mapping, object_recover_maps_to_recover_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_recover),
                 static_cast<uint32_t>(object_recover_xattr::type));
}

FB_TEST(blob_type_mapping, kv_maps_to_kv_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv),
                 static_cast<uint32_t>(kv_xattr::type));
}

FB_TEST(blob_type_mapping, kv_checkpoint_maps_to_checkpoint_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint),
                 static_cast<uint32_t>(kv_checkpoint_xattr::type));
}

FB_TEST(blob_type_mapping, kv_checkpoint_new_maps_to_new_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint_new),
                 static_cast<uint32_t>(kv_checkpoint_new_xattr::type));
}

FB_TEST(blob_type_mapping, super_blob_maps_to_super_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::super_blob),
                 static_cast<uint32_t>(super_xattr::type));
}

FB_TEST(blob_type_mapping, free_maps_to_free_xattr) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::free),
                 static_cast<uint32_t>(free_xattr::type));
}

// ============================================================================
// Test Suite: fixed32_serialization (Fixed32 Serialization Tests)
// ============================================================================

FB_SUITE_SETUP(fixed32_serialization) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed32_serialization) {
    // Teardown code here
}

FB_TEST(fixed32_serialization, put_and_get_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint32_t value = 0x12345678;
    FB_ASSERT_TRUE(PutFixed32(sbuf, value));

    sbuf.reset();
    uint32_t decoded = 0;
    FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(fixed32_serialization, put_zero) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 0u));

    sbuf.reset();
    uint32_t decoded = 1;
    FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));
    FB_ASSERT_EQ(decoded, 0u);
}

FB_TEST(fixed32_serialization, put_max_value) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint32_t value = 0xFFFFFFFF;
    FB_ASSERT_TRUE(PutFixed32(sbuf, value));

    sbuf.reset();
    uint32_t decoded = 0;
    FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(fixed32_serialization, insufficient_space_put) {
    char buffer[2];
    spdk_buffer sbuf(buffer, 2);

    uint32_t value = 123;
    FB_ASSERT_FALSE(PutFixed32(sbuf, value));
}

FB_TEST(fixed32_serialization, insufficient_space_get) {
    char buffer[2];
    spdk_buffer sbuf(buffer, 2);

    uint32_t decoded = 0;
    FB_ASSERT_FALSE(GetFixed32(sbuf, decoded));
}

FB_TEST(fixed32_serialization, multiple_values) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 2u));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 3u));

    sbuf.reset();
    uint32_t v1, v2, v3;
    FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
    FB_ASSERT_TRUE(GetFixed32(sbuf, v2));
    FB_ASSERT_TRUE(GetFixed32(sbuf, v3));

    FB_ASSERT_EQ(v1, 1u);
    FB_ASSERT_EQ(v2, 2u);
    FB_ASSERT_EQ(v3, 3u);
}

FB_TEST(fixed32_serialization, buffer_usage) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 12345));
    FB_ASSERT_EQ(sbuf.used(), 4u);
}

// ============================================================================
// Test Suite: fixed64_serialization (Fixed64 Serialization Tests)
// ============================================================================

FB_SUITE_SETUP(fixed64_serialization) {
    // Setup code here
}

FB_SUITE_TEARDOWN(fixed64_serialization) {
    // Teardown code here
}

FB_TEST(fixed64_serialization, put_and_get_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint64_t value = 0x123456789ABCDEF0ULL;
    FB_ASSERT_TRUE(PutFixed64(sbuf, value));

    sbuf.reset();
    uint64_t decoded = 0;
    FB_ASSERT_TRUE(GetFixed64(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(fixed64_serialization, put_zero) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed64(sbuf, 0ull));

    sbuf.reset();
    uint64_t decoded = 1;
    FB_ASSERT_TRUE(GetFixed64(sbuf, decoded));
    FB_ASSERT_EQ(decoded, 0ull);
}

FB_TEST(fixed64_serialization, put_max_value) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint64_t value = 0xFFFFFFFFFFFFFFFFULL;
    FB_ASSERT_TRUE(PutFixed64(sbuf, value));

    sbuf.reset();
    uint64_t decoded = 0;
    FB_ASSERT_TRUE(GetFixed64(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(fixed64_serialization, insufficient_space_put) {
    char buffer[4];
    spdk_buffer sbuf(buffer, 4);

    uint64_t value = 123;
    FB_ASSERT_FALSE(PutFixed64(sbuf, value));
}

FB_TEST(fixed64_serialization, insufficient_space_get) {
    char buffer[4];
    spdk_buffer sbuf(buffer, 4);

    uint64_t decoded = 0;
    FB_ASSERT_FALSE(GetFixed64(sbuf, decoded));
}

FB_TEST(fixed64_serialization, buffer_usage) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed64(sbuf, 12345));
    FB_ASSERT_EQ(sbuf.used(), 8u);
}

// ============================================================================
// Test Suite: string_serialization (String Serialization Tests)
// ============================================================================

FB_SUITE_SETUP(string_serialization) {
    // Setup code here
}

FB_SUITE_TEARDOWN(string_serialization) {
    // Teardown code here
}

FB_TEST(string_serialization, put_and_get_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string value = "hello world";
    FB_ASSERT_TRUE(PutString(sbuf, value));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(string_serialization, empty_string) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string value;
    FB_ASSERT_TRUE(PutString(sbuf, value));

    sbuf.reset();
    std::string decoded = "not_empty";
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_TRUE(decoded.empty());
}

FB_TEST(string_serialization, long_string) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    std::string value(500, 'a');
    FB_ASSERT_TRUE(PutString(sbuf, value));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
    FB_ASSERT_EQ(decoded.size(), 500);
}

FB_TEST(string_serialization, binary_data) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string value = "\x00\x01\x02\x03";
    FB_ASSERT_TRUE(PutString(sbuf, value));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(string_serialization, insufficient_space_put) {
    char buffer[5];
    spdk_buffer sbuf(buffer, 5);

    std::string value = "hello";
    FB_ASSERT_FALSE(PutString(sbuf, value));
}

FB_TEST(string_serialization, buffer_usage_empty) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string value;
    FB_ASSERT_TRUE(PutString(sbuf, value));
    FB_ASSERT_EQ(sbuf.used(), 8u);  // Only size field
}

FB_TEST(string_serialization, buffer_usage_non_empty) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string value = "test";
    FB_ASSERT_TRUE(PutString(sbuf, value));
    FB_ASSERT_EQ(sbuf.used(), 12u);  // 8 for size + 4 for data
}

FB_TEST(string_serialization, multiple_strings) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string s1 = "first";
    std::string s2 = "second";
    std::string s3 = "third";

    FB_ASSERT_TRUE(PutString(sbuf, s1));
    FB_ASSERT_TRUE(PutString(sbuf, s2));
    FB_ASSERT_TRUE(PutString(sbuf, s3));

    sbuf.reset();

    std::string d1, d2, d3;
    FB_ASSERT_TRUE(GetString(sbuf, d1));
    FB_ASSERT_TRUE(GetString(sbuf, d2));
    FB_ASSERT_TRUE(GetString(sbuf, d3));

    FB_ASSERT_EQ(d1, s1);
    FB_ASSERT_EQ(d2, s2);
    FB_ASSERT_EQ(d3, s3);
}

// ============================================================================
// Test Suite: optional_string_serialization (Optional String Tests)
// ============================================================================

FB_SUITE_SETUP(optional_string_serialization) {
    // Setup code here
}

FB_SUITE_TEARDOWN(optional_string_serialization) {
    // Teardown code here
}

FB_TEST(optional_string_serialization, has_value) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::optional<std::string> value = "test_value";
    FB_ASSERT_TRUE(PutOptString(sbuf, value));

    sbuf.reset();
    std::optional<std::string> decoded;
    FB_ASSERT_TRUE(GetOptString(sbuf, decoded));
    FB_ASSERT_TRUE(decoded.has_value());
    FB_ASSERT_EQ(*decoded, "test_value");
}

FB_TEST(optional_string_serialization, no_value) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::optional<std::string> value = std::nullopt;
    FB_ASSERT_TRUE(PutOptString(sbuf, value));

    sbuf.reset();
    std::optional<std::string> decoded = "old";
    FB_ASSERT_TRUE(GetOptString(sbuf, decoded));
    FB_ASSERT_FALSE(decoded.has_value());
}

FB_TEST(optional_string_serialization, empty_string_value) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::optional<std::string> value = "";
    FB_ASSERT_TRUE(PutOptString(sbuf, value));

    sbuf.reset();
    std::optional<std::string> decoded;
    FB_ASSERT_TRUE(GetOptString(sbuf, decoded));
    // Empty optional string is serialized as nullopt
    FB_ASSERT_FALSE(decoded.has_value());
}

// ============================================================================
// Test Suite: string_length_calculation (String Length Tests)
// ============================================================================

FB_SUITE_SETUP(string_length_calculation) {
    // Setup code here
}

FB_SUITE_TEARDOWN(string_length_calculation) {
    // Teardown code here
}

FB_TEST(string_length_calculation, empty_string) {
    std::string value;
    FB_ASSERT_EQ(LengthString(value), 8u);
}

FB_TEST(string_length_calculation, single_char) {
    std::string value = "a";
    FB_ASSERT_EQ(LengthString(value), 9u);
}

FB_TEST(string_length_calculation, typical_string) {
    std::string value = "hello";
    FB_ASSERT_EQ(LengthString(value), 13u);
}

FB_TEST(string_length_calculation, long_string) {
    std::string value(1000, 'x');
    FB_ASSERT_EQ(LengthString(value), 1008u);
}

FB_TEST(string_length_calculation, exact_size_match) {
    std::string value(92, 'y');  // 92 + 8 = 100
    FB_ASSERT_EQ(LengthString(value), 100u);
}

// ============================================================================
// Test Suite: optional_string_length_calculation (Optional String Length Tests)
// ============================================================================

FB_SUITE_SETUP(optional_string_length_calculation) {
    // Setup code here
}

FB_SUITE_TEARDOWN(optional_string_length_calculation) {
    // Teardown code here
}

FB_TEST(optional_string_length_calculation, has_value) {
    std::optional<std::string> value = "test";
    FB_ASSERT_EQ(LengthOptString(value), 12u);
}

FB_TEST(optional_string_length_calculation, no_value) {
    std::optional<std::string> value = std::nullopt;
    FB_ASSERT_EQ(LengthOptString(value), 8u);
}

FB_TEST(optional_string_length_calculation, empty_value) {
    std::optional<std::string> value = "";
    FB_ASSERT_EQ(LengthOptString(value), 8u);
}

FB_TEST(optional_string_length_calculation, long_value) {
    std::optional<std::string> value(std::string(500, 'z'));
    FB_ASSERT_EQ(LengthOptString(value), 508u);
}

// ============================================================================
// Test Suite: xattr_val_type_variant (Xattr Variant Type Tests)
// ============================================================================

FB_SUITE_SETUP(xattr_val_type_variant) {
    // Setup code here
}

FB_SUITE_TEARDOWN(xattr_val_type_variant) {
    // Teardown code here
}

FB_TEST(xattr_val_type_variant, holds_blob_type) {
    xattr_val_type val = blob_type::log;
    FB_ASSERT_TRUE(std::holds_alternative<blob_type>(val));
    FB_ASSERT_EQ(std::get<blob_type>(val), blob_type::log);
}

FB_TEST(xattr_val_type_variant, holds_uint32) {
    xattr_val_type val = 12345u;
    FB_ASSERT_TRUE(std::holds_alternative<uint32_t>(val));
    FB_ASSERT_EQ(std::get<uint32_t>(val), 12345u);
}

FB_TEST(xattr_val_type_variant, holds_string) {
    xattr_val_type val = std::string("test");
    FB_ASSERT_TRUE(std::holds_alternative<std::string>(val));
    FB_ASSERT_EQ(std::get<std::string>(val), "test");
}

FB_TEST(xattr_val_type_variant, assignment_blob_type) {
    xattr_val_type val;
    val = blob_type::object;
    FB_ASSERT_EQ(std::get<blob_type>(val), blob_type::object);
}

FB_TEST(xattr_val_type_variant, assignment_uint32) {
    xattr_val_type val;
    val = 999u;
    FB_ASSERT_EQ(std::get<uint32_t>(val), 999u);
}

FB_TEST(xattr_val_type_variant, assignment_string) {
    xattr_val_type val;
    val = std::string("assigned");
    FB_ASSERT_EQ(std::get<std::string>(val), "assigned");
}

FB_TEST(xattr_val_type_variant, reassignment_different_types) {
    xattr_val_type val;
    val = blob_type::kv;
    FB_ASSERT_TRUE(std::holds_alternative<blob_type>(val));

    val = 42u;
    FB_ASSERT_TRUE(std::holds_alternative<uint32_t>(val));
    FB_ASSERT_EQ(std::get<uint32_t>(val), 42u);

    val = std::string("changed");
    FB_ASSERT_TRUE(std::holds_alternative<std::string>(val));
    FB_ASSERT_EQ(std::get<std::string>(val), "changed");
}

// ============================================================================
// Test Suite: set_xattr_ctx_structure (Set Xattr Context Tests)
// ============================================================================

FB_SUITE_SETUP(set_xattr_ctx_structure) {
    // Setup code here
}

FB_SUITE_TEARDOWN(set_xattr_ctx_structure) {
    // Teardown code here
}

FB_TEST(set_xattr_ctx_structure, default_construct) {
    set_xattr_ctx xattr_ctx{};
    FB_ASSERT_TRUE(xattr_ctx.cb_fn == nullptr);
    FB_ASSERT_TRUE(xattr_ctx.arg == nullptr);
}

FB_TEST(set_xattr_ctx_structure, assignment) {
    set_xattr_ctx xattr_ctx;
    xattr_ctx.cb_fn = [](void*, int) {};
    xattr_ctx.arg = nullptr;

    FB_ASSERT_TRUE(xattr_ctx.cb_fn != nullptr);
    FB_ASSERT_TRUE(xattr_ctx.arg == nullptr);
}

FB_TEST(set_xattr_ctx_structure, with_arg) {
    int dummy = 42;
    set_xattr_ctx xattr_ctx;
    xattr_ctx.arg = &dummy;

    FB_ASSERT_EQ(xattr_ctx.arg, &dummy);
}

// ============================================================================
// Test Suite: rblob_xattr_complete_callback (Xattr Complete Callback Tests)
// ============================================================================

FB_SUITE_SETUP(rblob_xattr_complete_callback) {
    // Setup code here
}

FB_SUITE_TEARDOWN(rblob_xattr_complete_callback) {
    // Teardown code here
}

FB_TEST(rblob_xattr_complete_callback, callback_signature) {
    // Verify callback type exists and can be assigned
    rblob_xattr_complete cb = [](void* arg, int rc) {};
    FB_ASSERT_TRUE(cb != nullptr);
}

FB_TEST(rblob_xattr_complete_callback, callback_invocation) {
    int result = 0;
    rblob_xattr_complete cb = [](void* arg, int rc) {
        int* out = static_cast<int*>(arg);
        *out = rc;
    };

    cb(&result, 42);
    FB_ASSERT_EQ(result, 42);
}

FB_TEST(rblob_xattr_complete_callback, null_arg) {
    int call_count = 0;
    rblob_xattr_complete cb = [&call_count](void* arg, int rc) {
        call_count++;
    };

    cb(nullptr, 0);
    // Verify callback was actually invoked
    FB_ASSERT_EQ(call_count, 1);
}

FB_TEST(rblob_xattr_complete_callback, error_code_zero) {
    int result = -1;
    rblob_xattr_complete cb = [](void* arg, int rc) {
        int* out = static_cast<int*>(arg);
        *out = rc;
    };

    cb(&result, 0);  // Success
    FB_ASSERT_EQ(result, 0);
}

FB_TEST(rblob_xattr_complete_callback, error_code_nonzero) {
    int result = 0;
    rblob_xattr_complete cb = [](void* arg, int rc) {
        int* out = static_cast<int*>(arg);
        *out = rc;
    };

    cb(&result, -1);  // Error
    FB_ASSERT_EQ(result, -1);
}

// ============================================================================
// Test Suite: blob_type_string_output (Blob Type String Output Tests)
// ============================================================================

FB_SUITE_SETUP(blob_type_string_output) {
    // Setup code here
}

FB_SUITE_TEARDOWN(blob_type_string_output) {
    // Teardown code here
}

FB_TEST(blob_type_string_output, all_types_unique) {
    std::set<std::string> strings;

    strings.insert(type_string(blob_type::log));
    strings.insert(type_string(blob_type::object));
    strings.insert(type_string(blob_type::object_snap));
    strings.insert(type_string(blob_type::object_recover));
    strings.insert(type_string(blob_type::kv));
    strings.insert(type_string(blob_type::kv_checkpoint));
    strings.insert(type_string(blob_type::kv_checkpoint_new));
    strings.insert(type_string(blob_type::super_blob));
    strings.insert(type_string(blob_type::free));

    FB_ASSERT_EQ(strings.size(), 9);
}

FB_TEST(blob_type_string_output, contains_blob_type_prefix) {
    std::string s = type_string(blob_type::log);
    FB_ASSERT_TRUE(s.find("blob_type::") == 0);
}

FB_TEST(blob_type_string_output, matches_enum_name) {
    FB_ASSERT_EQ(type_string(blob_type::log), "blob_type::log");
    FB_ASSERT_EQ(type_string(blob_type::object), "blob_type::object");
    FB_ASSERT_EQ(type_string(blob_type::kv), "blob_type::kv");
}

FB_TEST(blob_type_string_output, operator_ostream) {
    std::ostringstream oss;
    oss << blob_type::log;
    FB_ASSERT_EQ(oss.str(), type_string(blob_type::log));
}

FB_TEST(blob_type_string_output, multiple_outputs) {
    std::ostringstream oss;
    oss << blob_type::log << " " << blob_type::object << " " << blob_type::kv;

    std::string expected = type_string(blob_type::log) + " " +
                          type_string(blob_type::object) + " " +
                          type_string(blob_type::kv);
    FB_ASSERT_EQ(oss.str(), expected);
}

// ============================================================================
// Test Suite: xattr_field_boundaries (Xattr Field Boundary Tests)
// ============================================================================

FB_SUITE_SETUP(xattr_field_boundaries) {
    // Setup code here
}

FB_SUITE_TEARDOWN(xattr_field_boundaries) {
    // Teardown code here
}

FB_TEST(xattr_field_boundaries, log_shard_id_max) {
    log_xattr xattr;
    xattr.shard_id = std::numeric_limits<uint32_t>::max();
    FB_ASSERT_EQ(xattr.shard_id, std::numeric_limits<uint32_t>::max());
}

FB_TEST(xattr_field_boundaries, object_shard_id_max) {
    object_xattr xattr;
    xattr.shard_id = std::numeric_limits<uint32_t>::max();
    FB_ASSERT_EQ(xattr.shard_id, std::numeric_limits<uint32_t>::max());
}

FB_TEST(xattr_field_boundaries, pg_string_max_size) {
    log_xattr xattr;
    xattr.pg = std::string(10000, 'x');
    FB_ASSERT_EQ(xattr.pg.size(), 10000);
}

FB_TEST(xattr_field_boundaries, obj_name_max_size) {
    object_xattr xattr;
    xattr.obj_name = std::string(10000, 'y');
    FB_ASSERT_EQ(xattr.obj_name.size(), 10000);
}

FB_TEST(xattr_field_boundaries, snap_name_max_size) {
    object_snap_xattr xattr;
    xattr.snap_name = std::string(10000, 'z');
    FB_ASSERT_EQ(xattr.snap_name.size(), 10000);
}

FB_TEST(xattr_field_boundaries, all_xattr_types_defined) {
    // Verify all blob_type values have corresponding xattr types
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::log), 0u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object), 1u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_snap), 2u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_recover), 3u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv), 4u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint), 5u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint_new), 6u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::super_blob), 7u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::free), 8u);
}

// ============================================================================
// Test Suite: serialization_combined (Combined Serialization Tests)
// ============================================================================

FB_SUITE_SETUP(serialization_combined) {
    // Setup code here
}

FB_SUITE_TEARDOWN(serialization_combined) {
    // Teardown code here
}

FB_TEST(serialization_combined, fixed32_and_string) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint32_t num = 123;
    std::string str = "test";

    FB_ASSERT_TRUE(PutFixed32(sbuf, num));
    FB_ASSERT_TRUE(PutString(sbuf, str));

    sbuf.reset();

    uint32_t decoded_num;
    std::string decoded_str;

    FB_ASSERT_TRUE(GetFixed32(sbuf, decoded_num));
    FB_ASSERT_TRUE(GetString(sbuf, decoded_str));

    FB_ASSERT_EQ(decoded_num, num);
    FB_ASSERT_EQ(decoded_str, str);
}

FB_TEST(serialization_combined, fixed64_and_string) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint64_t num = 0x123456789ABCDEF0ULL;
    std::string str = "large_number";

    FB_ASSERT_TRUE(PutFixed64(sbuf, num));
    FB_ASSERT_TRUE(PutString(sbuf, str));

    sbuf.reset();

    uint64_t decoded_num;
    std::string decoded_str;

    FB_ASSERT_TRUE(GetFixed64(sbuf, decoded_num));
    FB_ASSERT_TRUE(GetString(sbuf, decoded_str));

    FB_ASSERT_EQ(decoded_num, num);
    FB_ASSERT_EQ(decoded_str, str);
}

FB_TEST(serialization_combined, multiple_types) {
    char buffer[200];
    spdk_buffer sbuf(buffer, 200);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 2ull));
    FB_ASSERT_TRUE(PutString(sbuf, "three"));
    FB_ASSERT_TRUE(PutOptString(sbuf, std::nullopt));

    sbuf.reset();

    uint32_t v1;
    uint64_t v2;
    std::string v3;
    std::optional<std::string> v4;

    FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
    FB_ASSERT_TRUE(GetFixed64(sbuf, v2));
    FB_ASSERT_TRUE(GetString(sbuf, v3));
    FB_ASSERT_TRUE(GetOptString(sbuf, v4));

    FB_ASSERT_EQ(v1, 1u);
    FB_ASSERT_EQ(v2, 2ull);
    FB_ASSERT_EQ(v3, "three");
    FB_ASSERT_FALSE(v4.has_value());
}

FB_TEST(serialization_combined, buffer_reuse) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    // First write
    FB_ASSERT_TRUE(PutFixed32(sbuf, 100u));
    sbuf.reset();

    // Read back
    uint32_t v;
    FB_ASSERT_TRUE(GetFixed32(sbuf, v));
    FB_ASSERT_EQ(v, 100u);

    // Reuse buffer
    sbuf.reset();
    FB_ASSERT_TRUE(PutFixed32(sbuf, 200u));
    sbuf.reset();

    FB_ASSERT_TRUE(GetFixed32(sbuf, v));
    FB_ASSERT_EQ(v, 200u);
}

// ============================================================================
// Test Suite: serialization_error_cases (Serialization Error Tests)
// ============================================================================

FB_SUITE_SETUP(serialization_error_cases) {
    // Setup code here
}

FB_SUITE_TEARDOWN(serialization_error_cases) {
    // Teardown code here
}

FB_TEST(serialization_error_cases, insufficient_for_fixed32_header) {
    char buffer[3];
    spdk_buffer sbuf(buffer, 3);

    FB_ASSERT_FALSE(PutFixed32(sbuf, 123u));
}

FB_TEST(serialization_error_cases, insufficient_for_fixed64_header) {
    char buffer[7];
    spdk_buffer sbuf(buffer, 7);

    FB_ASSERT_FALSE(PutFixed64(sbuf, 123ull));
}

FB_TEST(serialization_error_cases, insufficient_for_string_header) {
    char buffer[7];
    spdk_buffer sbuf(buffer, 7);

    FB_ASSERT_FALSE(PutString(sbuf, "test"));
}

FB_TEST(serialization_error_cases, insufficient_for_string_data) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    FB_ASSERT_FALSE(PutString(sbuf, "too_long_string"));
}

FB_TEST(serialization_error_cases, get_fixed32_insufficient) {
    char buffer[3];
    spdk_buffer sbuf(buffer, 3);

    uint32_t v;
    FB_ASSERT_FALSE(GetFixed32(sbuf, v));
}

FB_TEST(serialization_error_cases, get_fixed64_insufficient) {
    char buffer[7];
    spdk_buffer sbuf(buffer, 7);

    uint64_t v;
    FB_ASSERT_FALSE(GetFixed64(sbuf, v));
}

FB_TEST(serialization_error_cases, get_string_header_insufficient) {
    char buffer[7];
    spdk_buffer sbuf(buffer, 7);

    std::string v;
    FB_ASSERT_FALSE(GetString(sbuf, v));
}

FB_TEST(serialization_error_cases, get_string_data_insufficient) {
    char buffer[9];
    spdk_buffer sbuf(buffer, 9);

    // Try to read string that claims to be longer than buffer
    // Note: This test may not be possible without corrupting the buffer
    std::string v;
    FB_ASSERT_FALSE(GetString(sbuf, v));
}

// ============================================================================
// Test Suite: xattr_comparison (Xattr Comparison Tests)
// ============================================================================

FB_SUITE_SETUP(xattr_comparison) {
    // Setup code here
}

FB_SUITE_TEARDOWN(xattr_comparison) {
    // Teardown code here
}

FB_TEST(xattr_comparison, log_xattr_equality) {
    log_xattr x1{};
    x1.shard_id = 1;
    x1.pg = "pg1";

    log_xattr x2{};
    x2.shard_id = 1;
    x2.pg = "pg1";

    FB_ASSERT_EQ(x1.shard_id, x2.shard_id);
    FB_ASSERT_EQ(x1.pg, x2.pg);
}

FB_TEST(xattr_comparison, log_xattr_inequality) {
    log_xattr x1{};
    x1.shard_id = 1;

    log_xattr x2{};
    x2.shard_id = 2;

    FB_ASSERT_TRUE(x1.shard_id != x2.shard_id);
}

FB_TEST(xattr_comparison, object_xattr_equality) {
    object_xattr x1{};
    x1.shard_id = 1;
    x1.pg = "pg1";
    x1.obj_name = "obj1";

    object_xattr x2{};
    x2.shard_id = 1;
    x2.pg = "pg1";
    x2.obj_name = "obj1";

    FB_ASSERT_EQ(x1.shard_id, x2.shard_id);
    FB_ASSERT_EQ(x1.pg, x2.pg);
    FB_ASSERT_EQ(x1.obj_name, x2.obj_name);
}

FB_TEST(xattr_comparison, object_snap_all_fields) {
    object_snap_xattr x{};
    x.shard_id = 5;
    x.pg = "pool.pg";
    x.obj_name = "object";
    x.snap_name = "snapshot";

    FB_ASSERT_EQ(x.shard_id, 5u);
    FB_ASSERT_EQ(x.pg, "pool.pg");
    FB_ASSERT_EQ(x.obj_name, "object");
    FB_ASSERT_EQ(x.snap_name, "snapshot");
}

// ============================================================================
// Test Suite: spdk_buffer_utility (Spdk Buffer Utility Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer_utility) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer_utility) {
    // Teardown code here
}

FB_TEST(spdk_buffer_utility, remain_after_write) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_EQ(sbuf.remain(), 96u);
}

FB_TEST(spdk_buffer_utility, remain_multiple_writes) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 2ull));
    FB_ASSERT_EQ(sbuf.remain(), 88u);
}

FB_TEST(spdk_buffer_utility, remain_full_buffer) {
    char buffer[4];
    spdk_buffer sbuf(buffer, 4);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

FB_TEST(spdk_buffer_utility, reset_clears_used) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(sbuf.used() > 0);

    sbuf.reset();
    FB_ASSERT_EQ(sbuf.used(), 0u);
}

FB_TEST(spdk_buffer_utility, used_accumulates) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_EQ(sbuf.used(), 4u);

    FB_ASSERT_TRUE(PutFixed64(sbuf, 2ull));
    FB_ASSERT_EQ(sbuf.used(), 12u);

    FB_ASSERT_TRUE(PutString(sbuf, "test"));
    FB_ASSERT_EQ(sbuf.used(), 24u);
}

// ============================================================================
// Test Suite: spdk_buffer_edge_cases (Spdk Buffer Edge Cases Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer_edge_cases) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer_edge_cases) {
    // Teardown code here
}

FB_TEST(spdk_buffer_edge_cases, exact_fit_fixed32) {
    char buffer[4];
    spdk_buffer sbuf(buffer, 4);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 123u));
    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

FB_TEST(spdk_buffer_edge_cases, exact_fit_fixed64) {
    char buffer[8];
    spdk_buffer sbuf(buffer, 8);

    FB_ASSERT_TRUE(PutFixed64(sbuf, 123ull));
    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

FB_TEST(spdk_buffer_edge_cases, exact_fit_string) {
    // String "hi" needs 8 (header) + 2 (data) = 10 bytes
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    FB_ASSERT_TRUE(PutString(sbuf, "hi"));
    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

FB_TEST(spdk_buffer_edge_cases, one_byte_short_fixed32) {
    char buffer[3];
    spdk_buffer sbuf(buffer, 3);

    FB_ASSERT_FALSE(PutFixed32(sbuf, 123u));
}

FB_TEST(spdk_buffer_edge_cases, one_byte_short_fixed64) {
    char buffer[7];
    spdk_buffer sbuf(buffer, 7);

    FB_ASSERT_FALSE(PutFixed64(sbuf, 123ull));
}

FB_TEST(spdk_buffer_edge_cases, zero_size_buffer) {
    char buffer[1];
    spdk_buffer sbuf(buffer, 0);

    FB_ASSERT_EQ(sbuf.remain(), 0u);
    FB_ASSERT_EQ(sbuf.used(), 0u);

    FB_ASSERT_FALSE(PutFixed32(sbuf, 123u));
}

FB_TEST(spdk_buffer_edge_cases, overflow_protection) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    // First write succeeds
    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_EQ(sbuf.used(), 4u);
    FB_ASSERT_EQ(sbuf.remain(), 6u);

    // Second write that would overflow fails
    FB_ASSERT_FALSE(PutFixed64(sbuf, 2ull));
    FB_ASSERT_EQ(sbuf.used(), 4u);  // Used should not change
}

FB_TEST(spdk_buffer_edge_cases, consecutive_writes_tracking) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    size_t expected_used = 0;

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    expected_used += 4;
    FB_ASSERT_EQ(sbuf.used(), expected_used);

    FB_ASSERT_TRUE(PutFixed64(sbuf, 2ull));
    expected_used += 8;
    FB_ASSERT_EQ(sbuf.used(), expected_used);

    FB_ASSERT_TRUE(PutString(sbuf, "abc"));
    expected_used += 8 + 3;  // header + data
    FB_ASSERT_EQ(sbuf.used(), expected_used);

    FB_ASSERT_TRUE(PutString(sbuf, "xyz"));
    expected_used += 8 + 3;
    FB_ASSERT_EQ(sbuf.used(), expected_used);
}

FB_TEST(spdk_buffer_edge_cases, reset_repeatedly) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    for (int i = 0; i < 10; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, i));
        FB_ASSERT_EQ(sbuf.used(), 4u);
        sbuf.reset();
        FB_ASSERT_EQ(sbuf.used(), 0u);
    }
}

FB_TEST(spdk_buffer_edge_cases, get_after_partial_failure) {
    char buffer[8];
    spdk_buffer sbuf(buffer, 8);

    // Write fixed64 successfully
    FB_ASSERT_TRUE(PutFixed64(sbuf, 123ull));

    sbuf.reset();

    // Read it back
    uint64_t v1;
    FB_ASSERT_TRUE(GetFixed64(sbuf, v1));
    FB_ASSERT_EQ(v1, 123ull);

    // Try to read more (should fail)
    uint64_t v2;
    FB_ASSERT_FALSE(GetFixed64(sbuf, v2));
}

// ============================================================================
// Test Suite: log_entry_operations (Log Entry Operations Tests)
// ============================================================================

FB_SUITE_SETUP(log_entry_operations) {
    // Setup code here
}

FB_SUITE_TEARDOWN(log_entry_operations) {
    // Teardown code here
}

FB_TEST(log_entry_operations, entry_default_values) {
    log_entry_t entry{};
    FB_ASSERT_EQ(entry.term_id, 0);
    FB_ASSERT_EQ(entry.index, 0);
    FB_ASSERT_EQ(entry.size, 0);
    FB_ASSERT_EQ(entry.type, 0);
}

FB_TEST(log_entry_operations, entry_field_assignment) {
    log_entry_t entry{};
    entry.term_id = 5;
    entry.index = 100;
    entry.size = 256;
    entry.type = RAFT_LOGTYPE_WRITE;
    entry.meta = "test_meta";

    FB_ASSERT_EQ(entry.term_id, 5);
    FB_ASSERT_EQ(entry.index, 100);
    FB_ASSERT_EQ(entry.size, 256);
    FB_ASSERT_EQ(entry.type, RAFT_LOGTYPE_WRITE);
    FB_ASSERT_EQ(entry.meta, "test_meta");
}

FB_TEST(log_entry_operations, entry_large_values) {
    log_entry_t entry{};
    entry.term_id = std::numeric_limits<int>::max();
    entry.index = std::numeric_limits<long>::max();
    entry.size = std::numeric_limits<int>::max();

    FB_ASSERT_EQ(entry.term_id, std::numeric_limits<int>::max());
    FB_ASSERT_EQ(entry.index, std::numeric_limits<long>::max());
    FB_ASSERT_EQ(entry.size, std::numeric_limits<int>::max());
}

FB_TEST(log_entry_operations, entry_meta_empty) {
    log_entry_t entry{};
    FB_ASSERT_TRUE(entry.meta.empty());
}

FB_TEST(log_entry_operations, entry_meta_long) {
    log_entry_t entry{};
    entry.meta = std::string(500, 'm');
    FB_ASSERT_EQ(entry.meta.size(), 500);
}

FB_TEST(log_entry_operations, entry_meta_binary) {
    log_entry_t entry{};
    entry.meta = "\x00\x01\x02\x03\x04";
    FB_ASSERT_EQ(entry.meta.size(), 5);
}

FB_TEST(log_entry_operations, entry_copy) {
    log_entry_t entry1{};
    entry1.term_id = 10;
    entry1.index = 50;
    entry1.meta = "original";

    log_entry_t entry2 = entry1;
    FB_ASSERT_EQ(entry2.term_id, 10);
    FB_ASSERT_EQ(entry2.index, 50);
    FB_ASSERT_EQ(entry2.meta, "original");
}

FB_TEST(log_entry_operations, entry_different_types) {
    log_entry_t entry{};

    entry.type = RAFT_LOGTYPE_WRITE;
    FB_ASSERT_EQ(entry.type, RAFT_LOGTYPE_WRITE);

    entry.type = RAFT_LOGTYPE_DELETE;
    FB_ASSERT_EQ(entry.type, RAFT_LOGTYPE_DELETE);

    entry.type = RAFT_LOGTYPE_ADD_NONVOTING_NODE;
    FB_ASSERT_EQ(entry.type, RAFT_LOGTYPE_ADD_NONVOTING_NODE);

    entry.type = RAFT_LOGTYPE_CONFIGURATION;
    FB_ASSERT_EQ(entry.type, RAFT_LOGTYPE_CONFIGURATION);
}

FB_TEST(log_entry_operations, encode_decode_roundtrip) {
    char buffer[500];
    spdk_buffer sbuf(buffer, 500);

    log_entry_t original{};
    original.term_id = 123;
    original.index = 456;
    original.size = 789;
    original.type = RAFT_LOGTYPE_WRITE;
    original.meta = "test_metadata";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, original));

    sbuf.reset();
    log_entry_t decoded{};
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));

    FB_ASSERT_EQ(decoded.term_id, original.term_id);
    FB_ASSERT_EQ(decoded.index, original.index);
    FB_ASSERT_EQ(decoded.size, original.size);
    FB_ASSERT_EQ(decoded.type, original.type);
    FB_ASSERT_EQ(decoded.meta, original.meta);
}

FB_TEST(log_entry_operations, multiple_entries_sequence) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    for (int i = 0; i < 5; i++) {
        log_entry_t entry{};
        entry.term_id = i;
        entry.index = i * 10;
        entry.meta = "entry_" + std::to_string(i);

        FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    }

    FB_ASSERT_TRUE(sbuf.used() > 0);
}

// ============================================================================
// Test Suite: serialization_large_data (Large Data Serialization Tests)
// ============================================================================

FB_SUITE_SETUP(serialization_large_data) {
    // Setup code here
}

FB_SUITE_TEARDOWN(serialization_large_data) {
    // Teardown code here
}

FB_TEST(serialization_large_data, large_string_1kb) {
    char buffer[2000];
    spdk_buffer sbuf(buffer, 2000);

    std::string large(1024, 'A');
    FB_ASSERT_TRUE(PutString(sbuf, large));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_EQ(decoded.size(), 1024);
}

FB_TEST(serialization_large_data, large_string_4kb) {
    char buffer[5000];
    spdk_buffer sbuf(buffer, 5000);

    std::string large(4096, 'B');
    FB_ASSERT_TRUE(PutString(sbuf, large));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));
    FB_ASSERT_EQ(decoded.size(), 4096);
}

FB_TEST(serialization_large_data, many_small_entries) {
    char buffer[10000];
    spdk_buffer sbuf(buffer, 10000);

    for (int i = 0; i < 100; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, i));
    }

    sbuf.reset();
    for (int i = 0; i < 100; i++) {
        uint32_t v;
        FB_ASSERT_TRUE(GetFixed32(sbuf, v));
        FB_ASSERT_EQ(v, static_cast<uint32_t>(i));
    }
}

FB_TEST(serialization_large_data, alternating_types) {
    char buffer[5000];
    spdk_buffer sbuf(buffer, 5000);

    for (int i = 0; i < 50; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, i));
        FB_ASSERT_TRUE(PutString(sbuf, std::to_string(i)));
    }

    sbuf.reset();
    for (int i = 0; i < 50; i++) {
        uint32_t v;
        std::string s;
        FB_ASSERT_TRUE(GetFixed32(sbuf, v));
        FB_ASSERT_TRUE(GetString(sbuf, s));
        FB_ASSERT_EQ(v, static_cast<uint32_t>(i));
        FB_ASSERT_EQ(s, std::to_string(i));
    }
}

FB_TEST(serialization_large_data, optional_string_large) {
    char buffer[6000];
    spdk_buffer sbuf(buffer, 6000);

    std::optional<std::string> large(std::string(5000, 'C'));
    FB_ASSERT_TRUE(PutOptString(sbuf, large));

    sbuf.reset();
    std::optional<std::string> decoded;
    FB_ASSERT_TRUE(GetOptString(sbuf, decoded));
    FB_ASSERT_TRUE(decoded.has_value());
    FB_ASSERT_EQ(decoded->size(), 5000);
}

FB_TEST(serialization_large_data, buffer_near_capacity) {
    char buffer[20];
    spdk_buffer sbuf(buffer, 20);

    // Fill to exactly capacity
    FB_ASSERT_TRUE(PutFixed32(sbuf, 1));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 2));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 3));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 4));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 5));

    FB_ASSERT_EQ(sbuf.remain(), 0u);

    // Any more writes should fail
    FB_ASSERT_FALSE(PutFixed32(sbuf, 6));
}

FB_TEST(serialization_large_data, boundary_exhaustion) {
    char buffer[17];
    spdk_buffer sbuf(buffer, 17);

    // Write 16 bytes
    FB_ASSERT_TRUE(PutFixed64(sbuf, 1ull));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 2ull));

    FB_ASSERT_EQ(sbuf.remain(), 1u);

    // Can't write fixed32 (needs 4 bytes)
    FB_ASSERT_FALSE(PutFixed32(sbuf, 3u));
}

FB_TEST(serialization_large_data, mixed_large_small) {
    char buffer[3000];
    spdk_buffer sbuf(buffer, 3000);

    FB_ASSERT_TRUE(PutString(sbuf, std::string(1000, 'X')));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(PutString(sbuf, std::string(1000, 'Y')));
    FB_ASSERT_TRUE(PutFixed32(sbuf, 2u));

    sbuf.reset();

    std::string s1, s2;
    uint32_t v1, v2;

    FB_ASSERT_TRUE(GetString(sbuf, s1));
    FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
    FB_ASSERT_TRUE(GetString(sbuf, s2));
    FB_ASSERT_TRUE(GetFixed32(sbuf, v2));

    FB_ASSERT_EQ(s1.size(), 1000);
    FB_ASSERT_EQ(s2.size(), 1000);
    FB_ASSERT_EQ(v1, 1u);
    FB_ASSERT_EQ(v2, 2u);
}

FB_TEST(serialization_large_data, sequential_fixed64) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    for (uint64_t i = 0; i < 100; i++) {
        FB_ASSERT_TRUE(PutFixed64(sbuf, i * 1000));
    }

    sbuf.reset();

    for (uint64_t i = 0; i < 100; i++) {
        uint64_t v;
        FB_ASSERT_TRUE(GetFixed64(sbuf, v));
        FB_ASSERT_EQ(v, i * 1000);
    }
}

// ============================================================================
// Test Suite: error_recovery (Error Recovery Tests)
// ============================================================================

FB_SUITE_SETUP(error_recovery) {
    // Setup code here
}

FB_SUITE_TEARDOWN(error_recovery) {
    // Teardown code here
}

FB_TEST(error_recovery, partial_write_recovery) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    // First write succeeds
    FB_ASSERT_TRUE(PutFixed32(sbuf, 100u));

    // Second write fails (insufficient space)
    FB_ASSERT_FALSE(PutFixed64(sbuf, 200ull));

    // Buffer state should remain unchanged after failure
    FB_ASSERT_EQ(sbuf.used(), 4u);

    // Can still do operations that fit
    FB_ASSERT_TRUE(PutFixed32(sbuf, 300u));
    FB_ASSERT_EQ(sbuf.used(), 8u);
}

FB_TEST(error_recovery, reset_after_failure) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_FALSE(PutFixed64(sbuf, 2ull));

    // Reset clears the failure state
    sbuf.reset();

    FB_ASSERT_EQ(sbuf.used(), 0u);
    FB_ASSERT_TRUE(PutFixed64(sbuf, 3ull));
}

FB_TEST(error_recovery, read_after_write_failure) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 42u));

    // Failed write shouldn't affect previous data
    FB_ASSERT_FALSE(PutFixed64(sbuf, 99ull));

    sbuf.reset();

    uint32_t v;
    FB_ASSERT_TRUE(GetFixed32(sbuf, v));
    FB_ASSERT_EQ(v, 42u);
}

FB_TEST(error_recovery, consecutive_failures) {
    char buffer[4];
    spdk_buffer sbuf(buffer, 4);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));

    // Multiple consecutive failures
    for (int i = 0; i < 10; i++) {
        FB_ASSERT_FALSE(PutFixed32(sbuf, i));
    }

    // Buffer state remains consistent
    FB_ASSERT_EQ(sbuf.used(), 4u);
}

FB_TEST(error_recovery, mixed_success_failure) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));    // Success
    FB_ASSERT_FALSE(PutString(sbuf, std::string(200, 'x')));  // Fail
    FB_ASSERT_TRUE(PutFixed32(sbuf, 2u));    // Success
    FB_ASSERT_FALSE(PutFixed64(sbuf, 3ull)); // Fail
    FB_ASSERT_TRUE(PutString(sbuf, "ok"));  // Success

    FB_ASSERT_EQ(sbuf.used(), 16u);  // 4 + 4 + 8
}

FB_TEST(error_recovery, get_with_insufficient_data) {
    char buffer[4];
    spdk_buffer sbuf(buffer, 4);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 123u));
    sbuf.reset();

    // Read successfully
    uint32_t v;
    FB_ASSERT_TRUE(GetFixed32(sbuf, v));

    // Try to read more (no data left)
    uint32_t v2;
    FB_ASSERT_FALSE(GetFixed32(sbuf, v2));

    // Buffer position unchanged after failed read
    FB_ASSERT_EQ(sbuf.used(), 4u);
}

FB_TEST(error_recovery, string_read_failure_preserves_state) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    sbuf.reset();

    uint32_t v;
    FB_ASSERT_TRUE(GetFixed32(sbuf, v));

    // Try to read string (insufficient data for header)
    std::string s;
    FB_ASSERT_FALSE(GetString(sbuf, s));

    // State preserved
    FB_ASSERT_EQ(sbuf.used(), 4u);
}

FB_TEST(error_recovery, reset_clears_all_state) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    // Multiple operations
    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_FALSE(PutString(sbuf, std::string(200, 'x')));
    FB_ASSERT_TRUE(PutString(sbuf, "test"));

    size_t used_before = sbuf.used();

    // Reset
    sbuf.reset();

    FB_ASSERT_EQ(sbuf.used(), 0u);
    FB_ASSERT_EQ(sbuf.remain(), 100u);

    // Can start fresh
    FB_ASSERT_TRUE(PutFixed64(sbuf, 999ull));
}

FB_TEST(error_recovery, optional_string_failure_handling) {
    char buffer[20];
    spdk_buffer sbuf(buffer, 20);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));

    // Try to put optional string that's too large
    std::optional<std::string> large_opt(std::string(100, 'x'));
    FB_ASSERT_FALSE(PutOptString(sbuf, large_opt));

    // Previous data intact
    FB_ASSERT_EQ(sbuf.used(), 4u);
}

// ============================================================================
// Test Suite: data_integrity (Data Integrity Tests)
// ============================================================================

FB_SUITE_SETUP(data_integrity) {
    // Setup code here
}

FB_SUITE_TEARDOWN(data_integrity) {
    // Teardown code here
}

FB_TEST(data_integrity, fixed32_value_preserved) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint32_t original = 0xDEADBEEF;
    FB_ASSERT_TRUE(PutFixed32(sbuf, original));

    sbuf.reset();
    uint32_t decoded;
    FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));

    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(data_integrity, fixed64_value_preserved) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint64_t original = 0x123456789ABCDEF0ULL;
    FB_ASSERT_TRUE(PutFixed64(sbuf, original));

    sbuf.reset();
    uint64_t decoded;
    FB_ASSERT_TRUE(GetFixed64(sbuf, decoded));

    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(data_integrity, string_content_preserved) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original = "Hello, World! 测试数据";
    FB_ASSERT_TRUE(PutString(sbuf, original));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));

    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(data_integrity, binary_data_preserved) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original;
    for (int i = 0; i < 50; i++) {
        original.push_back(static_cast<char>(i));
    }

    FB_ASSERT_TRUE(PutString(sbuf, original));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));

    FB_ASSERT_EQ(decoded.size(), original.size());
    for (size_t i = 0; i < original.size(); i++) {
        FB_ASSERT_EQ(decoded[i], original[i]);
    }
}

FB_TEST(data_integrity, multiple_values_order) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 2ull));
    FB_ASSERT_TRUE(PutString(sbuf, "third"));

    sbuf.reset();

    uint32_t v1;
    uint64_t v2;
    std::string v3;

    FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
    FB_ASSERT_TRUE(GetFixed64(sbuf, v2));
    FB_ASSERT_TRUE(GetString(sbuf, v3));

    FB_ASSERT_EQ(v1, 1u);
    FB_ASSERT_EQ(v2, 2ull);
    FB_ASSERT_EQ(v3, "third");
}

FB_TEST(data_integrity, optional_string_values) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::optional<std::string> opt1 = "present";
    std::optional<std::string> opt2 = std::nullopt;
    std::optional<std::string> opt3 = "also_present";

    FB_ASSERT_TRUE(PutOptString(sbuf, opt1));
    FB_ASSERT_TRUE(PutOptString(sbuf, opt2));
    FB_ASSERT_TRUE(PutOptString(sbuf, opt3));

    sbuf.reset();

    std::optional<std::string> d1, d2, d3;
    FB_ASSERT_TRUE(GetOptString(sbuf, d1));
    FB_ASSERT_TRUE(GetOptString(sbuf, d2));
    FB_ASSERT_TRUE(GetOptString(sbuf, d3));

    FB_ASSERT_TRUE(d1.has_value() && *d1 == "present");
    FB_ASSERT_FALSE(d2.has_value());
    FB_ASSERT_TRUE(d3.has_value() && *d3 == "also_present");
}

FB_TEST(data_integrity, zero_values_handling) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 0u));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 0ull));
    FB_ASSERT_TRUE(PutString(sbuf, ""));

    sbuf.reset();

    uint32_t v1 = 99;
    uint64_t v2 = 99;
    std::string v3 = "non-empty";

    FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
    FB_ASSERT_TRUE(GetFixed64(sbuf, v2));
    FB_ASSERT_TRUE(GetString(sbuf, v3));

    FB_ASSERT_EQ(v1, 0u);
    FB_ASSERT_EQ(v2, 0ull);
    FB_ASSERT_TRUE(v3.empty());
}

FB_TEST(data_integrity, special_characters) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original = "\n\t\r\\\"\'\0";
    FB_ASSERT_TRUE(PutString(sbuf, original));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));

    FB_ASSERT_EQ(decoded.size(), original.size());
}

FB_TEST(data_integrity, utf8_preserved) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original = "中文测试 日本語 한국어";
    FB_ASSERT_TRUE(PutString(sbuf, original));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));

    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(data_integrity, long_term_id) {
    char buffer[500];
    spdk_buffer sbuf(buffer, 500);

    log_entry_t entry{};
    entry.term_id = 999999999;
    entry.index = 888888888;
    entry.meta = "long_term_test";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));

    sbuf.reset();
    log_entry_t decoded{};
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));

    FB_ASSERT_EQ(decoded.term_id, 999999999);
    FB_ASSERT_EQ(decoded.index, 888888888);
    FB_ASSERT_EQ(decoded.meta, "long_term_test");
}

// ============================================================================
// Test Suite: consistency_checks (Consistency Checks Tests)
// ============================================================================

FB_SUITE_SETUP(consistency_checks) {
    // Setup code here
}

FB_SUITE_TEARDOWN(consistency_checks) {
    // Teardown code here
}

FB_TEST(consistency_checks, buffer_remain_matches_size) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_EQ(sbuf.remain() + sbuf.used(), 100u);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_EQ(sbuf.remain() + sbuf.used(), 100u);

    FB_ASSERT_TRUE(PutString(sbuf, "test"));
    FB_ASSERT_EQ(sbuf.remain() + sbuf.used(), 100u);
}

FB_TEST(consistency_checks, used_accumulates_correctly) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_EQ(sbuf.used(), 0u);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_EQ(sbuf.used(), 4u);

    FB_ASSERT_TRUE(PutFixed32(sbuf, 2u));
    FB_ASSERT_EQ(sbuf.used(), 8u);

    FB_ASSERT_TRUE(PutFixed64(sbuf, 3ull));
    FB_ASSERT_EQ(sbuf.used(), 16u);
}

FB_TEST(consistency_checks, reset_clears_consistently) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    // Fill buffer partially
    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(PutString(sbuf, "data"));

    size_t used_before = sbuf.used();
    FB_ASSERT_TRUE(used_before > 0);

    // Reset
    sbuf.reset();

    FB_ASSERT_EQ(sbuf.used(), 0u);
    FB_ASSERT_EQ(sbuf.remain(), 100u);
}

FB_TEST(consistency_checks, encode_decode_size_match) {
    char buffer[500];
    spdk_buffer sbuf(buffer, 500);

    log_entry_t entry{};
    entry.term_id = 100;
    entry.index = 200;
    entry.meta = "test";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    size_t encoded_size = sbuf.used();

    sbuf.reset();
    log_entry_t decoded{};
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));

    FB_ASSERT_EQ(sbuf.used(), encoded_size);
}

FB_TEST(consistency_checks, string_length_consistency) {
    std::string test = "hello";

    size_t length = LengthString(test);

    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutString(sbuf, test));
    FB_ASSERT_EQ(sbuf.used(), length);
}

FB_TEST(consistency_checks, optional_string_length_consistency) {
    std::optional<std::string> test1 = "value";
    std::optional<std::string> test2 = std::nullopt;

    size_t len1 = LengthOptString(test1);
    size_t len2 = LengthOptString(test2);

    FB_ASSERT_TRUE(len1 > len2);

    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutOptString(sbuf, test1));
    FB_ASSERT_EQ(sbuf.used(), len1);

    sbuf.reset();
    FB_ASSERT_TRUE(PutOptString(sbuf, test2));
    FB_ASSERT_EQ(sbuf.used(), len2);
}

FB_TEST(consistency_checks, type_enum_values_unique) {
    std::set<uint32_t> values;

    values.insert(static_cast<uint32_t>(blob_type::log));
    values.insert(static_cast<uint32_t>(blob_type::object));
    values.insert(static_cast<uint32_t>(blob_type::object_snap));
    values.insert(static_cast<uint32_t>(blob_type::object_recover));
    values.insert(static_cast<uint32_t>(blob_type::kv));
    values.insert(static_cast<uint32_t>(blob_type::kv_checkpoint));
    values.insert(static_cast<uint32_t>(blob_type::kv_checkpoint_new));
    values.insert(static_cast<uint32_t>(blob_type::super_blob));
    values.insert(static_cast<uint32_t>(blob_type::free));

    FB_ASSERT_EQ(values.size(), 9u);  // All values are unique
}

FB_TEST(consistency_checks, type_enum_sequential) {
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::log), 0u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object), 1u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_snap), 2u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::object_recover), 3u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv), 4u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint), 5u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::kv_checkpoint_new), 6u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::super_blob), 7u);
    FB_ASSERT_EQ(static_cast<uint32_t>(blob_type::free), 8u);
}

FB_TEST(consistency_checks, xattr_count_matches_names) {
    FB_ASSERT_EQ(log_xattr::xattr_count, 3u);
    FB_ASSERT_EQ(object_xattr::xattr_count, 4u);
    FB_ASSERT_EQ(object_snap_xattr::xattr_count, 5u);
    FB_ASSERT_EQ(object_recover_xattr::xattr_count, 4u);
    FB_ASSERT_EQ(kv_xattr::xattr_count, 2u);
    FB_ASSERT_EQ(kv_checkpoint_xattr::xattr_count, 2u);
    FB_ASSERT_EQ(kv_checkpoint_new_xattr::xattr_count, 2u);
    FB_ASSERT_EQ(super_xattr::xattr_count, 1u);
    FB_ASSERT_EQ(free_xattr::xattr_count, 1u);
}

FB_TEST(consistency_checks, fb_blob_initial_state) {
    fb_blob blob{};
    FB_ASSERT_TRUE(blob.blob == nullptr);
    FB_ASSERT_EQ(blob.blobid, 0ull);

    // Both fields default to zero/null
    fb_blob blob2{};
    FB_ASSERT_EQ(blob.blobid, blob2.blobid);
}

// ============================================================================
// Test Suite: stress_patterns (Stress Patterns Tests)
// ============================================================================

FB_SUITE_SETUP(stress_patterns) {
    // Setup code here
}

FB_SUITE_TEARDOWN(stress_patterns) {
    // Teardown code here
}

FB_TEST(stress_patterns, repeated_same_value) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    uint32_t value = 12345;
    for (int i = 0; i < 100; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, value));
    }

    sbuf.reset();
    for (int i = 0; i < 100; i++) {
        uint32_t decoded;
        FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));
        FB_ASSERT_EQ(decoded, value);
    }
}

FB_TEST(stress_patterns, incrementing_sequence) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    for (int i = 0; i < 200; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, i));
    }

    sbuf.reset();
    for (int i = 0; i < 200; i++) {
        uint32_t decoded;
        FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));
        FB_ASSERT_EQ(decoded, static_cast<uint32_t>(i));
    }
}

FB_TEST(stress_patterns, alternating_patterns) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    for (int i = 0; i < 50; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, 0u));
        FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    }

    sbuf.reset();
    for (int i = 0; i < 50; i++) {
        uint32_t v0, v1;
        FB_ASSERT_TRUE(GetFixed32(sbuf, v0));
        FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
        FB_ASSERT_EQ(v0, 0u);
        FB_ASSERT_EQ(v1, 1u);
    }
}

FB_TEST(stress_patterns, max_min_alternation) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    for (int i = 0; i < 50; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, 0u));
        FB_ASSERT_TRUE(PutFixed32(sbuf, std::numeric_limits<uint32_t>::max()));
    }

    sbuf.reset();
    for (int i = 0; i < 50; i++) {
        uint32_t min_val, max_val;
        FB_ASSERT_TRUE(GetFixed32(sbuf, min_val));
        FB_ASSERT_TRUE(GetFixed32(sbuf, max_val));
        FB_ASSERT_EQ(min_val, 0u);
        FB_ASSERT_EQ(max_val, std::numeric_limits<uint32_t>::max());
    }
}

FB_TEST(stress_patterns, varying_length_strings) {
    char buffer[5000];
    spdk_buffer sbuf(buffer, 5000);

    for (int i = 1; i <= 20; i++) {
        FB_ASSERT_TRUE(PutString(sbuf, std::string(i, 'x')));
    }

    sbuf.reset();
    for (int i = 1; i <= 20; i++) {
        std::string decoded;
        FB_ASSERT_TRUE(GetString(sbuf, decoded));
        FB_ASSERT_EQ(decoded.size(), static_cast<size_t>(i));
    }
}

FB_TEST(stress_patterns, mixed_type_batches) {
    char buffer[3000];
    spdk_buffer sbuf(buffer, 3000);

    for (int batch = 0; batch < 10; batch++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, batch));
        FB_ASSERT_TRUE(PutFixed64(sbuf, batch));
        FB_ASSERT_TRUE(PutString(sbuf, std::to_string(batch)));
    }

    sbuf.reset();
    for (int batch = 0; batch < 10; batch++) {
        uint32_t v1;
        uint64_t v2;
        std::string v3;
        FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
        FB_ASSERT_TRUE(GetFixed64(sbuf, v2));
        FB_ASSERT_TRUE(GetString(sbuf, v3));
        FB_ASSERT_EQ(v1, static_cast<uint32_t>(batch));
        FB_ASSERT_EQ(v2, static_cast<uint64_t>(batch));
        FB_ASSERT_EQ(v3, std::to_string(batch));
    }
}

FB_TEST(stress_patterns, log_entry_batch) {
    char buffer[5000];
    spdk_buffer sbuf(buffer, 5000);

    for (int i = 0; i < 20; i++) {
        log_entry_t entry{};
        entry.term_id = i;
        entry.index = i * 10;
        entry.meta = "meta_" + std::to_string(i);
        FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    }

    FB_ASSERT_TRUE(sbuf.used() > 0);
}

FB_TEST(stress_patterns, reset_and_refill_multiple_times) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    for (int round = 0; round < 10; round++) {
        // Fill with different data each round
        FB_ASSERT_TRUE(PutFixed32(sbuf, round));
        FB_ASSERT_TRUE(PutString(sbuf, std::to_string(round)));

        sbuf.reset();

        // Verify can start fresh
        FB_ASSERT_EQ(sbuf.used(), 0u);
    }
}

FB_TEST(stress_patterns, sequential_optional_strings) {
    char buffer[2000];
    spdk_buffer sbuf(buffer, 2000);

    for (int i = 0; i < 50; i++) {
        std::optional<std::string> opt = (i % 2 == 0) ? std::optional<std::string>("present") : std::nullopt;
        FB_ASSERT_TRUE(PutOptString(sbuf, opt));
    }

    sbuf.reset();
    for (int i = 0; i < 50; i++) {
        std::optional<std::string> decoded;
        FB_ASSERT_TRUE(GetOptString(sbuf, decoded));
        bool should_have_value = (i % 2 == 0);
        FB_ASSERT_EQ(decoded.has_value(), should_have_value);
        if (should_have_value) {
            FB_ASSERT_EQ(*decoded, "present");
        }
    }
}

// ============================================================================
// Test Suite: special_scenarios (Special Scenarios Tests)
// ============================================================================

FB_SUITE_SETUP(special_scenarios) {
    // Setup code here
}

FB_SUITE_TEARDOWN(special_scenarios) {
    // Teardown code here
}

FB_TEST(special_scenarios, empty_to_empty_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original;
    FB_ASSERT_TRUE(PutString(sbuf, original));

    sbuf.reset();
    std::string decoded = "not_empty";
    FB_ASSERT_TRUE(GetString(sbuf, decoded));

    FB_ASSERT_TRUE(decoded.empty());
}

FB_TEST(special_scenarios, nullopt_roundtrip) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::optional<std::string> original = std::nullopt;
    FB_ASSERT_TRUE(PutOptString(sbuf, original));

    sbuf.reset();
    std::optional<std::string> decoded = "has_value";
    FB_ASSERT_TRUE(GetOptString(sbuf, decoded));

    FB_ASSERT_FALSE(decoded.has_value());
}

FB_TEST(special_scenarios, fixed32_all_bits_set) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint32_t value = 0xFFFFFFFF;
    FB_ASSERT_TRUE(PutFixed32(sbuf, value));

    sbuf.reset();
    uint32_t decoded;
    FB_ASSERT_TRUE(GetFixed32(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(special_scenarios, fixed64_all_bits_set) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    uint64_t value = 0xFFFFFFFFFFFFFFFFULL;
    FB_ASSERT_TRUE(PutFixed64(sbuf, value));

    sbuf.reset();
    uint64_t decoded;
    FB_ASSERT_TRUE(GetFixed64(sbuf, decoded));
    FB_ASSERT_EQ(decoded, value);
}

FB_TEST(special_scenarios, string_with_only_nulls) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string original(10, '\0');
    FB_ASSERT_TRUE(PutString(sbuf, original));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));

    FB_ASSERT_EQ(decoded.size(), 10);
    FB_ASSERT_EQ(decoded, original);
}

FB_TEST(special_scenarios, string_single_char) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    FB_ASSERT_TRUE(PutString(sbuf, "a"));

    sbuf.reset();
    std::string decoded;
    FB_ASSERT_TRUE(GetString(sbuf, decoded));

    FB_ASSERT_EQ(decoded, "a");
    FB_ASSERT_EQ(decoded.size(), 1);
}

FB_TEST(special_scenarios, log_entry_zero_meta) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    log_entry_t entry{};
    entry.term_id = 1;
    entry.index = 2;
    entry.meta = "";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));

    sbuf.reset();
    log_entry_t decoded{};
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));

    FB_ASSERT_TRUE(decoded.meta.empty());
}

FB_TEST(special_scenarios, log_entry_zero_size) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    log_entry_t entry{};
    entry.size = 0;

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
}

FB_TEST(special_scenarios, entry_type_all_valid_types) {
    log_entry_t entry{};

    int valid_types[] = {
        RAFT_LOGTYPE_WRITE,
        RAFT_LOGTYPE_DELETE,
        RAFT_LOGTYPE_ADD_NONVOTING_NODE,
        RAFT_LOGTYPE_CONFIGURATION
    };

    for (int type : valid_types) {
        entry.type = type;
        FB_ASSERT_TRUE(type >= 0);
    }
}

FB_TEST(special_scenarios, xattr_pg_empty_allowed) {
    log_xattr xattr{};
    xattr.pg = "";

    FB_ASSERT_TRUE(xattr.pg.empty());
    FB_ASSERT_EQ(xattr.pg.size(), 0);
}

FB_TEST(special_scenarios, xattr_obj_name_empty_allowed) {
    object_xattr xattr{};
    xattr.obj_name = "";

    FB_ASSERT_TRUE(xattr.obj_name.empty());
    FB_ASSERT_EQ(xattr.obj_name.size(), 0);
}

FB_TEST(special_scenarios, xattr_snap_name_empty_allowed) {
    object_snap_xattr xattr{};
    xattr.snap_name = "";

    FB_ASSERT_TRUE(xattr.snap_name.empty());
    FB_ASSERT_EQ(xattr.snap_name.size(), 0);
}

// ============================================================================
// Test Suite: validation_checks (Validation Checks Tests)
// ============================================================================

FB_SUITE_SETUP(validation_checks) {
    // Setup code here
}

FB_SUITE_TEARDOWN(validation_checks) {
    // Teardown code here
}

FB_TEST(validation_checks, positive_shard_id_valid) {
    log_xattr xattr{};
    xattr.shard_id = 0;
    // Shard_id is uint32_t, always non-negative

    xattr.shard_id = 100;
    FB_ASSERT_EQ(xattr.shard_id, 100u);
}

FB_TEST(validation_checks, shard_id_range) {
    object_xattr xattr{};
    xattr.shard_id = std::numeric_limits<uint32_t>::min();
    FB_ASSERT_EQ(xattr.shard_id, 0u);

    xattr.shard_id = std::numeric_limits<uint32_t>::max();
    FB_ASSERT_EQ(xattr.shard_id, std::numeric_limits<uint32_t>::max());
}

FB_TEST(validation_checks, string_length_reasonable) {
    std::string short_str = "a";
    std::string medium_str = "hello_world";
    std::string long_str(1000, 'x');

    FB_ASSERT_TRUE(LengthString(short_str) >= 8);
    FB_ASSERT_TRUE(LengthString(medium_str) >= 8);
    FB_ASSERT_TRUE(LengthString(long_str) >= 8);
}

FB_TEST(validation_checks, optional_string_length_reasonable) {
    std::optional<std::string> no_value = std::nullopt;
    std::optional<std::string> has_value = "test";

    FB_ASSERT_EQ(LengthOptString(no_value), 8u);
    FB_ASSERT_TRUE(LengthOptString(has_value) > 8u);
}

FB_TEST(validation_checks, buffer_size_calculation) {
    // Calculate minimum buffer for 5 fixed32 values
    size_t min_size = 5 * sizeof(uint32_t);

    char buffer[min_size];
    spdk_buffer sbuf(buffer, min_size);

    for (int i = 0; i < 5; i++) {
        FB_ASSERT_TRUE(PutFixed32(sbuf, i));
    }

    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

FB_TEST(validation_checks, entry_index_monotonic_simulation) {
    log_entry_t entries[10];
    for (int i = 0; i < 10; i++) {
        entries[i].index = i;
    }

    // Simulate checking monotonic property
    for (int i = 1; i < 10; i++) {
        FB_ASSERT_TRUE(entries[i].index > entries[i-1].index);
    }
}

FB_TEST(validation_checks, term_id_positive_simulation) {
    log_entry_t entry{};
    entry.term_id = 1;

    FB_ASSERT_TRUE(entry.term_id > 0);

    entry.term_id = 10;
    FB_ASSERT_TRUE(entry.term_id > 0);
}

FB_TEST(validation_checks, type_within_range) {
    int type = static_cast<int>(blob_type::log);
    FB_ASSERT_TRUE(type >= 0 && type <= 8);

    type = static_cast<int>(blob_type::free);
    FB_ASSERT_TRUE(type >= 0 && type <= 8);
}

FB_TEST(validation_checks, xattr_names_valid) {
    // All xattr names should be non-null and non-empty
    for (size_t i = 0; i < log_xattr::xattr_count; i++) {
        FB_ASSERT_TRUE(log_xattr::xattr_names[i] != nullptr);
        FB_ASSERT_TRUE(strlen(log_xattr::xattr_names[i]) > 0);
    }
}

FB_TEST(validation_checks, fb_blob_blobid_non_negative) {
    fb_blob blob{};
    blob.blobid = 0;
    // blobid is uint64_t, always >= 0

    blob.blobid = 12345;
    FB_ASSERT_TRUE(blob.blobid > 0);
}

FB_TEST(validation_checks, encoding_preserves_field_count) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    log_entry_t entry{};
    entry.term_id = 100;
    entry.index = 200;
    entry.size = 0;
    entry.type = 0;
    entry.meta = "";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    size_t size1 = sbuf.used();

    sbuf.reset();
    entry.meta = "test";
    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    size_t size2 = sbuf.used();

    FB_ASSERT_TRUE(size2 > size1);
}

FB_TEST(validation_checks, decode_validates_field_presence) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    log_entry_t entry{};
    entry.term_id = 1;
    entry.index = 2;
    entry.meta = "x";

    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));

    sbuf.reset();
    log_entry_t decoded{};
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));

    // All fields should be decoded
    FB_ASSERT_TRUE(decoded.term_id == 1);
    FB_ASSERT_TRUE(decoded.index == 2);
    FB_ASSERT_TRUE(decoded.meta == "x");
}

// ============================================================================
// Test Suite: final_comprehensive (Final Comprehensive Tests)
// ============================================================================

FB_SUITE_SETUP(final_comprehensive) {
    // Setup code here
}

FB_SUITE_TEARDOWN(final_comprehensive) {
    // Teardown code here
}

FB_TEST(final_comprehensive, complete_workflow_simulation) {
    char buffer[5000];
    spdk_buffer sbuf(buffer, 5000);

    // Simulate complete Workflow
    // 1. Write metadata
    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));  // Version
    FB_ASSERT_TRUE(PutString(sbuf, "metadata"));  // Metadata string

    // 2. Write entries
    for (int i = 0; i < 10; i++) {
        log_entry_t entry{};
        entry.term_id = i;
        entry.index = i * 10;
        entry.meta = "entry_" + std::to_string(i);
        FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));
    }

    // 3. Write final marker
    FB_ASSERT_TRUE(PutFixed64(sbuf, 0xFFFFFFFFFFFFFFFFULL));

    FB_ASSERT_TRUE(sbuf.used() > 0);

    // Read back
    sbuf.reset();

    uint32_t version;
    std::string metadata;
    FB_ASSERT_TRUE(GetFixed32(sbuf, version));
    FB_ASSERT_TRUE(GetString(sbuf, metadata));

    FB_ASSERT_EQ(version, 1u);
    FB_ASSERT_EQ(metadata, "metadata");
}

FB_TEST(final_comprehensive, multi_type_consistency) {
    char buffer[1000];
    spdk_buffer sbuf(buffer, 1000);

    // Write different types
    FB_ASSERT_TRUE(PutFixed32(sbuf, 1u));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 2ull));
    FB_ASSERT_TRUE(PutString(sbuf, "three"));
    FB_ASSERT_TRUE(PutOptString(sbuf, "four"));

    sbuf.reset();

    // Read and verify each type maintains its own encoding
    uint32_t v1;
    uint64_t v2;
    std::string v3;
    std::optional<std::string> v4;

    FB_ASSERT_TRUE(GetFixed32(sbuf, v1));
    FB_ASSERT_TRUE(GetFixed64(sbuf, v2));
    FB_ASSERT_TRUE(GetString(sbuf, v3));
    FB_ASSERT_TRUE(GetOptString(sbuf, v4));

    FB_ASSERT_EQ(v1, 1u);
    FB_ASSERT_EQ(v2, 2ull);
    FB_ASSERT_EQ(v3, "three");
    FB_ASSERT_TRUE(v4.has_value() && *v4 == "four");
}

FB_TEST(final_comprehensive, xattr_complete_assignment) {
    object_snap_xattr xattr{};
    xattr.shard_id = 5;
    xattr.pg = "pool.pg";
    xattr.obj_name = "object";
    xattr.snap_name = "snapshot";

    FB_ASSERT_EQ(xattr.shard_id, 5u);
    FB_ASSERT_EQ(xattr.pg, "pool.pg");
    FB_ASSERT_EQ(xattr.obj_name, "object");
    FB_ASSERT_EQ(xattr.snap_name, "snapshot");

    // Verify type matches expected
    FB_ASSERT_EQ(static_cast<uint32_t>(xattr.type), 2u);
}

FB_TEST(final_comprehensive, all_blob_types_covered) {
    // Verify all blob types have tests coverage
    blob_type types[] = {
        blob_type::log,
        blob_type::object,
        blob_type::object_snap,
        blob_type::object_recover,
        blob_type::kv,
        blob_type::kv_checkpoint,
        blob_type::kv_checkpoint_new,
        blob_type::super_blob,
        blob_type::free
    };

    for (auto type : types) {
        std::string str = type_string(type);
        FB_ASSERT_TRUE(!str.empty());
        FB_ASSERT_TRUE(str.find("blob_type::") != std::string::npos);
    }
}

FB_TEST(final_comprehensive, serialization_deserialization_pairs) {
    // For each type, test serialization-deserialization pair
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    // Fixed32
    uint32_t f32 = 100;
    FB_ASSERT_TRUE(PutFixed32(sbuf, f32));
    sbuf.reset();
    uint32_t f32_out;
    FB_ASSERT_TRUE(GetFixed32(sbuf, f32_out));
    FB_ASSERT_EQ(f32_out, f32);

    sbuf.reset();

    // Fixed64
    uint64_t f64 = 200;
    FB_ASSERT_TRUE(PutFixed64(sbuf, f64));
    sbuf.reset();
    uint64_t f64_out;
    FB_ASSERT_TRUE(GetFixed64(sbuf, f64_out));
    FB_ASSERT_EQ(f64_out, f64);
}

FB_TEST(final_comprehensive, variant_type_coverage) {
    // Test all variant types in xattr_val_type
    xattr_val_type v1 = blob_type::log;
    xattr_val_type v2 = 42u;
    xattr_val_type v3 = std::string("test");

    FB_ASSERT_TRUE(std::holds_alternative<blob_type>(v1));
    FB_ASSERT_TRUE(std::holds_alternative<uint32_t>(v2));
    FB_ASSERT_TRUE(std::holds_alternative<std::string>(v3));
}

FB_TEST(final_comprehensive, error_handling_coverage) {
    char buffer[1];
    spdk_buffer sbuf(buffer, 1);

    // All operations should fail with insufficient buffer
    FB_ASSERT_FALSE(PutFixed32(sbuf, 1u));
    FB_ASSERT_FALSE(PutFixed64(sbuf, 1ull));
    FB_ASSERT_FALSE(PutString(sbuf, "x"));

    uint32_t v;
    uint64_t v2;
    std::string s;
    FB_ASSERT_FALSE(GetFixed32(sbuf, v));
    FB_ASSERT_FALSE(GetFixed64(sbuf, v2));
    FB_ASSERT_FALSE(GetString(sbuf, s));
}

FB_TEST(final_comprehensive, boundary_values_all_types) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    // Min values
    FB_ASSERT_TRUE(PutFixed32(sbuf, 0u));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 0ull));
    FB_ASSERT_TRUE(PutString(sbuf, ""));

    sbuf.reset();

    // Max values
    FB_ASSERT_TRUE(PutFixed32(sbuf, std::numeric_limits<uint32_t>::max()));
    FB_ASSERT_TRUE(PutFixed64(sbuf, std::numeric_limits<uint64_t>::max()));

    FB_ASSERT_TRUE(sbuf.used() > 0);
}

FB_TEST(final_comprehensive, log_entry_all_types) {
    log_entry_t entry{};
    entry.term_id = 100;
    entry.index = 1000;
    entry.meta = "test";

    // Test all entry types
    int types[] = {
        RAFT_LOGTYPE_WRITE,
        RAFT_LOGTYPE_DELETE,
        RAFT_LOGTYPE_ADD_NONVOTING_NODE,
        RAFT_LOGTYPE_CONFIGURATION
    };

    for (int type : types) {
        entry.type = type;
        char buffer[100];
        spdk_buffer sbuf(buffer, 100);

        FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));

        sbuf.reset();
        log_entry_t decoded{};
        FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded));
        FB_ASSERT_EQ(static_cast<int>(decoded.type), type);
    }
}

FB_TEST(final_comprehensive, comprehensive_roundtrip) {
    char buffer[10000];
    spdk_buffer sbuf(buffer, 10000);

    // Comprehensive test with all data types
    FB_ASSERT_TRUE(PutFixed32(sbuf, 12345u));
    FB_ASSERT_TRUE(PutFixed64(sbuf, 9876543210ull));
    FB_ASSERT_TRUE(PutString(sbuf, "comprehensive_test_string"));

    log_entry_t entry{};
    entry.term_id = 5;
    entry.index = 100;
    entry.meta = "entry_meta";
    FB_ASSERT_TRUE(EncodeLogHeader(sbuf, entry));

    FB_ASSERT_TRUE(PutOptString(sbuf, "optional_value"));
    FB_ASSERT_TRUE(PutOptString(sbuf, std::nullopt));

    sbuf.reset();

    // Verify all data read back correctly
    uint32_t f32;
    uint64_t f64;
    std::string str;
    log_entry_t decoded_entry;
    std::optional<std::string> opt1, opt2;

    FB_ASSERT_TRUE(GetFixed32(sbuf, f32));
    FB_ASSERT_TRUE(GetFixed64(sbuf, f64));
    FB_ASSERT_TRUE(GetString(sbuf, str));
    FB_ASSERT_TRUE(DecodeLogHeader(sbuf, decoded_entry));
    FB_ASSERT_TRUE(GetOptString(sbuf, opt1));
    FB_ASSERT_TRUE(GetOptString(sbuf, opt2));

    FB_ASSERT_EQ(f32, 12345u);
    FB_ASSERT_EQ(f64, 9876543210ull);
    FB_ASSERT_EQ(str, "comprehensive_test_string");
    FB_ASSERT_EQ(decoded_entry.term_id, 5);
    FB_ASSERT_TRUE(opt1.has_value());
    FB_ASSERT_FALSE(opt2.has_value());
}

// ============================================================================
// Test Suite: buffer_list_basic (Buffer List Basic Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_basic) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_basic) {
    // Teardown code here
}

FB_TEST(buffer_list_basic, empty_list_bytes_zero) {
    buffer_list bl;
    FB_ASSERT_EQ(bl.bytes(), 0u);
}

FB_TEST(buffer_list_basic, empty_list_empty) {
    buffer_list bl;
    FB_ASSERT_TRUE(bl.empty());
}

FB_TEST(buffer_list_basic, append_single_buffer) {
    char buf1[100];
    spdk_buffer sbuf1(buf1, 100);

    buffer_list bl;
    bl.append_buffer(sbuf1);

    FB_ASSERT_EQ(bl.bytes(), 100u);
    FB_ASSERT_FALSE(bl.empty());
}

FB_TEST(buffer_list_basic, append_multiple_buffers) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    FB_ASSERT_EQ(bl.bytes(), 300u);
}

FB_TEST(buffer_list_basic, prepend_buffer) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.prepend_buffer(sbuf2);

    FB_ASSERT_EQ(bl.bytes(), 300u);

    // Verify front buffer is the prepended one
    auto it = bl.begin();
    FB_ASSERT_EQ(it->size(), 200u);
}

FB_TEST(buffer_list_basic, append_buffer_list) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl1;
    bl1.append_buffer(sbuf1);

    buffer_list bl2;
    bl2.append_buffer(sbuf2);
    bl2.append_buffer(sbuf3);

    bl1.append_buffer(bl2);

    FB_ASSERT_EQ(bl1.bytes(), 600u);
    FB_ASSERT_TRUE(bl2.empty());
}

FB_TEST(buffer_list_basic, clear_list) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    FB_ASSERT_EQ(bl.bytes(), 300u);

    bl.clear();

    FB_ASSERT_EQ(bl.bytes(), 0u);
    FB_ASSERT_TRUE(bl.empty());
}

FB_TEST(buffer_list_basic, trim_front) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    bl.trim_front();

    FB_ASSERT_EQ(bl.bytes(), 200u);
}

FB_TEST(buffer_list_basic, trim_back) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    bl.trim_back();

    FB_ASSERT_EQ(bl.bytes(), 100u);
}

FB_TEST(buffer_list_basic, pop_front) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    spdk_buffer front = bl.pop_front();

    FB_ASSERT_EQ(front.size(), 100u);
    FB_ASSERT_EQ(bl.bytes(), 200u);
}

FB_TEST(buffer_list_basic, pop_front_list) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    buffer_list front_list = bl.pop_front_list(2);

    FB_ASSERT_EQ(front_list.bytes(), 300u);
    FB_ASSERT_EQ(bl.bytes(), 300u);
}

// ============================================================================
// Test Suite: buffer_list_iovec (Buffer List Iovec Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_iovec) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_iovec) {
    // Teardown code here
}

FB_TEST(buffer_list_iovec, to_iovec_empty_list) {
    buffer_list bl;
    iovecs iovs = bl.to_iovec();

    FB_ASSERT_TRUE(iovs.empty());
}

FB_TEST(buffer_list_iovec, to_iovec_single_buffer) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    iovecs iovs = bl.to_iovec();

    FB_ASSERT_EQ(iovs.size(), 1u);
    FB_ASSERT_EQ(iovs[0].iov_len, 100ul);
}

FB_TEST(buffer_list_iovec, to_iovec_multiple_buffers) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    iovecs iovs = bl.to_iovec();

    FB_ASSERT_EQ(iovs.size(), 2u);
    FB_ASSERT_EQ(iovs[0].iov_len, 100ul);
    FB_ASSERT_EQ(iovs[1].iov_len, 200ul);
}

FB_TEST(buffer_list_iovec, to_iovec_with_offset) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    iovecs iovs = bl.to_iovec(50, 100);

    FB_ASSERT_EQ(iovs.size(), 2u);
}

FB_TEST(buffer_list_iovec, to_iovec_boundary) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    // Request exactly to boundary
    iovecs iovs = bl.to_iovec(0, 300);

    FB_ASSERT_EQ(iovs.size(), 2u);
}

FB_TEST(buffer_list_iovec, to_iovec_exceeds_bytes) {
    char buf1[100];
    spdk_buffer sbuf1(buf1, 100);

    buffer_list bl;
    bl.append_buffer(sbuf1);

    // Request more than available
    iovecs iovs = bl.to_iovec(0, 200);

    FB_ASSERT_TRUE(iovs.empty());
}

FB_TEST(buffer_list_iovec, to_iovec_position_exceeds) {
    char buf1[100];
    spdk_buffer sbuf1(buf1, 100);

    buffer_list bl;
    bl.append_buffer(sbuf1);

    // Position beyond available
    iovecs iovs = bl.to_iovec(150, 50);

    FB_ASSERT_TRUE(iovs.empty());
}

FB_TEST(buffer_list_iovec, to_iovec_full_list) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    iovecs iovs = bl.to_iovec();

    FB_ASSERT_EQ(iovs.size(), 3u);
    FB_ASSERT_EQ(bl.bytes(), 600u);
}

// ============================================================================
// Test Suite: buffer_list_encoder_basic (Buffer List Encoder Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_encoder_basic) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_encoder_basic) {
    // Teardown code here
}

FB_TEST(buffer_list_encoder_basic, put_uint64_single_buffer) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put(12345ull));

    FB_ASSERT_EQ(encoder.used(), 8u);
}

FB_TEST(buffer_list_encoder_basic, put_string_single_buffer) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put("test"));

    FB_ASSERT_EQ(encoder.used(), 12u);  // 8 (size) + 4 (data)
}

FB_TEST(buffer_list_encoder_basic, get_uint64_single_buffer) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put(999ull));

    bl.begin()->reset();  // Reset for reading
    buffer_list_encoder decoder(bl);

    uint64_t value;
    FB_ASSERT_TRUE(decoder.get(value));
    FB_ASSERT_EQ(value, 999ull);
}

FB_TEST(buffer_list_encoder_basic, get_string_single_buffer) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put("hello"));

    bl.begin()->reset();
    buffer_list_encoder decoder(bl);

    std::string str;
    FB_ASSERT_TRUE(decoder.get(str));
    FB_ASSERT_EQ(str, "hello");
}

FB_TEST(buffer_list_encoder_basic, put_get_roundtrip) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put(111ull));
    FB_ASSERT_TRUE(encoder.put("data"));

    bl.begin()->reset();
    buffer_list_encoder decoder(bl);

    uint64_t num;
    std::string str;
    FB_ASSERT_TRUE(decoder.get(num));
    FB_ASSERT_TRUE(decoder.get(str));

    FB_ASSERT_EQ(num, 111ull);
    FB_ASSERT_EQ(str, "data");
}

FB_TEST(buffer_list_encoder_basic, remain_calculation) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);

    size_t initial_remain = encoder.remain();
    FB_ASSERT_EQ(initial_remain, 100u);

    encoder.put(1ull);
    FB_ASSERT_EQ(encoder.remain(), 92u);
}

FB_TEST(buffer_list_encoder_basic, bytes_calculation) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);

    FB_ASSERT_EQ(encoder.bytes(), 300u);
}

FB_TEST(buffer_list_encoder_basic, multiple_values) {
    char buf[200];
    spdk_buffer sbuf(buf, 200);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put(1ull));
    FB_ASSERT_TRUE(encoder.put(2ull));
    FB_ASSERT_TRUE(encoder.put(3ull));

    bl.begin()->reset();
    buffer_list_encoder decoder(bl);

    uint64_t v1, v2, v3;
    FB_ASSERT_TRUE(decoder.get(v1));
    FB_ASSERT_TRUE(decoder.get(v2));
    FB_ASSERT_TRUE(decoder.get(v3));

    FB_ASSERT_EQ(v1, 1ull);
    FB_ASSERT_EQ(v2, 2ull);
    FB_ASSERT_EQ(v3, 3ull);
}

FB_TEST(buffer_list_encoder_basic, insufficient_space) {
    char buf[10];
    spdk_buffer sbuf(buf, 10);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_FALSE(encoder.put(1ull));  // Needs 8 bytes, but we have 10
}

// ============================================================================
// Test Suite: buffer_list_encoder_cross_buffer (Cross Buffer Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_encoder_cross_buffer) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_encoder_cross_buffer) {
    // Teardown code here
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_uint64) {
    char buf1[4], buf2[10];
    spdk_buffer sbuf1(buf1, 4);
    spdk_buffer sbuf2(buf2, 10);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put(12345ull));

    FB_ASSERT_EQ(encoder.used(), 8u);
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_string) {
    char buf1[5], buf2[15];
    spdk_buffer sbuf1(buf1, 5);
    spdk_buffer sbuf2(buf2, 15);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    FB_ASSERT_TRUE(encoder.put("hello"));  // 8 + 5 = 13 bytes

    FB_ASSERT_EQ(encoder.used(), 13u);
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_get_uint64) {
    char buf1[4], buf2[10];
    spdk_buffer sbuf1(buf1, 4);
    spdk_buffer sbuf2(buf2, 10);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    encoder.put(888ull);

    bl.begin()->reset();
    (++bl.begin())->reset();

    buffer_list_encoder decoder(bl);
    uint64_t value;
    FB_ASSERT_TRUE(decoder.get(value));
    FB_ASSERT_EQ(value, 888ull);
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_get_string) {
    char buf1[5], buf2[15];
    spdk_buffer sbuf1(buf1, 5);
    spdk_buffer sbuf2(buf2, 15);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    encoder.put("test123");

    bl.begin()->reset();
    (++bl.begin())->reset();

    buffer_list_encoder decoder(bl);
    std::string str;
    FB_ASSERT_TRUE(decoder.get(str));
    FB_ASSERT_EQ(str, "test123");
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_multiple_values) {
    char buf1[10], buf2[50];
    spdk_buffer sbuf1(buf1, 10);
    spdk_buffer sbuf2(buf2, 50);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    encoder.put(1ull);
    encoder.put("cross");
    encoder.put(2ull);

    bl.begin()->reset();
    (++bl.begin())->reset();

    buffer_list_encoder decoder(bl);
    uint64_t v1, v2;
    std::string str;
    decoder.get(v1);
    decoder.get(str);
    decoder.get(v2);

    FB_ASSERT_EQ(v1, 1ull);
    FB_ASSERT_EQ(str, "cross");
    FB_ASSERT_EQ(v2, 2ull);
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_large_data) {
    char buf1[50], buf2[1000];
    spdk_buffer sbuf1(buf1, 50);
    spdk_buffer sbuf2(buf2, 1000);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    encoder.put(100ull);
    encoder.put(std::string(100, 'x'));

    FB_ASSERT_EQ(encoder.used(), 116u);  // 8 + (8 + 100)
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_char_array) {
    char buf1[5], buf2[15];
    spdk_buffer sbuf1(buf1, 5);
    spdk_buffer sbuf2(buf2, 15);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    const char* data = "1234567890";
    encoder.put(data, 10);

    FB_ASSERT_EQ(encoder.used(), 10u);
}

FB_TEST(buffer_list_encoder_cross_buffer, cross_buffer_get_char_array) {
    char buf1[5], buf2[15];
    spdk_buffer sbuf1(buf1, 5);
    spdk_buffer sbuf2(buf2, 15);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    buffer_list_encoder encoder(bl);
    encoder.put("abcdefghij", 10);

    bl.begin()->reset();
    (++bl.begin())->reset();

    buffer_list_encoder decoder(bl);
    char output[11] = {};
    decoder.get(output, 10);

    FB_ASSERT_TRUE(strcmp(output, "abcdefghij") == 0);
}

// ============================================================================
// Test Suite: buffer_list_iterator (Buffer List Iterator Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_iterator) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_iterator) {
    // Teardown code here
}

FB_TEST(buffer_list_iterator, begin_end_empty) {
    buffer_list bl;
    FB_ASSERT_TRUE(bl.begin() == bl.end());
}

FB_TEST(buffer_list_iterator, begin_end_single) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    FB_ASSERT_TRUE(bl.begin() != bl.end());
}

FB_TEST(buffer_list_iterator, iterate_single) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    size_t count = 0;
    for (auto it = bl.begin(); it != bl.end(); ++it) {
        count++;
    }

    FB_ASSERT_EQ(count, 1u);
}

FB_TEST(buffer_list_iterator, iterate_multiple) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    size_t count = 0;
    for (auto it = bl.begin(); it != bl.end(); ++it) {
        count++;
    }

    FB_ASSERT_EQ(count, 3u);
}

FB_TEST(buffer_list_iterator, access_buffer_size) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    auto it = bl.begin();
    FB_ASSERT_EQ(it->size(), 100u);

    ++it;
    FB_ASSERT_EQ(it->size(), 200u);
}

FB_TEST(buffer_list_iterator, const_iterator) {
    char buf[100];
    spdk_buffer sbuf(buf, 100);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list::const_iterator cit = bl.begin();
    FB_ASSERT_TRUE(cit != bl.end());
}

FB_TEST(buffer_list_iterator, front_back_access) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    // Verify sizes using iterators
    auto it = bl.begin();
    FB_ASSERT_EQ(it->size(), 100u);
    ++it;
    FB_ASSERT_EQ(it->size(), 200u);
}

FB_TEST(buffer_list_iterator, iterator_after_trim) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    bl.trim_front();

    size_t count = 0;
    for (auto it = bl.begin(); it != bl.end(); ++it) {
        count++;
    }

    FB_ASSERT_EQ(count, 1u);
}

FB_TEST(buffer_list_iterator, iterator_after_pop) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    bl.pop_front();

    size_t count = 0;
    for (auto it = bl.begin(); it != bl.end(); ++it) {
        count++;
    }

    FB_ASSERT_EQ(count, 1u);
}

// ============================================================================
// Test Suite: buffer_list_complex_operations (Buffer List Complex Operations)
// ============================================================================

FB_SUITE_SETUP(buffer_list_complex_operations) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_complex_operations) {
    // Teardown code here
}

FB_TEST(buffer_list_complex_operations, append_rvalue) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl1;
    bl1.append_buffer(sbuf1);

    buffer_list bl2;
    bl2.append_buffer(sbuf2);

    bl1.append_buffer(std::move(bl2));

    FB_ASSERT_EQ(bl1.bytes(), 300u);
}

FB_TEST(buffer_list_complex_operations, multiple_trim_operations) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    bl.trim_front();
    bl.trim_back();

    FB_ASSERT_EQ(bl.bytes(), 200u);
}

FB_TEST(buffer_list_complex_operations, sequential_pops) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    spdk_buffer p1 = bl.pop_front();
    spdk_buffer p2 = bl.pop_front();

    FB_ASSERT_EQ(p1.size(), 100u);
    FB_ASSERT_EQ(p2.size(), 200u);
    FB_ASSERT_EQ(bl.bytes(), 300u);
}

FB_TEST(buffer_list_complex_operations, mixed_operations) {
    char buf1[100], buf2[200], buf3[300], buf4[400];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);
    spdk_buffer sbuf4(buf4, 400);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.prepend_buffer(sbuf3);
    bl.append_buffer(sbuf4);

    FB_ASSERT_EQ(bl.bytes(), 1000u);

    bl.trim_front();
    bl.trim_back();

    FB_ASSERT_EQ(bl.bytes(), 300u);
}

FB_TEST(buffer_list_complex_operations, empty_operations) {
    buffer_list bl;

    FB_ASSERT_TRUE(bl.empty());
    FB_ASSERT_EQ(bl.bytes(), 0u);

    // Operations on empty list
    bl.clear();

    FB_ASSERT_TRUE(bl.empty());
}

FB_TEST(buffer_list_complex_operations, append_clear_append) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);

    bl.clear();

    bl.append_buffer(sbuf2);

    FB_ASSERT_EQ(bl.bytes(), 200u);
    FB_ASSERT_FALSE(bl.empty());
}

FB_TEST(buffer_list_complex_operations, pop_front_list_partial) {
    char buf1[100], buf2[200], buf3[300], buf4[400];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);
    spdk_buffer sbuf4(buf4, 400);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);
    bl.append_buffer(sbuf4);

    buffer_list partial = bl.pop_front_list(3);

    FB_ASSERT_EQ(partial.bytes(), 600u);  // 100 + 200 + 300
    FB_ASSERT_EQ(bl.bytes(), 400u);
}

FB_TEST(buffer_list_complex_operations, to_iovec_partial_range) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    // Get middle section
    iovecs iovs = bl.to_iovec(50, 200);

    FB_ASSERT_TRUE(iovs.size() > 0);
}

FB_TEST(buffer_list_complex_operations, large_buffer_list) {
    buffer_list bl;

    for (int i = 0; i < 100; i++) {
        char* buf = new char[100];
        spdk_buffer sbuf(buf, 100);
        bl.append_buffer(sbuf);
        delete[] buf;
    }

    FB_ASSERT_EQ(bl.bytes(), 10000u);
}

// ============================================================================
// Test Suite: buffer_list_stress (Buffer List Stress Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_stress) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_stress) {
    // Teardown code here
}

FB_TEST(buffer_list_stress, repeated_append_pop) {
    buffer_list bl;

    for (int round = 0; round < 10; round++) {
        char buf[100];
        spdk_buffer sbuf(buf, 100);
        bl.append_buffer(sbuf);

        bl.pop_front();
    }

    FB_ASSERT_TRUE(bl.empty());
}

FB_TEST(buffer_list_stress, many_small_buffers) {
    buffer_list bl;

    for (int i = 0; i < 100; i++) {
        char buf[10];
        spdk_buffer sbuf(buf, 10);
        bl.append_buffer(sbuf);
    }

    FB_ASSERT_EQ(bl.bytes(), 1000u);

    for (int i = 0; i < 100; i++) {
        bl.pop_front();
    }

    FB_ASSERT_TRUE(bl.empty());
}

FB_TEST(buffer_list_stress, alternating_operations) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;

    for (int i = 0; i < 50; i++) {
        bl.append_buffer(sbuf1);
        bl.prepend_buffer(sbuf2);
    }

    FB_ASSERT_EQ(bl.bytes(), 15000u);
}

FB_TEST(buffer_list_stress, encoder_stress) {
    char buf[1000];
    spdk_buffer sbuf(buf, 1000);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);

    for (int i = 0; i < 100; i++) {
        encoder.put(static_cast<uint64_t>(i));
    }

    FB_ASSERT_EQ(encoder.used(), 800u);
}

FB_TEST(buffer_list_stress, mixed_encoder_operations) {
    char buf[2000];
    spdk_buffer sbuf(buf, 2000);

    buffer_list bl;
    bl.append_buffer(sbuf);

    buffer_list_encoder encoder(bl);

    for (int i = 0; i < 50; i++) {
        encoder.put(static_cast<uint64_t>(i));
        encoder.put(std::to_string(i));
    }

    FB_ASSERT_TRUE(encoder.used() > 0);
}

FB_TEST(buffer_list_stress, iovec_repeated_conversion) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    for (int i = 0; i < 100; i++) {
        iovecs iovs = bl.to_iovec();
        FB_ASSERT_EQ(iovs.size(), 2u);
    }
}

FB_TEST(buffer_list_stress, trim_operations_cycle) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    for (int i = 0; i < 10; i++) {
        bl.trim_front();
        bl.trim_back();
    }

    // After 10 rounds of both trims, should have 1 buffer left
    FB_ASSERT_EQ(bl.bytes(), 200u);
}

// ============================================================================
// Test Suite: buffer_list_iovec (Buffer List IO Vector Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_list_iovec) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_list_iovec) {
    // Teardown code here
}

FB_TEST(buffer_list_iovec, to_iovec_empty_list) {
    buffer_list bl;
    iovecs iovs = bl.to_iovec();
    FB_ASSERT_TRUE(iovs.empty());
}

FB_TEST(buffer_list_iovec, to_iovec_single_buffer) {
    char buf1[100];
    spdk_buffer sbuf1(buf1, 100);

    buffer_list bl;
    bl.append_buffer(sbuf1);

    iovecs iovs = bl.to_iovec();
    FB_ASSERT_EQ(iovs.size(), 1u);
    FB_ASSERT_EQ(iovs[0].iov_len, 100u);
}

FB_TEST(buffer_list_iovec, to_iovec_multiple_buffers) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    iovecs iovs = bl.to_iovec();
    FB_ASSERT_EQ(iovs.size(), 3u);
    FB_ASSERT_EQ(iovs[0].iov_len, 100u);
    FB_ASSERT_EQ(iovs[1].iov_len, 200u);
    FB_ASSERT_EQ(iovs[2].iov_len, 300u);
}

FB_TEST(buffer_list_iovec, to_iovec_partial_from_start) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    iovecs iovs = bl.to_iovec(0, 150);
    FB_ASSERT_EQ(iovs.size(), 2u);
    FB_ASSERT_EQ(iovs[0].iov_len, 100u);
    FB_ASSERT_EQ(iovs[1].iov_len, 50u);
}

FB_TEST(buffer_list_iovec, to_iovec_partial_middle) {
    char buf1[100], buf2[200], buf3[300];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);
    spdk_buffer sbuf3(buf3, 300);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);
    bl.append_buffer(sbuf3);

    iovecs iovs = bl.to_iovec(50, 200);
    FB_ASSERT_EQ(iovs.size(), 2u);
    FB_ASSERT_EQ(iovs[0].iov_len, 50u);
    FB_ASSERT_EQ(iovs[1].iov_len, 150u);
}

FB_TEST(buffer_list_iovec, to_iovec_exceeds_bytes) {
    char buf1[100];
    spdk_buffer sbuf1(buf1, 100);

    buffer_list bl;
    bl.append_buffer(sbuf1);

    iovecs iovs = bl.to_iovec(0, 200);
    FB_ASSERT_TRUE(iovs.empty());
}

FB_TEST(buffer_list_iovec, to_iovec_at_boundary) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    iovecs iovs = bl.to_iovec(100, 100);
    FB_ASSERT_EQ(iovs.size(), 1u);
    FB_ASSERT_EQ(iovs[0].iov_len, 100u);
}

FB_TEST(buffer_list_iovec, to_iovec_full_length) {
    char buf1[100], buf2[200];
    spdk_buffer sbuf1(buf1, 100);
    spdk_buffer sbuf2(buf2, 200);

    buffer_list bl;
    bl.append_buffer(sbuf1);
    bl.append_buffer(sbuf2);

    iovecs iovs = bl.to_iovec(0, 300);
    FB_ASSERT_EQ(iovs.size(), 2u);
}

// ============================================================================
// Test Suite: spdk_buffer_append (Spdk Buffer Append Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer_append) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer_append) {
    // Teardown code here
}

FB_TEST(spdk_buffer_append, append_c_string) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    size_t written = sbuf.append("hello", 5);
    FB_ASSERT_EQ(written, 5u);
    FB_ASSERT_EQ(sbuf.used(), 5u);
}

FB_TEST(spdk_buffer_append, append_std_string) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    std::string str = "test_string";
    size_t written = sbuf.append(str);
    FB_ASSERT_EQ(written, str.size());
    FB_ASSERT_EQ(sbuf.used(), str.size());
}

FB_TEST(spdk_buffer_append, append_partial) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    size_t written = sbuf.append("very_long_string", 16);
    FB_ASSERT_EQ(written, 10u);
    FB_ASSERT_EQ(sbuf.used(), 10u);
}

FB_TEST(spdk_buffer_append, append_to_full_buffer) {
    char buffer[5];
    spdk_buffer sbuf(buffer, 5);

    sbuf.append("aaaaa", 5);
    FB_ASSERT_EQ(sbuf.remain(), 0u);

    size_t written = sbuf.append("b", 1);
    FB_ASSERT_EQ(written, 0u);
}

FB_TEST(spdk_buffer_append, append_empty) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    size_t written = sbuf.append("", 0);
    FB_ASSERT_EQ(written, 0u);
    FB_ASSERT_EQ(sbuf.used(), 0u);
}

// ============================================================================
// Test Suite: spdk_buffer_inc (Spdk Buffer Increment Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer_inc) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer_inc) {
    // Teardown code here
}

FB_TEST(spdk_buffer_inc, inc_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    size_t incremented = sbuf.inc(10);
    FB_ASSERT_EQ(incremented, 10u);
    FB_ASSERT_EQ(sbuf.used(), 10u);
}

FB_TEST(spdk_buffer_inc, inc_exceed_remain) {
    char buffer[10];
    spdk_buffer sbuf(buffer, 10);

    size_t incremented = sbuf.inc(20);
    FB_ASSERT_EQ(incremented, 10u);
    FB_ASSERT_EQ(sbuf.used(), 10u);
}

FB_TEST(spdk_buffer_inc, inc_zero) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    size_t incremented = sbuf.inc(0);
    FB_ASSERT_EQ(incremented, 0u);
    FB_ASSERT_EQ(sbuf.used(), 0u);
}

FB_TEST(spdk_buffer_inc, inc_cumulative) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    sbuf.inc(10);
    sbuf.inc(20);
    sbuf.inc(30);

    FB_ASSERT_EQ(sbuf.used(), 60u);
}

FB_TEST(spdk_buffer_inc, inc_full) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    sbuf.inc(100);
    FB_ASSERT_EQ(sbuf.used(), 100u);
    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

// ============================================================================
// Test Suite: spdk_buffer_set_used (Spdk Buffer Set Used Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer_set_used) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer_set_used) {
    // Teardown code here
}

FB_TEST(spdk_buffer_set_used, set_used_basic) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    sbuf.set_used(50);
    FB_ASSERT_EQ(sbuf.used(), 50u);
    FB_ASSERT_EQ(sbuf.remain(), 50u);
}

FB_TEST(spdk_buffer_set_used, set_used_exceed_size) {
    char buffer[50];
    spdk_buffer sbuf(buffer, 50);

    sbuf.set_used(100);
    FB_ASSERT_EQ(sbuf.used(), 50u);  // Clamped to size
}

FB_TEST(spdk_buffer_set_used, set_used_zero) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    sbuf.append("test", 4);
    sbuf.set_used(0);
    FB_ASSERT_EQ(sbuf.used(), 0u);
}

FB_TEST(spdk_buffer_set_used, set_used_full) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    sbuf.set_used(100);
    FB_ASSERT_EQ(sbuf.used(), 100u);
    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

FB_TEST(spdk_buffer_set_used, set_used_negative_scenario) {
    char buffer[100];
    spdk_buffer sbuf(buffer, 100);

    // set_used with size_t, negative not applicable
    sbuf.set_used(0);
    FB_ASSERT_EQ(sbuf.used(), 0u);
}

// ============================================================================
// Test Suite: spdk_buffer_default (Spdk Buffer Default Tests)
// ============================================================================

FB_SUITE_SETUP(spdk_buffer_default) {
    // Setup code here
}

FB_SUITE_TEARDOWN(spdk_buffer_default) {
    // Teardown code here
}

FB_TEST(spdk_buffer_default, default_constructor_null) {
    spdk_buffer sbuf;
    FB_ASSERT_TRUE(sbuf.get_buf() == nullptr);
}

FB_TEST(spdk_buffer_default, default_size_zero) {
    spdk_buffer sbuf;
    FB_ASSERT_EQ(sbuf.size(), 0u);
}

FB_TEST(spdk_buffer_default, default_used_zero) {
    spdk_buffer sbuf;
    FB_ASSERT_EQ(sbuf.used(), 0u);
}

FB_TEST(spdk_buffer_default, default_remain_zero) {
    spdk_buffer sbuf;
    FB_ASSERT_EQ(sbuf.remain(), 0u);
}

FB_TEST(spdk_buffer_default, append_to_default) {
    spdk_buffer sbuf;
    size_t written = sbuf.append("test", 4);
    FB_ASSERT_EQ(written, 0u);
}

FB_TEST(spdk_buffer_default, inc_on_default) {
    spdk_buffer sbuf;
    size_t incremented = sbuf.inc(10);
    FB_ASSERT_EQ(incremented, 0u);
}

FB_TEST(spdk_buffer_default, reset_on_default) {
    spdk_buffer sbuf;
    sbuf.reset();
    FB_ASSERT_EQ(sbuf.used(), 0u);
}

// ============================================================================
// Test Suite: buffer_pool_constants (Buffer Pool Constants Tests)
// ============================================================================

FB_SUITE_SETUP(buffer_pool_constants) {
    // Setup code here
}

FB_SUITE_TEARDOWN(buffer_pool_constants) {
    // Teardown code here
}

FB_TEST(buffer_pool_constants, buffer_size_value) {
    FB_ASSERT_EQ(buffer_size, 4_KB);
}

FB_TEST(buffer_pool_constants, buffer_memory_value) {
    FB_ASSERT_EQ(buffer_memory, 512_MB);
}

FB_TEST(buffer_pool_constants, buffer_pool_size_calculation) {
    FB_ASSERT_EQ(buffer_pool_size, buffer_memory / buffer_size);
    FB_ASSERT_EQ(buffer_pool_size, 512_MB / 4_KB);
}

FB_TEST(buffer_pool_constants, buffer_pool_size_positive) {
    FB_ASSERT_TRUE(buffer_pool_size > 0);
}

FB_TEST(buffer_pool_constants, buffer_size_alignment) {
    // 4KB is typically page-aligned
    FB_ASSERT_TRUE(buffer_size % 4096 == 0);
}

FB_TEST(buffer_pool_constants, constants_consistency) {
    // Verify mathematical relationship
    uint64_t calculated = buffer_memory / buffer_size;
    FB_ASSERT_EQ(calculated, static_cast<uint64_t>(buffer_pool_size));
}

FB_TEST_MAIN()
