/* Copyright (c) 2023 ChinaUnicom
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
	"log"
	"testing"
)
func TestEtcdAPI(t *testing.T) {
	ctx, _ := context.WithCancel(context.Background())
	endpoints := []string{"localhost:2379"} // Replace with your etcd server endpoints

	client, err := NewEtcdClient(endpoints)
	if err != nil {
		log.Fatal(err)
	}

	// Put example
	err = client.Put(ctx, "key1", "value1")
	if err != nil {
		t.Fatalf("Failed to put key-value pair: %v", err)
	}
	fmt.Println("Key-value pair stored successfully.")

	// Get example
	value, err := client.Get(ctx, "key1")
	if err != nil {
		t.Fatalf("Failed to get value for key: %v", err)
	}
	expectedValue := "value1"
	if value != expectedValue {
		t.Errorf("Expected value: %s, got: %s", expectedValue, value)
	}

	// Transaction example
	txn := client.NewTxn()
	err = txn.Put("key2", "value2").
		Delete("key1").
		Put("key3", "value3").
		Commit(ctx)
	if err != nil {
		t.Fatalf("Failed to execute transaction: %v", err)
	}
	fmt.Println("Transaction executed successfully.")

	// Verify transaction effects
	value, err = client.Get(ctx, "key1")
	if err == nil {
		t.Errorf("Expected error for deleted key 'key1', got value: %s", value)
	}

	value, err = client.Get(ctx, "key2")
	if err != nil {
		t.Fatalf("Failed to get value for key 'key2': %v", err)
	}
	expectedValue = "value2"
	if value != expectedValue {
		t.Errorf("Expected value: %s, got: %s", expectedValue, value)
	}

	value, err = client.Get(ctx, "key3")
	if err != nil {
		t.Fatalf("Failed to get value for key 'key3': %v", err)
	}
	expectedValue = "value3"
	if value != expectedValue {
		t.Errorf("Expected value: %s, got: %s", expectedValue, value)
	}
}

