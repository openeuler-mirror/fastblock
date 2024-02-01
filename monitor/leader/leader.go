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

package leader

import (
	"context"
	"monitor/log"
)

func LeaderCallback(whoami string, ctx context.Context) {
	log.Info(ctx, "i'm the leader, i'm %s", whoami)
}

func LeaderProcessBootMessage(id int32, uuid string, size int64) {


}

func FollowerCallback(leaderID string, ctx context.Context) {
	log.Info(ctx, "i'm the follower, leader is %s", leaderID)
}
