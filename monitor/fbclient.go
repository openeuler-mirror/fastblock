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
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"monitor/msg"
	"net"
	"strconv"
	"time"
	"monitor/utils"

	"github.com/golang/protobuf/proto"
	"github.com/google/uuid"
)

var monitor_endpoint *string
var poolname *string
var op *string
var port *int
var size *int
var pgsize *int
var pgcount *int
var address *string
var hostname *string
var uid *string
var osdid *int
var fakeOsdCount *int
var fakeHostCount *int
var version *int64
var unset *string
var set *string

// image
var imageName *string

var imagesize *int
var object_size *int

var osdmapversion int64
var myosdmap []*msg.OsdDynamicInfo
var mypgmap map[int32]*msg.PGInfos
var imagemap map[string]*msg.ImageInfo = make(map[string]*msg.ImageInfo)

var poolpgmapversion map[int32]int64

type ResponseChanData struct {
	uuid  string
	osdid int32
}

func arrayToStr(array []int32) string {
    str := "["
	for i := 0; i < len(array); i++ {
		if i != 0 {
			str += "," + strconv.FormatInt(int64(array[i]), 10)
		} else {
			str += strconv.FormatInt(int64(array[i]), 10)
		}
	}
	str += "]"
	return str
} 

func clientHandleResponses(ctx context.Context, conn net.Conn, stopChan chan<- struct{}, responseChan chan<- ResponseChanData) {
	appliedOsdCounter := 0
	bootedOsdCounter := 0
	for {
		select {
		case <-ctx.Done():
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
				fmt.Println("Error unmarshalling request:", err)
				continue
			}

			// Handle the request based on the message type
			switch payload := resp.Union.(type) {

			case *msg.Response_ApplyIdResponse:
				// Access the fields of the ApplyIdResponse
				osdid := payload.ApplyIdResponse.GetId()
				uuid := payload.ApplyIdResponse.GetUuid()
				fmt.Printf("Received ApplyIdResponse, id is %d, uuid is %s\r\n", osdid, uuid)

				// in this case we can stop
				if *fakeOsdCount == 0 {
					stopChan <- struct{}{}
					return
				} else {
					appliedOsdCounter++
					// Send the ResponseChanData to responseChan
					responseChan <- ResponseChanData{uuid, osdid}
				}

			case *msg.Response_BootResponse:
				result := payload.BootResponse.GetResult()
				fmt.Printf("Received BootResponse, result is %d\r\n", result)

				if *fakeOsdCount == 0 {
					stopChan <- struct{}{}
					return
				} else {
					bootedOsdCounter++
				}
				if bootedOsdCounter != *fakeOsdCount {
					continue
				}

				stopChan <- struct{}{}
				return

			case *msg.Response_OsdStopResponse:
				isok := payload.OsdStopResponse.GetOk()
				fmt.Printf("Received OsdStopResponse, ok is %v\r\n", isok)
				stopChan <- struct{}{}
				return

			case *msg.Response_CreatePoolResponse:
				// Access the fields of the ApplyIdResponse
				pid := payload.CreatePoolResponse.GetPoolid()
				ok := payload.CreatePoolResponse.GetOk()
				if ok {
					fmt.Printf("pool %s created, pool is %d\r\n", *poolname, pid)
				}
				//in client, after received a message, we can quit
				stopChan <- struct{}{}
				return

			case *msg.Response_DeletePoolResponse:
				// Access the fields of the ApplyIdResponse
				ok := payload.DeletePoolResponse.GetOk()
				if ok {
					fmt.Printf("pool %s deleted\r\n", *poolname)
				}

				//in client, after received a message, we can quit
				stopChan <- struct{}{}
				return

			case *msg.Response_ListPoolsResponse:
				// Access the fields of the ApplyIdResponse
				pis := payload.ListPoolsResponse.GetPi()
				if len(pis) == 0 {
					fmt.Printf("pools: no pools\r\n")
				}

				fmt.Printf("pools: \r\n")
				for _, pi := range pis {
					//print every area of pi
					fmt.Printf("poolid: %d, name: %s, pgcount: %d, pgsize: %d, root: %s\r\n", pi.Poolid, pi.Name, pi.Pgcount, pi.Pgsize, pi.Root)
				}

				//in client, after received a message, we can quit
				stopChan <- struct{}{}
				return

			case *msg.Response_HeartbeatResponse:
				// Access the fields of the ApplyIdResponse
				ok := payload.HeartbeatResponse.GetOk()
				fmt.Printf("Received HeartbeatResponse, it is ok ? %t \r\n", ok)
			case *msg.Response_GetPgmapResponse:
				pgmap := *payload.GetPgmapResponse

				if len(pgmap.PoolidPgmapversion) == 0 {
					fmt.Printf("no pools created yet\r\n")
				} else {
					ec := pgmap.GetErrorcode()
					for pool, errcode := range ec {
						if errcode == msg.GetPgMapErrorCode_pgMapGetOk {
							for _, pgs := range pgmap.GetPgs() {
								mypgmap[pool] = pgs
								poolpgmapversion[pool] = pgmap.PoolidPgmapversion[pool]
							}
						} else if errcode == msg.GetPgMapErrorCode_PgMapSameVersion {
							//we only update the poolpgmapversion and mypgmap when there is a update
						} else {
							fmt.Printf("pgmap for pool %d is errorcode is %d\r\n", pool, errcode)
						}
					}
				}
				if *op == "watchpgmap" {
					continue
				} else {
					if len(mypgmap) > 0 {
						fmt.Printf("%-10v   %-10v   %-25v    %-25v   \r\n", "PGID", "STATE", "OSDLIST", "NEWOSDLIST")
						for pid, pgs := range mypgmap {
							for _, pg := range pgs.Pi {
								pgid := strconv.FormatInt(int64(pid), 10)  + "." + strconv.FormatInt(int64(pg.GetPgid()), 10)
								osdlist := arrayToStr(pg.GetOsdid())
								newosdlist := arrayToStr(pg.GetNewosdid())
	
								fmt.Printf("%-10v   %-10v   %-25v    %-25v   \r\n", pgid, utils.PgStateStr(utils.PGSTATE(pg.GetState())), 
										osdlist, newosdlist)
							}
						}
					}
				}

				stopChan <- struct{}{}
				return

			case *msg.Response_GetOsdmapResponse:
				osdmap := *payload.GetOsdmapResponse
				if osdmap.GetErrorcode() != msg.OsdMapErrorCode_ok {
					fmt.Printf("osdmap errorcode is %d\r\n", osdmap.GetErrorcode())
				} else {
					//monitor's map is newer than ours, update osdmap
					if osdmapversion < osdmap.GetOsdmapversion() {
						osdmapversion = osdmap.GetOsdmapversion()
						myosdmap = osdmap.GetOsds()
					}
				}
				if *op == "watchosdmap" {
					// we don't quit
					continue
				} else {
					fmt.Printf("%-10v   %-20v   %-10v    %-10v   %-10v \r\n", "OSDID", "ADDRESS", "PORT", "STATUS-UP", "STATUS-IN")
					for _, osd := range myosdmap {
						state1 := "up"
						if !osd.GetIsup() {
							state1 = "down"
						}
						state2 := "in"
						if !osd.GetIsin() {
							state2 = "out"
						}
						fmt.Printf("%-10v   %-20v   %-10v    %-10v   %-10v \r\n", osd.GetOsdid(), osd.GetAddress(), osd.GetPort(), 
						        state1, state2)
					}
				}

				stopChan <- struct{}{}
				return
			case *msg.Response_GetClusterMapResponse:
				om := *payload.GetClusterMapResponse.GetGomResponse()
				pgmapVersion := payload.GetClusterMapResponse.GpmResponse.GetPoolidPgmapversion()
				pgmapErr := payload.GetClusterMapResponse.GpmResponse.GetErrorcode()
				pgs := payload.GetClusterMapResponse.GetGpmResponse().GetPgs()

				if om.GetErrorcode() != msg.OsdMapErrorCode_ok {
					fmt.Printf("osdmap errorcode is %d\r\n", om.GetErrorcode())
				} else {
					//monitor's map is newer than ours, update osdmap
					if osdmapversion < om.GetOsdmapversion() {
						osdmapversion = om.GetOsdmapversion()
						myosdmap = om.GetOsds()
					}
				}

				if len(pgmapVersion) == 0 {
					fmt.Printf("no pools created yet\r\n")
				} else {
					for pool, errcode := range pgmapErr {
						if errcode == msg.GetPgMapErrorCode_pgMapGetOk {
							for _, pgs := range pgs {
								mypgmap[pool] = pgs
								poolpgmapversion[pool] = pgmapVersion[pool]
							}
						} else if errcode == msg.GetPgMapErrorCode_PgMapSameVersion {
							//we only update the poolpgmapversion and mypgmap when there is a update
						} else {
							fmt.Printf("pgmap for pool %d is errorcode is %d\r\n", pool, errcode)
						}
					}
				}

				if *op == "watchclustermap" {
					// we don't quit
					continue
				} else {
					for _, osd := range myosdmap {
						fmt.Printf("osd info %v\r\n", osd)
					}
					for pid, pgs := range mypgmap {
						for _, pg := range pgs.Pi {
							fmt.Printf("pool is %d, pg is %d, osds are %v \r\n", pid, pg.GetPgid(), pg.GetOsdid())
						}
					}
				}

				stopChan <- struct{}{}
				return
			case *msg.Response_CreateImageResponse:
				ok := payload.CreateImageResponse.GetErrorcode()
				createimageinfo := payload.CreateImageResponse.ImageInfo

				if ok != msg.CreateImageErrorCode_createImageOk {
					fmt.Printf("create image %s failed, %s\n", *imageName, ok.String())
					stopChan <- struct{}{}
					return
				}
				if _, exists := imagemap[createimageinfo.GetImagename()]; exists {
					imagemap[createimageinfo.GetImagename()] = createimageinfo
				} else {
					imagemap[createimageinfo.GetImagename()] = createimageinfo
				}

				fmt.Printf("Image is created, imagename: %s  Poolname: %s  Size: %d   Objectsize: %d  \n ", createimageinfo.Imagename, createimageinfo.Poolname, createimageinfo.Size_, createimageinfo.ObjectSize)
				stopChan <- struct{}{}
				return

			case *msg.Response_RemoveImageResponse:
				ok := payload.RemoveImageResponse.GetErrorcode()
				if ok != 0 {
					fmt.Printf("imagename  %s can't remove , not exist \n", *imageName)
					stopChan <- struct{}{}
					return
				}
				removeimageinfo := payload.RemoveImageResponse.ImageInfo
				if _, exists := imagemap[removeimageinfo.GetImagename()]; exists {
					delete(imagemap, removeimageinfo.GetImagename())
				}
				fmt.Printf("imagename %s is removed\r\n", removeimageinfo.GetImagename())
				stopChan <- struct{}{}
				return

			case *msg.Response_ResizeImageResponse:
				ok := payload.ResizeImageResponse.GetErrorcode()
				if ok != 0 {
					fmt.Printf("imagename  %s is not exist \n ", *imageName)
					stopChan <- struct{}{}
					return
				}
				resizeimageinfo := payload.ResizeImageResponse.ImageInfo
				if _, exists := imagemap[resizeimageinfo.GetImagename()]; exists {
					imagemap[resizeimageinfo.Imagename].Size_ = resizeimageinfo.Size_
				}
				fmt.Printf("imagename  %s is resized , new size: %d \n", resizeimageinfo.Imagename, resizeimageinfo.Size_)
				stopChan <- struct{}{}
				return

			case *msg.Response_GetImageInfoResponse:
				ok := payload.GetImageInfoResponse.GetErrorcode()
				if ok != 0 {
					fmt.Printf("imagename  %s is not exist \n ", *imageName)
					stopChan <- struct{}{}
					return
				}
				getimageinfo := payload.GetImageInfoResponse.ImageInfo
				if _, exists := imagemap[getimageinfo.GetImagename()]; exists {
					imagemap[getimageinfo.Imagename].Size_ = getimageinfo.Size_
					imagemap[getimageinfo.Imagename].Poolname = getimageinfo.Poolname
					imagemap[getimageinfo.Imagename].ObjectSize = getimageinfo.ObjectSize
				} else {
					imagemap[getimageinfo.Imagename] = getimageinfo
				}
				fmt.Printf("Imagename: %s  get info imagename: %s  Poolname: %s  Size: %d   Objectsize: %d  \n", *imageName, getimageinfo.Imagename, getimageinfo.Poolname, getimageinfo.Size_, getimageinfo.ObjectSize)
				stopChan <- struct{}{}
				return

			case *msg.Response_OsdOutResponse:
				isok := payload.OsdOutResponse.GetOk()
				fmt.Printf("Received OsdOutResponse, ok is %v\r\n", isok)
				stopChan <- struct{}{}
				return

			case *msg.Response_OsdInResponse:
				isok := payload.OsdInResponse.GetOk()
				fmt.Printf("Received OsdInResponse, ok is %v\r\n", isok)
				stopChan <- struct{}{}
				return

			case *msg.Response_NoReblanceResponse:
				isok := payload.NoReblanceResponse.GetOk()
				fmt.Printf("Received NoReblanceResponse, ok is %v\r\n", isok)
				stopChan <- struct{}{}
				return

			case *msg.Response_NoOutResponse:
				isok := payload.NoOutResponse.GetOk()
				fmt.Printf("Received NoOutResponse, ok is %v\r\n", isok)
				stopChan <- struct{}{}
				return

			default:
				fmt.Printf("Unknown message type %v\r\n", payload)

			}
		}
	}
}
func printOsdMap() {
	t := time.NewTicker(5 * time.Second)
	for range t.C {
		fmt.Printf("osdmap version is %d\r\n", osdmapversion)
		for _, osd := range myosdmap {
			fmt.Printf("osd info %v\r\n", osd)
		}
	}
}

