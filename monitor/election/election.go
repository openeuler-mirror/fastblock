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

package election

import (
	"context"
	"fmt"
	"monitor/log"
	"sync"
	"time"

	"monitor/etcdapi"
)

type LeaderElection struct {
	Mutex            sync.Mutex
	etcdClient       *etcdapi.EtcdClient
	electionKey      string
	candidateID      string
	leaderCallback   func(string, context.Context, *etcdapi.EtcdClient)
	followerCallback func(string, context.Context)
	leaseID          etcdapi.LeaseID
	IsLeader         bool
	leaderChangeChan chan struct{}
}

func NewLeaderElection(etcdClient *etcdapi.EtcdClient, electionKey string, candidateID string, leaderCallback func(string, context.Context, *etcdapi.EtcdClient), followerCallback func(string, context.Context)) *LeaderElection {
	return &LeaderElection{
		etcdClient:       etcdClient,
		electionKey:      electionKey,
		candidateID:      candidateID,
		leaderCallback:   leaderCallback,
		followerCallback: followerCallback,
		IsLeader:         false,
		leaderChangeChan: make(chan struct{}),
	}
}

func (l *LeaderElection) Run(ctx context.Context) error {
	// Create a lease to hold the leader key
	lid, err := l.etcdClient.Grant(ctx, 10)
	if err != nil {
		return err
	}
	l.Mutex.Lock()
	l.leaseID = lid
	l.Mutex.Unlock()

	// Start a goroutine to handle leader election
	go l.electLeader(ctx)

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-l.leaderChangeChan:
			if l.IsLeader {
				log.Info(ctx, "become leader, going to process leader logic")
				l.leaderCallback(l.candidateID, ctx, l.etcdClient)
			} else {
				l.followerCallback(l.getLeaderID(ctx), ctx)
			}
		}
	}
}

func (l *LeaderElection) electLeader(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		default:
			l.Mutex.Lock()
			isLeader := l.IsLeader
			lid := l.leaseID
			l.Mutex.Unlock()

			//both follower and leader should keepalive their lease
			//when follower has chance to win election, make sure it's lease is still alive
			err := l.etcdClient.KeepAliveOnce(ctx, lid)
			if err != nil {
				fmt.Printf("Failed to renew lease: %v", err)
			}
			if !isLeader {
				// Try to acquire the leader key
				isSuccess, err := l.etcdClient.PutAndLease(ctx, l.electionKey, l.candidateID, l.leaseID)
				if err != nil {
					fmt.Printf("Failed to campaign for leader: %v\r\n", err)
				} else if isSuccess {
					log.Info(ctx, "in elect Leader, good in put keys in etcd, become leader")
					l.Mutex.Lock()
					l.IsLeader = true
					l.leaderChangeChan <- struct{}{}
					l.Mutex.Unlock()
				}

			}
			time.Sleep(1 * time.Second)
		}
	}
}

func (l *LeaderElection) getLeaderID(ctx context.Context) string {
	resp, err := l.etcdClient.Get(ctx, l.electionKey)
	if err != nil {
		fmt.Printf("Failed to get leader ID: %v", err)
		return ""
	}

	return resp
}
