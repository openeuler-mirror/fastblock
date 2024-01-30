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
package main

import (
	"context"
	"fmt"
	"log"
	"monitor/msg"
	"net"
	"strconv"
	"time"

	"github.com/golang/protobuf/proto"
	"github.com/google/uuid"
)

type ResponseChanData struct {
	uuid  string
	osdid int32
}

//we are trying to fake a osd for test, so we write like this, accurately we will write client it in C

var OsdMapVersion int64
var PgMapVersion int64

func handleResponses(ctx context.Context, conn net.Conn, responseChan chan<- ResponseChanData, stopChan chan<- struct{}, pgmapchan chan<- msg.GetPgMapResponse, osdmapChan chan<- msg.GetOsdMapResponse) {
	counter := 0
	for {
		select {
		case <-ctx.Done():
			fmt.Println("Context canceled, stop handling new requests")
			return

		default:
			// Read data from the connection
			buffer := make([]byte, 65536)
			n, err := conn.Read(buffer)
			if err != nil {
				fmt.Println("Error reading from connection:", err)
				return
			}

			// Unmarshal the received data into a Request message
			resp := &msg.Response{}
			err = proto.Unmarshal(buffer[:n], resp)
			if err != nil {
				//discard this message
				fmt.Println("Error unmarshaling request:", err)
				continue
			}

			// Handle the request based on the message type
			switch payload := resp.Union.(type) {
			case *msg.Response_ApplyIdResponse:
				// Access the fields of the ApplyIdResponse
				osdid := payload.ApplyIdResponse.GetId()
				uuid := payload.ApplyIdResponse.GetUuid()
				fmt.Printf("Received ApplyIdResponse, id is %d, uuid is %s\r\n", osdid, uuid)
				// Send the ResponseChanData to responseChan
				responseChan <- ResponseChanData{uuid, osdid}
			case *msg.Response_BootResponse:
				isok := payload.BootResponse.GetOk()
				fmt.Printf("Received BootResponse, ok is %v\r\n", isok)
				counter++
				if counter == 36 {
					fmt.Printf("got 36 Received BootResponses, we are done\r\n")
					stopChan <- struct{}{}
					return
				}
			case *msg.Response_CreatePoolResponse:
				// Access the fields of the ApplyIdResponse
				pgid := payload.CreatePoolResponse.GetPoolid()
				fmt.Printf("Received CreatePoolResponse, pgid is %d\r\n", pgid)
			case *msg.Response_HeartbeatResponse:
				// Access the fields of the ApplyIdResponse
				ok := payload.HeartbeatResponse.GetOk()
				fmt.Printf("Received HeartbeatResponse, it is ok ? %t \r\n", ok)
			case *msg.Response_GetPgmapResponse:
				pgmapchan <- *payload.GetPgmapResponse

			case *msg.Response_GetOsdmapResponse:
				// Access the fields of the GetOsdmapResponse
				osdmapChan <- *payload.GetOsdmapResponse
			}
		}
	}
}

func handleMapChanges(ctx context.Context, pgChan chan msg.GetPgMapResponse, osdmapChan chan msg.GetOsdMapResponse) {
	for {
		select {
		case <-ctx.Done():
			fmt.Println("Context canceled, stop handling new requests")
			return
		case pgmap := <-pgChan:
			fmt.Printf("Received GetPgmapResponse")
			ec := pgmap.GetErrorcode()
			for _, errcode := range ec {
				if errcode == msg.GetPgMapErrorCode_pgMapGetOk {
					for pid, pg := range pgmap.GetPgs() {
						fmt.Printf("pg is %d, %v\r\n", pid, pg.Pi)
					}
				}
			}

		case osdmap := <-osdmapChan:
			fmt.Printf("Received GetOsdmapResponse")

			ov := osdmap.GetOsdmapversion()
			if ov <= OsdMapVersion {
				fmt.Printf("osdmap version is less than ours, ours is %d, monitor's is %d\r\n", OsdMapVersion, ov)
				continue
			}
			// for osdmap, if needfullmap is true, the map returned should be fullmap

			// check ther map with our map

			osds := osdmap.GetOsds()
			for _, osd := range osds {
				fmt.Printf("osd info %v\r\n", osd)
			}

		default:
		}
	}

}

func sendApplyIDRequest(ctx context.Context, conn net.Conn, bootCh chan struct{}) {
	for {
		select {
		case <-ctx.Done():
			fmt.Println("Context canceled, stop sending applyid requests")
			return
		default:
			//seed 20 applyid request
			for i := 0; i < 36; i++ {
				log.Printf("Sending ApplyIDRequest: %d", i)
				id := uuid.New().String()
				// Create a ApplyIdRequest
				request := &msg.Request{
					Union: &msg.Request_ApplyIdRequest{
						ApplyIdRequest: &msg.ApplyIDRequest{
							Uuid: id,
						},
					},
				}

				// Marshal the ApplyIdRequest
				data, err := proto.Marshal(request)
				if err != nil {
					log.Println("Error marshaling ApplyIDRequest:", err)
					return
				}

				// Send the ApplyIDRequest to the server
				_, err = conn.Write(data)
				if err != nil {
					log.Println("Error sending ApplyIDRequest:", err)
					return
				}
				// wait for bootCh
				<-bootCh

			}
		}
	}
}