func printPgMap() {
	t := time.NewTicker(2 * time.Second)
	for range t.C {
		for pid, pgs := range mypgmap {
			fmt.Printf("pool %d: \r\n", pid)
			for _, pg := range pgs.Pi {
				fmt.Printf("pg %d, osds are %v \r\n", pg.GetPgid(), pg.GetOsdid())
			}
			fmt.Println("----------------------------------------------------------------------------")
		}
	}
}

func sendCreatePoolRequest(conn net.Conn) {
	// Create a CreatePoolRequest
	request := &msg.Request{
		Union: &msg.Request_CreatePoolRequest{
			CreatePoolRequest: &msg.CreatePoolRequest{
				Name:    *poolname,
				Pgsize:  int32(*pgsize),
				Pgcount: int32(*pgcount),
				//(fixme) make it configurable
				Failuredomain: "osd",
				Root:          "defaultroot",
			},
		},
	}

	// Marshal the CreatePoolRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling CreatePoolRequest:", err)
		return
	}

	// Send the CreatePoolRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending CreatePoolRequest:", err)
		return
	}

}

func sendDeletePoolRequest(conn net.Conn) {
	// Create a DeletePoolRequest
	request := &msg.Request{
		Union: &msg.Request_DeletePoolRequest{
			DeletePoolRequest: &msg.DeletePoolRequest{
				Name: *poolname,
			},
		},
	}

	// Marshal the DeletePoolRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling DeletePoolRequest:", err)
		return
	}

	// Send the DeletePoolRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending CreatePoolRequest:", err)
		return
	}

}

