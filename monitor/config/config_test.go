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
package config

import "testing"

func TestTernary(t *testing.T) {
	// true branch
	if v := Ternary(true, "yes", "no").(string); v != "yes" {
		t.Errorf("Ternary(true, yes, no) = %q, want yes", v)
	}

	// false branch
	if v := Ternary(false, "yes", "no").(string); v != "no" {
		t.Errorf("Ternary(false, yes, no) = %q, want no", v)
	}

	// int types
	if v := Ternary(true, 42, 0).(int); v != 42 {
		t.Errorf("Ternary(true, 42, 0) = %d, want 42", v)
	}
	if v := Ternary(false, 42, 0).(int); v != 0 {
		t.Errorf("Ternary(false, 42, 0) = %d, want 0", v)
	}
}
