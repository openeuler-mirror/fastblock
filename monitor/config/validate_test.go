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

func TestValidateEmptyMonitors(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Error("validate should panic on empty Monitors")
		}
	}()
	validate(&Config{Monitors: []string{}, MonHost: []string{"host1"}})
}

func TestValidateEmptyMonHost(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Error("validate should panic on empty MonHost")
		}
	}()
	validate(&Config{Monitors: []string{"mon1"}, MonHost: []string{}})
}

func TestValidateLengthMismatch(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Error("validate should panic on length mismatch")
		}
	}()
	validate(&Config{Monitors: []string{"mon1"}, MonHost: []string{"host1", "host2"}})
}

func TestValidateValid(t *testing.T) {
	defer func() {
		if r := recover(); r != nil {
			t.Errorf("validate should not panic on valid config, got: %v", r)
		}
	}()
	validate(&Config{Monitors: []string{"mon1"}, MonHost: []string{"host1"}})
}