func clientSendGetPgMapRequest(conn net.Conn) {
	request := &msg.Request{
		Union: &msg.Request_GetPgmapRequest{
			GetPgmapRequest: &msg.GetPgMapRequest{
				PoolVersions: make(map[int32]int64),
			},
		},
	}

	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling GetPgMapRequest:", err)
		return
	}

	// Send the CreatePGRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending GetPgMapRequest:", err)
		return
	}

}

func clientSendGetOsdMapRequest(conn net.Conn, version int64) {
	request := &msg.Request{
		Union: &msg.Request_GetOsdmapRequest{
			GetOsdmapRequest: &msg.GetOsdMapRequest{
				Osdid:          int32(*osdid),
				Currentversion: version,
			},
		},
	}

	// Marshal the GetOsdMapRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling GetOsdMapRequest:", err)
		return
	}

	// Send the CreatePGRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending GetOsdMapRequest:", err)
		return
	}
}

func clientSendGetClusterMapRequest(conn net.Conn, isWatch bool) {
	watchTicker := time.NewTicker(1 * time.Second)
	for range watchTicker.C {
		//empty pg version, because we don't send pool versions
		gpmr := &msg.GetPgMapRequest{
			PoolVersions: make(map[int32]int64),
		}

		gomr := &msg.GetOsdMapRequest{
			Osdid:          int32(*osdid),
			Currentversion: *version,
		}

		request := &msg.Request{
			Union: &msg.Request_GetClusterMapRequest{
				GetClusterMapRequest: &msg.GetClusterMapRequest{
					GpmRequest: gpmr,
					GomRequest: gomr,
				},
			},
		}

		// Marshal the GetClusterMapRequest
		data, err := proto.Marshal(request)
		if err != nil {
			log.Println("Error marshaling GetClusterMapRequest:", err)
			return
		}

		// Send the CreatePGRequest to the server
		_, err = conn.Write(data)
		if err != nil {
			log.Println("Error sending GetClusterMapRequest:", err)
			return
		}
		if !isWatch {
			break
		}
	}
}

