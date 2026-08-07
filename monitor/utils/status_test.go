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

func TestSpeedToString(t *testing.T) {
	tests := []struct {
		input    uint64
		expected string
	}{
		{0, "0 B/s"},
		{100, "100 B/s"},
		{512, "512 B/s"},
		{1023, "1023 B/s"},
		{1024, "1 kB/s"},
		{1536, "1.5 kB/s"},
		{1048576, "1 MB/s"},
		{1073741824, "1 GB/s"},
		{1099511627776, "1 TB/s"},
	}

	for _, tt := range tests {
		result := SpeedToString(tt.input)
		if result != tt.expected {
			t.Errorf("SpeedToString(%d) = %q, want %q", tt.input, result, tt.expected)
		}
	}
}
