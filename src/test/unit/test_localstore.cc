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

FB_TEST_MAIN()