func clientWatchOsdMapRequest(conn net.Conn) {
	watchTicker := time.NewTicker(5 * time.Second)
	for range watchTicker.C {
		request := &msg.Request{
			Union: &msg.Request_GetOsdmapRequest{
				GetOsdmapRequest: &msg.GetOsdMapRequest{
					Osdid:          int32(*osdid),
					Currentversion: osdmapversion,
				},
			},
		}

		// Marshal the GetOsdMapRequest
		data, err := proto.Marshal(request)
		if err != nil {
			log.Println("Error marshaling GetOsdMapRequest:", err)
			return
		}

		// Send the CreatePGRequest to the server
		_, err = conn.Write(data)
		if err != nil {
			log.Println("Error sending GetOsdMapRequest:", err)
			return
		}
	}
}

func clientWatchPgMapRequest(conn net.Conn) {
	watchTicker := time.NewTicker(5 * time.Second)
	for range watchTicker.C {
		request := &msg.Request{
			Union: &msg.Request_GetPgmapRequest{
				GetPgmapRequest: &msg.GetPgMapRequest{
					PoolVersions: nil,
				},
			},
		}

		// Marshal the GetOsdMapRequest
		data, err := proto.Marshal(request)
		if err != nil {
			log.Println("Error marshaling GetOsdMapRequest:", err)
			return
		}

		// Send the CreatePGRequest to the server
		_, err = conn.Write(data)
		if err != nil {
			log.Println("Error sending GetOsdMapRequest:", err)
			return
		}
	}
}

