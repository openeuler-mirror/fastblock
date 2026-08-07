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

import (
	"strings"
	"testing"
)

func TestPrintStatusAllZero(t *testing.T) {
	result := PrintStatus(0, 0, 0, 0, 0, 0)
	if result != "" {
		t.Errorf("PrintStatus all zero should return empty, got %q", result)
	}
}

func TestPrintStatusClientOnly(t *testing.T) {
	result := PrintStatus(1024, 2048, 100, 200, 0, 0)
	if !strings.Contains(result, "io") {
		t.Errorf("expected result to contain 'io', got %q", result)
	}
	if !strings.Contains(result, "client") {
		t.Errorf("expected result to contain 'client', got %q", result)
	}
	if strings.Contains(result, "recovery") {
		t.Errorf("expected result to NOT contain 'recovery', got %q", result)
	}
}

func TestPrintStatusRecoveryOnly(t *testing.T) {
	result := PrintStatus(0, 0, 0, 0, 1048576, 10)
	if !strings.Contains(result, "recovery") {
		t.Errorf("expected result to contain 'recovery', got %q", result)
	}
}

func TestPrintStatusBoth(t *testing.T) {
	result := PrintStatus(1024, 2048, 100, 200, 1048576, 10)
	if !strings.Contains(result, "client") {
		t.Errorf("expected result to contain 'client', got %q", result)
	}
	if !strings.Contains(result, "recovery") {
		t.Errorf("expected result to contain 'recovery', got %q", result)
	}
}
