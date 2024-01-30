/* Copyright (c) 2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

package etcdapi

import (
	"context"
	"fmt"
	"monitor/config"
	"net/url"

	"github.com/coreos/etcd/clientv3"
	"github.com/coreos/etcd/embed"
)

type EtcdClient struct {
	client *clientv3.Client
}

type LeaseID clientv3.LeaseID

type ErrorCode int

const ErrorKeyNotFound ErrorCode = 404

func (e ErrorCode) Error() string {
	switch e {
	case ErrorKeyNotFound:
		return "Not found"
	default:
		return "Unknown error"
	}
}

func NewEtcdClient(endpoints []string) (*EtcdClient, error) {
	config := clientv3.Config{
		Endpoints: endpoints,
	}
	client, err := clientv3.New(config)
	if err != nil {
		return nil, fmt.Errorf("failed to create etcd client: %v", err)
	}

	return &EtcdClient{
		client: client,
	}, nil
}

// Put writes a key-value pair to the Etcd database.
//
// ctx is the context for the operation.
// key is the key to be written.
// value is the value to be written.
// Returns an error if the key-value pair could not be written.
func (c *EtcdClient) Put(ctx context.Context, key string, value string) error {
	_, err := c.client.Put(ctx, key, value)
	if err != nil {
		return fmt.Errorf("failed to put key-value pair: %v", err)
	}
	return nil
}

// Delete a key-value pair to the Etcd database.
//
// ctx is the context for the operation.
// key is the key to be written.
// Returns an error if the key-value pair could not be written.
func (c *EtcdClient) Delete(ctx context.Context, key string) error {
	_, err := c.client.Delete(ctx, key)
	if err != nil {
		return fmt.Errorf("failed to delete key-value pair: %v", err)
	}
	return nil
}

// PutAndLease puts a value in the etcd key-value store and associates it with a lease.
//
// ctx - The context.Context object for the request.
// key - The key to store the value.
// value - The value to store.
// lid - The LeaseID to associate with the value.
// Returns true if the value was successfully stored, false otherwise.
// Returns an error if there was a problem putting and leasing the value.
func (c *EtcdClient) PutAndLease(ctx context.Context, key, value string, lid LeaseID) (bool, error) {
	resp, err := c.client.Txn(ctx).
		If(clientv3.Compare(clientv3.CreateRevision(key), "=", 0)).
		Then(clientv3.OpPut(key, value, clientv3.WithLease(clientv3.LeaseID(lid)))).
		Commit()

	if err != nil {
		return false, fmt.Errorf("failed to put and lease: %v", err)
	}
	return resp.Succeeded, nil
}

// Grant grants a lease with the given time-to-live (TTL).
//
// ctx: The context.Context object for cancellation and timeouts.
// ttl: The time-to-live (TTL) for the lease in seconds.
// Returns the granted LeaseID and any error encountered.
func (c *EtcdClient) Grant(ctx context.Context, ttl int64) (LeaseID, error) {
	resp, err := c.client.Grant(ctx, ttl)
	if err != nil {
		return -1, fmt.Errorf("failed to grant lease: %v", err)
	}
	return LeaseID(resp.ID), nil
}

// KeepAliveOnce keeps a lease alive once and returns any error encountered.
//
// ctx: The context for the request.
// id: The ID of the lease to keep alive.
// error: Any error encountered.
func (c *EtcdClient) KeepAliveOnce(ctx context.Context, id LeaseID) error {
	_, err := c.client.KeepAliveOnce(ctx, clientv3.LeaseID(id))
	if err != nil {
		return fmt.Errorf("failed to grant lease: %v", err)
	}
	return nil
}

// Get retrieves the value associated with the given key from the EtcdClient.
//
// It takes a context.Context and a string key as parameters.
// It returns a string value and an error.
func (c *EtcdClient) Get(ctx context.Context, key string) (string, error) {
	resp, err := c.client.Get(ctx, key)
	if err != nil {
		return "", fmt.Errorf("failed to get value for key: %v", err)
	}

	if len(resp.Kvs) == 0 {
		return "", ErrorKeyNotFound
	}

	return string(resp.Kvs[0].Value), nil
}

// KeyValue represents a key-value pair.
type KeyValue struct {
	Key   string
	Value string
}

// GetWithPrefix retrieves all key-value pairs with the given prefix from etcd.
func (c *EtcdClient) GetWithPrefix(ctx context.Context, prefix string) ([]KeyValue, error) {
	resp, err := c.client.Get(ctx, prefix, clientv3.WithPrefix())
	if err != nil {
		return nil, err
	}

	kvs := make([]KeyValue, len(resp.Kvs))
	for i, kv := range resp.Kvs {
		kvs[i] = KeyValue{
			Key:   string(kv.Key),
			Value: string(kv.Value),
		}
	}

	return kvs, nil
}

func (c *EtcdClient) NewTxn() *TxnBuilder {
	return &TxnBuilder{
		client: c.client,
		ops:    make([]clientv3.Op, 0),
	}
}

type TxnBuilder struct {
	client *clientv3.Client
	ops    []clientv3.Op
}

func (t *TxnBuilder) Put(key, value string) *TxnBuilder {
	t.ops = append(t.ops, clientv3.OpPut(key, value))
	return t
}

func (t *TxnBuilder) Delete(key string) *TxnBuilder {
	t.ops = append(t.ops, clientv3.OpDelete(key))
	return t
}

func (t *TxnBuilder) Commit(ctx context.Context) error {
	_, err := t.client.Txn(ctx).Then(t.ops...).Commit()
	if err != nil {
		return fmt.Errorf("failed to execute transaction: %v", err)
	}
	return nil
}

// NewServer initializes and starts a new Etcd server.
//
// It takes no parameters and returns an embed.Etcd instance and an error.
func NewServer(app_cfg *config.Config) (e *embed.Etcd, err error) {
	cfg := embed.NewConfig()
	cfg.Debug = false

	if app_cfg.DataDir == "" {
		cfg.Dir = "/tmp/monitor"
	} else {
		cfg.Dir = app_cfg.DataDir
	}

	if app_cfg.EtcdName == "" {
		cfg.Name = "monitor"
	} else {
		cfg.Name = app_cfg.EtcdName
	}

	if app_cfg.EtcdInitialCluster == "" {
		cfg.InitialCluster = "monitor=http://localhost:2380"
	} else {
		cfg.InitialCluster = app_cfg.EtcdInitialCluster
	}

	if len(app_cfg.EtcdACUrls) > 0 {
		cfg.ACUrls = []url.URL{}
		for _, item := range app_cfg.EtcdACUrls {
			parsed, _ := url.Parse(item)
			cfg.ACUrls = append(cfg.ACUrls, *parsed)
		}
	}

	if len(app_cfg.EtcdAPUrls) > 0 {
		cfg.APUrls = []url.URL{}
		for _, item := range app_cfg.EtcdAPUrls {
			parsed, _ := url.Parse(item)
			cfg.APUrls = append(cfg.APUrls, *parsed)
		}
	}

	if len(app_cfg.EtcdLPUrls) > 0 {
		cfg.LPUrls = []url.URL{}
		for _, item := range app_cfg.EtcdLPUrls {
			parsed, _ := url.Parse(item)
			cfg.LPUrls = append(cfg.LPUrls, *parsed)
		}
	}

	if len(cfg.LCUrls) > 0 {
		cfg.LCUrls = []url.URL{}
		for _, item := range app_cfg.EtcdLCUrls {
			parsed, _ := url.Parse(item)
			cfg.LCUrls = append(cfg.LCUrls, *parsed)
		}
	}

	return embed.StartEtcd(cfg)
}