func sendListPoolsRequest(conn net.Conn) {
	request := &msg.Request{
		Union: &msg.Request_ListPoolsRequest{
			ListPoolsRequest: &msg.ListPoolsRequest{},
		},
	}

	// Marshal the GetOsdMapRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling ListPoolsRequest:", err)
		return
	}

	// Send the CreatePGRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending ListPoolsRequest:", err)
		return
	}
}

func clientSendApplyIDRequest(conn net.Conn, uuidstring string, counter int) {
	for i := 0; i < counter; i++ {
		// Create a ApplyIdRequest
		uid := uuidstring
		if counter > 1 {
			// only one applyid request, we respect the uuid
			uid = uuid.New().String()
		}
		request := &msg.Request{
			Union: &msg.Request_ApplyIdRequest{
				ApplyIdRequest: &msg.ApplyIDRequest{
					Uuid: uid,
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
		// sleep for one second because we are not busy
		time.Sleep(100 * time.Millisecond)
	}
}

func clientSendStopRequest(conn net.Conn) {
	// Create a OsdStopRequest
	request := &msg.Request{
		Union: &msg.Request_OsdStopRequest{
			OsdStopRequest: &msg.OsdStopRequest{
				Id: int32(*osdid),
			},
		},
	}

	// Marshal the OsdStopRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling OsdStopRequest:", err)
		return
	}

	// Send the OsdStopRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending OsdStopRequest:", err)
		return
	}
}

func clientSendBootRequest(conn net.Conn, hosts int, responseChan chan ResponseChanData) {
	// default case ,we just boot the osd with provided parameters
	if *fakeHostCount == 0 {
		request := &msg.Request{
			Union: &msg.Request_BootRequest{
				BootRequest: &msg.BootRequest{
					OsdId:   int32(*osdid),
					Uuid:    *uid,
					Size_:   int64(*size),
					Address: *address,
					Port:    uint32(*port),
					Host:    *hostname,
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
		return
	} else {
		for {
			select {
			case rc := <-responseChan:
				// Create a BootRequest
				fmt.Printf("using osdid %d to boot a osd\r\n", rc.osdid)
				suffix := strconv.Itoa(int(rc.osdid)%(hosts) + 1)
				hostname := "fbhost" + suffix
				port := 12345 + rc.osdid
				addr := "192.168.1." + suffix
				request := &msg.Request{
					Union: &msg.Request_BootRequest{
						BootRequest: &msg.BootRequest{
							OsdId:   rc.osdid,
							Uuid:    rc.uuid,
							Size_:   1024,
							Address: addr,
							Port:    uint32(port),
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
			}
		}
	}
}

func ClientSendCreateImageRequest(conn net.Conn, stopChan chan<- struct{}) {
	if *imagesize == 0 {
		fmt.Println("need imagesize > 0 ")
		stopChan <- struct{}{}
		return
	}
	if len(*imageName) == 0 {
		fmt.Println(" need  imagename  ")
		stopChan <- struct{}{}
		return
	}
	if len(*poolname) == 0 {
		fmt.Println(" need  poolname  ")
		stopChan <- struct{}{}
		return
	}

	if *imagesize < 4194304 {
		fmt.Println(" need  imagesize >= 4MB  ")
		stopChan <- struct{}{}
		return
	}
	request := &msg.Request{
		Union: &msg.Request_CreateImageRequest{
			CreateImageRequest: &msg.CreateImageRequest{
				Poolname:   string(*poolname),
				Imagename:  string(*imageName),
				Size_:      int64(*imagesize),
				ObjectSize: int64(*object_size),
			},
		},
	}

	// Marshal the GetOsdMapRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling GetOsdMapRequest:", err)
		return
	}

	// Send the CreatePGRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending GetOsdMapRequest:", err)
		return
	}
	//fmt.Printf("send create image requset  success: poolid:%d  imagename:%s  imagesize:%d  object_size: %d  \n\n",*poolid,*imageName,*imagesize,*object_size)
}

func ClientSendRemoveImageRequest(conn net.Conn, stopChan chan<- struct{}) {
	if len(*imageName) == 0 {
		fmt.Println("request need imagename ")
		stopChan <- struct{}{}
		return
	}
	if len(*poolname) == 0 {
		fmt.Println(" need  poolname  ")
		stopChan <- struct{}{}
		return
	}

	request := &msg.Request{
		Union: &msg.Request_RemoveImageRequest{
			RemoveImageRequest: &msg.RemoveImageRequest{
				Poolname:  string(*poolname),
				Imagename: *imageName,
			},
		},
	}

	// Marshal the GetOsdMapRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling GetOsdMapRequest:", err)
		return
	}

	// Send the CreatePGRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending GetOsdMapRequest:", err)
		return
	}
}

func ClientSendResizeImageRequest(conn net.Conn, stopChan chan<- struct{}) {

	if len(*imageName) == 0 {
		fmt.Println(" need  imagename  ")
		stopChan <- struct{}{}
		return
	}

	if *imagesize == 0 {
		fmt.Println("need imagesize > 0 ")
		stopChan <- struct{}{}
		return
	}

	if len(*poolname) == 0 {
		fmt.Println(" need  poolname  ")
		stopChan <- struct{}{}
		return
	}

	if *imagesize < 4194304 {
		fmt.Println(" need  imagesize >= 4MB  ")
		stopChan <- struct{}{}
		return
	}
	request := &msg.Request{
		Union: &msg.Request_ResizeImageRequest{
			ResizeImageRequest: &msg.ResizeImageRequest{
				Poolname:  string(*poolname),
				Imagename: *imageName,
				Size_:     int64(*imagesize),
			},
		},
	}

	// Marshal the GetOsdMapRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling GetOsdMapRequest:", err)
		return
	}

	// Send the CreatePGRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending GetOsdMapRequest:", err)
		return
	}
}

func ClientSendGetImageRequest(conn net.Conn, stopChan chan<- struct{}) {

	if len(*imageName) == 0 {
		fmt.Println("request need imagename ")
		stopChan <- struct{}{}
		return
	}
	if len(*poolname) == 0 {
		fmt.Println(" need  poolname  ")
		stopChan <- struct{}{}
		return
	}
	request := &msg.Request{
		Union: &msg.Request_Get_ImageInfo_Request{
			Get_ImageInfo_Request: &msg.GetImageInfoRequest{
				Poolname:  string(*poolname),
				Imagename: *imageName,
			},
		},
	}

	// Marshal the GetOsdMapRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling GetOsdMapRequest:", err)
		return
	}

	// Send the CreatePGRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending GetOsdMapRequest:", err)
		return
	}
}

