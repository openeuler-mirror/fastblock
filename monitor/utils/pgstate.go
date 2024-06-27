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

type PGSTATE uint64
/*
 * PgCreatin: 表示pg处于创建过程中
 * PgActive： 表示pg状态正常
 * PgUndersize：表示pg中处于up状态的osd数量小于副本数量，但大于副本数量的一半
 * PgDown：表示pg中处于up状态的osd数量小于等于副本数量的一半，pg不能处理读写请求
 * PgRemapped： 表示pg重新分布,会触发osd成员变更，触发osd成员变更，到成员变更完成这一阶段处于remapped状态。
*/
const (
	PgCreating  = 1 << 0
	PgActive    = 1 << 1
	PgUndersize = 1 << 2
	PgDown      = 1 << 3
	PgRemapped  = 1 << 4
) 

func PgStateStr(state PGSTATE) string {
	switch state {
	case  PgCreating:
		return "creating";
	case  PgActive:
        return "active";
	case  PgUndersize:
        return "undersize";
	case  PgDown:
        return "down";
	case  PgRemapped:
        return "remapped";
	case  PgCreating | PgUndersize:
		return "creating+undersize";
	case  PgCreating | PgDown:
		return "creating+down";
	case  PgUndersize | PgRemapped:
		return "undersize+remapped"
	case  PgDown | PgRemapped:
		return "down+remapped"
	default:
        return "unknown";
	}
}