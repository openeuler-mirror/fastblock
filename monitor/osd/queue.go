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

type CommonQueue struct {
    items []interface{}
}

func NewQueue() *CommonQueue {
    return &CommonQueue{
        items: []interface{}{},
    }
}

func (q *CommonQueue) Enqueue(item interface{}) {
    q.items = append(q.items, item)
}

func (q *CommonQueue) Dequeue() interface{} {
    if len(q.items) == 0 {
        return nil
    }
 
    item := q.items[0]
    q.items = q.items[1:]
    return item
}

func (q *CommonQueue) IsEmpty() bool {
    return len(q.items) == 0
}

func (q *CommonQueue) Size() int {
	return len(q.items) 
}