func clientSendOutOsd(conn net.Conn, osdid int) {
	// Create a OsdOutRequest
	request := &msg.Request{
		Union: &msg.Request_OsdOutRequest{
			OsdOutRequest: &msg.OsdOutRequest{
				Osdid: int32(osdid),
			},
		},
	}

	// Marshal the OsdOutRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling OsdOutRequest:", err)
		return
	}

	// Send the OsdOutRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending OsdOutRequest:", err)
		return
	}
}

func clientSendInOsd(conn net.Conn, osdid int) {
	// Create a OsdInRequest
	request := &msg.Request{
		Union: &msg.Request_OsdInRequest{
			OsdInRequest: &msg.OsdInRequest{
				Osdid: int32(osdid),
			},
		},
	}

	// Marshal the OsdInRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling OsdInRequest:", err)
		return
	}

	// Send the OsdInRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending OsdInRequest:", err)
		return
	}
}

func clientSendNoReblance(conn net.Conn, set bool) {
	// Create a NoReblanceRequest
	request := &msg.Request{
		Union: &msg.Request_NoReblanceRequest{
			NoReblanceRequest: &msg.NoReblanceRequest{
				Set: set,
			},
		},
	}

	// Marshal the NoReblanceRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling NoReblanceRequest:", err)
		return
	}

	// Send the NoReblanceRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending NoReblanceRequest:", err)
		return
	}
}