func sendBootRequest(ctx context.Context, conn net.Conn, responseChan chan ResponseChanData, bootCh chan<- struct{}) {
	for {
		select {
		case <-ctx.Done():
			fmt.Println("Context canceled, stop sending boot requests")
			return

		case rc := <-responseChan:
			// Create a BootRequest
			hostname := "fbhost" + strconv.Itoa(int(rc.osdid)%10)
			request := &msg.Request{
				Union: &msg.Request_BootRequest{
					BootRequest: &msg.BootRequest{
						OsdId:   rc.osdid,
						Uuid:    rc.uuid,
						Size_:   1024,
						Address: "127.0.0.1",
						Port:    12345,
						Host:    hostname,
					},
				},
			}

			// Marshal the BootRequest
			data, err := proto.Marshal(request)
			if err != nil {
				log.Println("Error marshaling BootRequest:", err)
				return
			}

			// Send the BootRequest to the server
			_, err = conn.Write(data)
			if err != nil {
				log.Println("Error sending BootRequest:", err)
				return
			}
			//send through bootCh
			bootCh <- struct{}{}

		default:

		}
	}
}

func sendHeartbeatRequest(conn net.Conn, ticker *time.Ticker) {
	for range ticker.C {
		// Create a HeartbeatRequest
		request := &msg.Request{
			Union: &msg.Request_HeartbeatRequest{
				HeartbeatRequest: &msg.HeartbeatRequest{
					Id: 1,
				},
			},
		}

		// Marshal the HeartbeatRequest
		data, err := proto.Marshal(request)
		if err != nil {
			log.Println("Error marshaling HeartbeatRequest:", err)
			return
		}

		// Send the HeartbeatRequest to the server
		_, err = conn.Write(data)
		if err != nil {
			log.Println("Error sending HeartbeatRequest:", err)
			return
		}

	}
}

func sendGetPgMapRequest(conn net.Conn, ticker *time.Ticker) {
	for range ticker.C {
		request := &msg.Request{
			Union: &msg.Request_GetPgmapRequest{
				GetPgmapRequest: &msg.GetPgMapRequest{
					PoolVersions: make(map[int32]int64),
				},
			},
		}

		// Marshal the GetPgMapRequest
		data, err := proto.Marshal(request)
		if err != nil {
			log.Println("Error marshaling GetPgMapRequest:", err)
			return
		}

		// Send the GetPgMapRequest to the server
		_, err = conn.Write(data)
		if err != nil {
			log.Println("Error sending GetPgMapRequest:", err)
			return
		}

	}
}

func sendGetOsdMapRequest(conn net.Conn, ticker *time.Ticker) {
	var currentversion int64 = -1
	for range ticker.C {
		request := &msg.Request{
			Union: &msg.Request_GetOsdmapRequest{
				GetOsdmapRequest: &msg.GetOsdMapRequest{
					Currentversion: currentversion,
				},
			},
		}

		// Marshal the GetOsdMapRequest
		data, err := proto.Marshal(request)
		if err != nil {
			log.Println("Error marshaling GetOsdMapRequest:", err)
			return
		}

		// Send the GetOsdMapRequest to the server
		_, err = conn.Write(data)
		if err != nil {
			log.Println("Error sending GetOsdMapRequest:", err)
			return
		}
	}
}

func main() {
	// Connect to the server
	OsdMapVersion = 0
	PgMapVersion = 0

	conn, err := net.Dial("tcp", "localhost:3333")
	if err != nil {
		log.Fatal("Error connecting to server:", err)
	}
	defer conn.Close()

	ctx, cancelFunc := context.WithCancel(context.Background())
	defer cancelFunc()

	fmt.Println("Connected to server")
	responseChan := make(chan ResponseChanData)
	stopChan := make(chan struct{})

	bootChan := make(chan struct{})
	// Send the ApplyIDRequest
	go sendApplyIDRequest(ctx, conn, bootChan)
	go sendBootRequest(ctx, conn, responseChan, bootChan)
	/*
		heartbeatTicker := time.NewTicker(time.Second)
		go sendHeartbeatRequest(conn, heartbeatTicker)
	*/

	pgmapChan := make(chan msg.GetPgMapResponse)
	PgMapTicker := time.NewTicker(10 * time.Second)
	go sendGetPgMapRequest(conn, PgMapTicker)

	osdmapChan := make(chan msg.GetOsdMapResponse)
	OsdMapTicker := time.NewTicker(10 * time.Second)
	go sendGetOsdMapRequest(conn, OsdMapTicker)

	go handleMapChanges(ctx, pgmapChan, osdmapChan)
	go handleResponses(ctx, conn, responseChan, stopChan, pgmapChan, osdmapChan)

	// Keep the main goroutine running
	for {
		select {
		case <-stopChan:
			return
		}

	}
}
