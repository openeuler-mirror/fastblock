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
 * @file test_localstore_types.cc
 * @brief Unit tests for localstore type definitions
 */

#include "test/framework/test_framework.h"
#include "test/framework/test_harness.h"

#include <string>
#include <cstdint>

// ============================================================================
// Test Suite: blob_type (Blob Type Enumeration)
// ============================================================================

namespace {

enum class blob_type : uint32_t {
  log = 0,
  object = 1,
  object_snap = 2,
  object_recover = 3,
  kv = 4,
  kv_checkpoint = 5,
  kv_checkpoint_new = 6,
  super_blob = 7,
  free = 8,
};

inline std::string type_string(const blob_type& type) {
  switch (type) {
    case blob_type::log: return "blob_type::log";
    case blob_type::object: return "blob_type::object";
    case blob_type::object_snap: return "blob_type::object_snap";
    case blob_type::object_recover: return "blob_type::object_recover";
    case blob_type::kv: return "blob_type::kv";
    case blob_type::kv_checkpoint: return "blob_type::kv_checkpoint";
    case blob_type::kv_checkpoint_new: return "blob_type::kv_checkpoint_new";
    case blob_type::super_blob: return "blob_type::super_blob";
    case blob_type::free: return "blob_type::free";
    default: return "blob_type::unknown";
  }
}

struct fb_blob {
    void* blob = nullptr;
    uint64_t blobid = 0;
};

} // anonymous namespace

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

FB_TEST(blob_type, type_string_log) {
    FB_ASSERT_EQ(type_string(blob_type::log), "blob_type::log");
}

FB_TEST(blob_type, type_string_object) {
    FB_ASSERT_EQ(type_string(blob_type::object), "blob_type::object");
}

FB_TEST(blob_type, type_string_object_snap) {
    FB_ASSERT_EQ(type_string(blob_type::object_snap), "blob_type::object_snap");
}

FB_TEST(blob_type, type_string_object_recover) {
    FB_ASSERT_EQ(type_string(blob_type::object_recover), "blob_type::object_recover");
}

FB_TEST(blob_type, type_string_kv) {
    FB_ASSERT_EQ(type_string(blob_type::kv), "blob_type::kv");
}

FB_TEST(blob_type, type_string_kv_checkpoint) {
    FB_ASSERT_EQ(type_string(blob_type::kv_checkpoint), "blob_type::kv_checkpoint");
}

FB_TEST(blob_type, type_string_kv_checkpoint_new) {
    FB_ASSERT_EQ(type_string(blob_type::kv_checkpoint_new), "blob_type::kv_checkpoint_new");
}

FB_TEST(blob_type, type_string_super_blob) {
    FB_ASSERT_EQ(type_string(blob_type::super_blob), "blob_type::super_blob");
}

FB_TEST(blob_type, type_string_free) {
    FB_ASSERT_EQ(type_string(blob_type::free), "blob_type::free");
}

// ============================================================================
// Test Suite: fb_blob (Blob Structure)
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
    blob.blob = reinterpret_cast<void*>(0x12345678);
    blob.blobid = 12345;
    FB_ASSERT_EQ(blob.blob, reinterpret_cast<void*>(0x12345678));
    FB_ASSERT_EQ(blob.blobid, 12345);
}

FB_TEST(fb_blob, size) {
    FB_ASSERT_TRUE(sizeof(fb_blob) >= sizeof(void*) + sizeof(uint64_t));
}
