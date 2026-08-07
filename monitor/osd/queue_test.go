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

func TestCommonQueue(t *testing.T) {
	q := NewQueue()

	if !q.IsEmpty() {
		t.Error("new queue should be empty")
	}
	if q.Size() != 0 {
		t.Errorf("new queue size = %d, want 0", q.Size())
	}

	// Dequeue from empty queue
	if q.Dequeue() != nil {
		t.Error("dequeue from empty queue should return nil")
	}

	// Enqueue and check
	q.Enqueue("a")
	q.Enqueue("b")
	q.Enqueue("c")

	if q.IsEmpty() {
		t.Error("queue should not be empty after enqueue")
	}
	if q.Size() != 3 {
		t.Errorf("queue size = %d, want 3", q.Size())
	}

	// FIFO order
	if v := q.Dequeue(); v != "a" {
		t.Errorf("dequeue = %v, want a", v)
	}
	if v := q.Dequeue(); v != "b" {
		t.Errorf("dequeue = %v, want b", v)
	}
	if q.Size() != 1 {
		t.Errorf("queue size = %d, want 1", q.Size())
	}
	if v := q.Dequeue(); v != "c" {
		t.Errorf("dequeue = %v, want c", v)
	}
	if !q.IsEmpty() {
		t.Error("queue should be empty after all dequeues")
	}
}
