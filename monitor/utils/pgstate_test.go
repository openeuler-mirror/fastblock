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
package utils

import "testing"

func TestPgStateStr(t *testing.T) {
	tests := []struct {
		state    PGSTATE
		expected string
	}{
		{PgCreating, "creating"},
		{PgActive, "active"},
		{PgUndersize, "undersize"},
		{PgDown, "down"},
		{PgRemapped, "remapped"},
		{PgCreating | PgUndersize, "creating+undersize"},
		{PgCreating | PgDown, "creating+down"},
		{PgUndersize | PgRemapped, "undersize+remapped"},
		{PgDown | PgRemapped, "down+remapped"},
		{PGSTATE(9999), "unknown"},
	}

	for _, tt := range tests {
		result := PgStateStr(tt.state)
		if result != tt.expected {
			t.Errorf("PgStateStr(%d) = %q, want %q", tt.state, result, tt.expected)
		}
	}
}
