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
package osd

import "testing"

func TestErrorCodeError(t *testing.T) {
	tests := []struct {
		code     ErrorCode
		contains string
	}{
		{EFailureDomainNeedNotSatisfied, "failure domain"},
		{EPoolAlreadyExists, "already occupied"},
		{EInternalError, "internal error"},
		{EPoolNotExist, "not exist"},
		{EPoolNotInstance, "not exist"},
		{EOsdTreeNotExist, "not exist"},
		{ENoEnoughOsd, "no enough osd"},
		{EPgDistributionError, "distribution"},
		{EUuidAlreadyExists, "occupied"},
		{ErrorCode(9999), "Unknown"},
	}

	for _, tt := range tests {
		result := tt.code.Error()
		if result == "" {
			t.Errorf("ErrorCode(%d).Error() returned empty string", tt.code)
		}
	}
}