func clientSendNoOut(conn net.Conn, set bool) {
	// Create a NoOutRequest
	request := &msg.Request{
		Union: &msg.Request_NoOutRequest{
			NoOutRequest: &msg.NoOutRequest{
				Set: set,
			},
		},
	}

	// Marshal the NoOutRequest
	data, err := proto.Marshal(request)
	if err != nil {
		log.Println("Error marshaling NoOutRequest:", err)
		return
	}

	// Send the NoOutRequest to the server
	_, err = conn.Write(data)
	if err != nil {
		log.Println("Error sending NoOutRequest:", err)
		return
	}
}

func main() {
	// (TODO)write command line args parse code
	// Define the command line arguments
	monitor_endpoint = flag.String("endpoint", "127.0.0.1:3333", "monitor server endpoint")
	supported_op := "supported: watchclustermap, getclustermap, fakeapplyid, fakebootosd, fakestartcluster, " + 
	        "createpool, deletepool, listpools, getosdmap, getpgmap, watchosdmap, watchpgmap, fakestoposd, " +
	        "createimage, removeimage, resizeimage, getimage, outosd, inosd"
	op = flag.String("op", "", supported_op)
	set = flag.String("set", "", "supported: noReblance, noout")
	unset = flag.String("unset", "", "supported: noReblance, noout")

	poolname = flag.String("poolname", "", "pool name you want to get")
	osdid = flag.Int("osdid", 0, "osd id to boot")
	port = flag.Int("port", 0, "port of this osd")
	pgcount = flag.Int("pgcount", 0, "pgcount of this pool")
	hostname = flag.String("hostname", "localhost", "hostname of this osd")
	address = flag.String("address", "127.0.0.1", "address of this osd")
	size = flag.Int("size", 0, "size of this osd")
	pgsize = flag.Int("pgsize", 3, "pg size of this pool")
	uid = flag.String("uuid", "", "uuid of this osd")
	fakeOsdCount = flag.Int("osdcount", 0, "count of osds in the cluster")
	fakeHostCount = flag.Int("hostcount", 0, "count of hosts in the cluster")
	version = flag.Int64("version", -1, "our current version ")
	imageName = flag.String("imagename", "", "image name you want get")
	//poolname = flag.String("poolname", 0, "pool id you want operate")
	imagesize = flag.Int("imagesize", 0, "image size")
	object_size = flag.Int("objectsize", 4194304, "object size")

	// Parse the command line arguments
	flag.Parse()
	if *op != "watchclustermap" && *op != "getclustermap" && *op != "createpool" && *op != "getosdmap" && 
			*op != "getpgmap" && *op != "listpools" && *op != "fakeapplyid" && *op != "fakebootosd" && 
			*op != "fakestartcluster" && *op != "watchosdmap" && *op != "watchpgmap" && *op != "fakestoposd" && 
			*op != "deletepool" && *op != "createimage" && *op != "removeimage" && *op != "resizeimage" && 
			*op != "getimage" && *op != "outosd" && *op != "inosd" {
		if *set != "noReblance" && *set != "noout" && *unset != "noReblance" && *unset != "noout"{
			log.Fatal("unsupported operation")
		}
	}

	// Connect to the monitor
	conn, err := net.Dial("tcp", *monitor_endpoint)

	if err != nil {
		log.Fatal("Error connecting to server:", err)
	}

	defer conn.Close()

	ctx, cancelFunc := context.WithCancel(context.Background())

	defer cancelFunc()

	//response chan Data
	stopChan := make(chan struct{})
	responseChan := make(chan ResponseChanData)

	switch *op {
	case "fakestartcluster":
		if *fakeOsdCount == 0 || *fakeHostCount == 0 {
			log.Fatal("must provide osd count and host count")
		}
		// we put osds equally in all the hosts
		go clientSendApplyIDRequest(conn, *uid, *fakeOsdCount)
		go clientSendBootRequest(conn, *fakeHostCount, responseChan)

	case "fakeapplyid":

		if *uid == "" {
			log.Fatal("uuid must not be empty for applyid")
		}

		if *fakeOsdCount != 0 {
			log.Fatal("can't specify osd count when fakebootosd")
		}

		go clientSendApplyIDRequest(conn, *uid, 1)
	case "fakebootosd":
		if *hostname == "" || *port == 0 || *size == 0 || *uid == "" || *address == "" {
			log.Fatal("lack of arguments for fakebootosd")
		}

		if *fakeOsdCount != 0 {
			log.Fatal("can't specify osd count when fakebootosd")
		}
		go clientSendBootRequest(conn, 1, responseChan)
	case "fakestoposd":
		if *osdid == 0 {
			log.Fatal("lack of osdid for fakestoposd")
		}

		go clientSendStopRequest(conn)

	case "createpool":
		if *poolname == "" {
			log.Fatal("poolname is empty")
		}
		if *pgcount == 0 {
			log.Fatal("pgcount is empty")
		}
		go sendCreatePoolRequest(conn)
	case "deletepool":
		if *poolname == "" {
			log.Fatal("poolname is empty")
		}
		go sendDeletePoolRequest(conn)
	case "getosdmap":
		go clientSendGetOsdMapRequest(conn, *version)
	case "getclustermap":
		poolpgmapversion = make(map[int32]int64)
		mypgmap = make(map[int32]*msg.PGInfos)
		go clientSendGetClusterMapRequest(conn, false)
	case "watchclustermap":
		poolpgmapversion = make(map[int32]int64)
		mypgmap = make(map[int32]*msg.PGInfos)
		go clientSendGetClusterMapRequest(conn, true)
		go printOsdMap()
		go printPgMap()

	case "watchosdmap":
		osdmapversion = -1
		go clientWatchOsdMapRequest(conn)
		go printOsdMap()

	case "watchpgmap":
		poolpgmapversion = make(map[int32]int64)
		mypgmap = make(map[int32]*msg.PGInfos)
		go clientWatchPgMapRequest(conn)
		go printPgMap()
	case "getpgmap":
		poolpgmapversion = make(map[int32]int64)
		mypgmap = make(map[int32]*msg.PGInfos)
		go clientSendGetPgMapRequest(conn)
	case "listpools":
		go sendListPoolsRequest(conn)
	case "createimage":
		//fmt.Println("create image")
		go ClientSendCreateImageRequest(conn, stopChan)
	case "removeimage":
		//fmt.Println("remove image")
		go ClientSendRemoveImageRequest(conn, stopChan)
	case "resizeimage":
		//fmt.Println("resize image")
		go ClientSendResizeImageRequest(conn, stopChan)
	case "getimage":
		//fmt.Println("get image")
		go ClientSendGetImageRequest(conn, stopChan)
	case "outosd":
		if *osdid == 0 {
			log.Fatal("lack of osdid for outosd")
		}
		go clientSendOutOsd(conn, *osdid)
	case "inosd":
		if *osdid == 0 {
			log.Fatal("lack of osdid for inosd")
		}
		go clientSendInOsd(conn, *osdid)		
	}

	if *op == "" {
		switch *set {
		case "noReblance":
			log.Println("set noReblance")	
			go clientSendNoReblance(conn, true)
		case "noout":
			log.Println("set noout")
			go clientSendNoOut(conn, true)
		} 

		if *set == "" {
			switch *unset {
			case "noReblance":
				log.Println("unset noReblance")
				go clientSendNoReblance(conn, false)
			case "noout":
				log.Println("set noout")
				go clientSendNoOut(conn, false)
			}
		}
	}

	go clientHandleResponses(ctx, conn, stopChan, responseChan)

	// Keep the main goroutine running
	for {
		select {
		case <-stopChan:
			return
		}

	}
}
