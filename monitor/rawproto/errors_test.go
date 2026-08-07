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
package rawproto

import (
	"errors"
	"testing"
)

func TestErrorStatusNil(t *testing.T) {
	result := ErrorStatus(nil)
	if result != StatusOK {
		t.Errorf("ErrorStatus(nil) = %d, want %d (StatusOK)", result, StatusOK)
	}
}

func TestErrorStatusNonNil(t *testing.T) {
	result := ErrorStatus(errors.New("some error"))
	if result != StatusInternalError {
		t.Errorf("ErrorStatus(err) = %d, want %d (StatusInternalError)", result, StatusInternalError)
	}
}
