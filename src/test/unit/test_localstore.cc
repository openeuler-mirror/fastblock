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

FB_TEST_MAIN()
