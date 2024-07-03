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
	"bytes"
	"context"
	"encoding/binary"
	"flag"
	"fmt"
	"monitor/config"
	"monitor/election"
	"monitor/etcdapi"
	"monitor/leader"
	"monitor/log"
	"monitor/msg"
	"monitor/osd"
	"net"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"sync"
	"syscall"
	"time"

	"github.com/golang/protobuf/proto"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

type Connection struct {
	conn          net.Conn
	requestCount  int
	lastUpdatedAt time.Time
}

var (
	connections sync.Map
	totalRPS    prometheus.Gauge
)

func handleRequest(request *msg.Request, ctx context.Context, conn net.Conn, client *etcdapi.EtcdClient) error {
	switch payload := request.Union.(type) {
	case *msg.Request_CreatePoolRequest:
		// Access the fields of the CreatePoolRequest
		log.Info(ctx, "Received CreatePoolRequest")
		pn := payload.CreatePoolRequest.GetName()
		ps := payload.CreatePoolRequest.GetPgsize()
		pc := payload.CreatePoolRequest.GetPgcount()
		fd := payload.CreatePoolRequest.GetFailuredomain()
		root := payload.CreatePoolRequest.GetRoot()

		cperr := msg.CreatePoolErrorCode_createPoolOk

		pid, err := osd.ProcessCreatePoolMessage(ctx, client, pn, int(ps), int(pc), fd, root)
		if err != nil {
			if err == osd.EPoolAlreadyExists {
				cperr = msg.CreatePoolErrorCode_poolNameExists
			} else if err == osd.EFailureDomainNeedNotSatisfied {
				cperr = msg.CreatePoolErrorCode_failureDomainNeedNotSatisfied
			} else if err == osd.ENoEnoughOsd {
				cperr = msg.CreatePoolErrorCode_noEnoughOsd
			} else if err == osd.EInternalError {
				cperr = msg.CreatePoolErrorCode_internalError
			}
		}

		response := &msg.Response{
			Union: &msg.Response_CreatePoolResponse{
				CreatePoolResponse: &msg.CreatePoolResponse{
					Poolid:    int32(pid),
					Errorcode: cperr,
				},
			},
		}

		// Marshal the response
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_DeletePoolRequest:
		// Access the fields of the CreatePoolRequest
		log.Info(ctx, "Received CreatePoolRequest")
		pn := payload.DeletePoolRequest.GetName()

		isOk := true
		err := osd.ProcessDeletePoolMessage(ctx, client, pn)
		if err != nil {
			isOk = false
		}

		response := &msg.Response{
			Union: &msg.Response_DeletePoolResponse{
				DeletePoolResponse: &msg.DeletePoolResponse{
					Ok: isOk,
				},
			},
		}

		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_ListPoolsRequest:
		log.Info(ctx, "Received ListPoolsRequest")

		pis, _ := osd.ProcessListPoolsMessage(ctx)
		response := &msg.Response{
			Union: &msg.Response_ListPoolsResponse{
				ListPoolsResponse: &msg.ListPoolsResponse{
					Pi: pis,
				},
			},
		}

		// Marshal the response
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_ApplyIdRequest:
		// Access the fields of the ApplyIdRequest
		log.Info(ctx, "Received ApplyIdRequest")
		uuid := payload.ApplyIdRequest.GetUuid()

		oid, err := osd.ProcessApplyIDMessage(ctx, client, uuid)
		if err != nil {
			oid = -1
		}

		// Create a BootResponse
		response := &msg.Response{
			Union: &msg.Response_ApplyIdResponse{
				ApplyIdResponse: &msg.ApplyIDResponse{
					Id:   int32(oid),
					Uuid: uuid, //we should send the uuid back for redundancy
				},
			},
		}

		// Marshal the response
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_BootRequest:
		// Access the fields of the BootRequest
		log.Info(ctx, "Received BootRequest")
		id := payload.BootRequest.GetOsdId()
		uuid := payload.BootRequest.GetUuid()

		//protoc-gen-gogo seems different name here
		size := payload.BootRequest.GetSize_()
		port := payload.BootRequest.GetPort()
		addr := payload.BootRequest.GetAddress()
		host := payload.BootRequest.GetHost()

		errnum := osd.ProcessBootMessage(ctx, client, id, uuid, size, port, host, addr)

		// Create a BootResponse
		response := &msg.Response{
			Union: &msg.Response_BootResponse{
				BootResponse: &msg.BootResponse{
					Result: int32(errnum),
				},
			},
		}

		// Marshal the BootResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_LeaderBeElectedRequest:
		leaderId := payload.LeaderBeElectedRequest.GetLeaderId()
		poolId := payload.LeaderBeElectedRequest.GetPoolId()
		pgId := payload.LeaderBeElectedRequest.GetPgId()
		osdListM := payload.LeaderBeElectedRequest.GetOsdList()
		newOsdListM := payload.LeaderBeElectedRequest.GetNewOsdList()
		log.Info(ctx, "Received LeaderBeElectedRequest, pg ", poolId, ".", pgId)

		var osdList []int
		var newOsdList []int
		for _, val := range osdListM {
			osdList = append(osdList, int(val))
		}
		for _, val := range newOsdListM {
			newOsdList = append(newOsdList, int(val))
		}
		isOk := true
		err := osd.ProcessLeaderBeElected(ctx, client, int(leaderId), poolId, pgId, osdList, newOsdList)
		if err != nil {
			isOk = false
		}

		response := &msg.Response{
			Union: &msg.Response_LeaderBeElectedResponse{
				LeaderBeElectedResponse: &msg.LeaderBeElectedResponse{
					Ok: isOk,
				},
			},
		}

		// Marshal the LeaderBeElectedResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_PgMemberChangeFinishRequest:
		result := payload.PgMemberChangeFinishRequest.GetResult()
		poolId := payload.PgMemberChangeFinishRequest.GetPoolId()
		pgId := payload.PgMemberChangeFinishRequest.GetPgId()
		osdListM := payload.PgMemberChangeFinishRequest.GetOsdList()
		log.Info(ctx, "Received PgMemberChangeFinishRequest, pg ", poolId, ".", pgId)

		var osdList []int
		for _, val := range osdListM {
			osdList = append(osdList, int(val))
		}
		isOk := true
		err := osd.ProcessPgMemberChangeFinish(ctx, client, int(result), poolId, pgId, osdList)
		if err != nil {
			isOk = false
		}

		response := &msg.Response{
			Union: &msg.Response_PgMemberChangeFinishResponse{
				PgMemberChangeFinishResponse: &msg.PgMemberChangeFinishResponse{
					Ok: isOk,
				},
			},
		}

		// Marshal the PgMemberChangeFinishResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_OsdStopRequest:
		log.Info(ctx, "Received StopRequest")
		id := payload.OsdStopRequest.GetId()

		ok := osd.ProcessOsdStopMessage(ctx, client, id)

		// Create a StopResponse
		response := &msg.Response{
			Union: &msg.Response_OsdStopResponse{
				OsdStopResponse: &msg.OsdStopResponse{
					Ok: ok,
				},
			},
		}

		// Marshal the OsdStopResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_HeartbeatRequest:
		log.Info(ctx, "Received HeartbeatRequest:")
		log.Info(ctx, "ID:", payload.HeartbeatRequest.GetId())

		// Create a HeartbeatResponse
		response := &msg.Response{
			Union: &msg.Response_HeartbeatResponse{
				HeartbeatResponse: &msg.HeartbeatResponse{
					Ok: true,
				},
			},
		}

		// Marshal the HeartbeatResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_GetClusterMapRequest:
		log.Info(ctx, "Received GetClusterMapRequest:")
		pv := payload.GetClusterMapRequest.GpmRequest.GetPoolVersions()

		gpmr, err := osd.ProcessGetPgMapMessage(ctx, pv)

		cv := payload.GetClusterMapRequest.GomRequest.GetCurrentversion()
		osdId := payload.GetClusterMapRequest.GomRequest.GetOsdid()
		odi, mapVersion, rc := osd.ProcessGetOsdMapMessage(ctx, cv, osdId)

		// Create a GetPgMapResponse
		response := &msg.Response{
			Union: &msg.Response_GetClusterMapResponse{
				GetClusterMapResponse: &msg.GetClusterMapResponse{
					GomResponse: &msg.GetOsdMapResponse{
						Errorcode:     rc,
						Osdmapversion: mapVersion,
						Osds:          odi,
					},
					GpmResponse: gpmr,
				},
			},
		}

		// Marshal the GetPgMapResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_GetPgmapRequest:
		log.Info(ctx, "Received GetPgMapRequest:")
		pv := payload.GetPgmapRequest.GetPoolVersions()

		gpmr, err := osd.ProcessGetPgMapMessage(ctx, pv)

		// Create a GetPgMapResponse
		response := &msg.Response{
			Union: &msg.Response_GetPgmapResponse{
				GetPgmapResponse: gpmr,
			},
		}

		// Marshal the GetPgMapResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}
	case *msg.Request_GetOsdmapRequest:
		log.Info(ctx, "Received GetOsdMapRequest:")
		cv := payload.GetOsdmapRequest.GetCurrentversion()
		osdId := payload.GetOsdmapRequest.GetOsdid()
		odi, mapVersion, rc := osd.ProcessGetOsdMapMessage(ctx, cv, osdId)

		// Create a GetPgMapResponse
		response := &msg.Response{
			Union: &msg.Response_GetOsdmapResponse{
				GetOsdmapResponse: &msg.GetOsdMapResponse{
					Errorcode:     rc,
					Osdmapversion: mapVersion,
					Osds:          odi,
				},
			},
		}

		// Marshal the GetPgMapResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}
	case *msg.Request_CreateImageRequest:

		log.Info(ctx, "Received CreateImageRequest")
		poolName := payload.CreateImageRequest.GetPoolname()
		imageName := payload.CreateImageRequest.GetImagename()
		size := payload.CreateImageRequest.GetSize_()
		objectSize := payload.CreateImageRequest.GetObjectSize()
		//CreateImageRequest

		createImgErrCode := osd.ProcessCreateImageMessage(ctx, client, imageName, poolName, size, objectSize)

		var imageInfo = &msg.ImageInfo{}
		imageInfo.Poolname = poolName
		imageInfo.Imagename = imageName
		imageInfo.Size_ = size
		imageInfo.ObjectSize = objectSize

		response := &msg.Response{
			Union: &msg.Response_CreateImageResponse{
				CreateImageResponse: &msg.CreateImageResponse{
					Errorcode: createImgErrCode,
					ImageInfo: imageInfo,
				},
			},
		}
		// Marshal the BootResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_RemoveImageRequest:
		log.Info(ctx, "Received RemoveImageRequest")
		poolName := payload.RemoveImageRequest.GetPoolname()
		imageName := payload.RemoveImageRequest.GetImagename()

		errRE, info := osd.ProcessRemoveImageMessage(ctx, client, imageName, poolName)
		var imageInfo = &msg.ImageInfo{}
		if errRE == msg.RemoveImageErrorCode_removeImageOk {
			imageInfo.Poolname = info.Poolname
			imageInfo.Imagename = info.Imagename
			imageInfo.Size_ = info.Imagesize
			imageInfo.ObjectSize = info.Objectsize
		}
		response := &msg.Response{
			Union: &msg.Response_RemoveImageResponse{
				RemoveImageResponse: &msg.RemoveImageResponse{
					Errorcode: errRE,
					ImageInfo: imageInfo,
				},
			},
		}

		// Marshal the BootResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_ResizeImageRequest:
		log.Info(ctx, "Received ResizeImageRequest")
		poolName := payload.ResizeImageRequest.GetPoolname()
		imageName := payload.ResizeImageRequest.GetImagename()
		size := payload.ResizeImageRequest.GetSize_()

		var imageInfo = &msg.ImageInfo{}
		errCode, info := osd.ProcessResizeImageMessage(ctx, client, imageName, poolName, size)
		//CreateImageRequest
		if errCode == msg.ResizeImageErrorCode_resizeImageOk {
			imageInfo.Poolname = info.Poolname
			imageInfo.Imagename = info.Imagename
			imageInfo.Size_ = info.Imagesize
			imageInfo.ObjectSize = info.Objectsize

		}

		response := &msg.Response{
			Union: &msg.Response_ResizeImageResponse{
				ResizeImageResponse: &msg.ResizeImageResponse{
					Errorcode: errCode,
					ImageInfo: imageInfo,
				},
			},
		}
		// Marshal the BootResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}
	case *msg.Request_Get_ImageInfo_Request:
		log.Info(ctx, "Received Get_ImageInfo_Request")

		poolName := payload.Get_ImageInfo_Request.GetPoolname()
		imageName := payload.Get_ImageInfo_Request.GetImagename()

		var imageInfo = &msg.ImageInfo{}
		errCode, info := osd.ProcessGetImageMessage(ctx, imageName, poolName)

		if errCode == msg.GetImageErrorCode_getImageOk {
			imageInfo.Poolname = info.Poolname
			imageInfo.Imagename = info.Imagename
			imageInfo.Size_ = info.Imagesize
			imageInfo.ObjectSize = info.Objectsize
		}

		response := &msg.Response{
			Union: &msg.Response_GetImageInfoResponse{
				GetImageInfoResponse: &msg.GetImageInfoResponse{
					Errorcode: errCode,
					ImageInfo: imageInfo,
				},
			},
		}
		// Marshal the BootResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_OsdOutRequest:
		log.Info(ctx, "Received OsdOutRequest")
		osdid := payload.OsdOutRequest.GetOsdid()

		ok := osd.ProcessOsdOutMessage(ctx, client, osdid)

		// Create a OsdOutResponse
		response := &msg.Response{
			Union: &msg.Response_OsdOutResponse{
				OsdOutResponse: &msg.OsdOutResponse{
					Ok: ok,
				},
			},
		}

		// Marshal the OsdOutResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_OsdInRequest:
		log.Info(ctx, "Received OsdInRequest")
		osdid := payload.OsdInRequest.GetOsdid()

		ok := osd.ProcessOsdInMessage(ctx, client, osdid)

		// Create a OsdInResponse
		response := &msg.Response{
			Union: &msg.Response_OsdInResponse{
				OsdInResponse: &msg.OsdInResponse{
					Ok: ok,
				},
			},
		}

		// Marshal the OsdInResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_NoReblanceRequest:
		log.Info(ctx, "Received NoReblanceRequest")
		set := payload.NoReblanceRequest.GetSet()

		ok := osd.ProcessNoReblanceMessage(ctx, client, set)

		// Create a NoReblanceResponse
		response := &msg.Response{
			Union: &msg.Response_NoReblanceResponse{
				NoReblanceResponse: &msg.NoReblanceResponse{
					Ok: ok,
				},
			},
		}

		// Marshal the NoReblanceResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_NoOutRequest:
		log.Info(ctx, "Received NoOutRequest")
		set := payload.NoOutRequest.GetSet()

		ok := osd.ProcessNoOutMessage(ctx, client, set)

		// Create a NoOutResponse
		response := &msg.Response{
			Union: &msg.Response_NoOutResponse{
				NoOutResponse: &msg.NoOutResponse{
					Ok: ok,
				},
			},
		}

		// Marshal the NoOutResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}
			
	case *msg.Request_GetClusterStatusRequest:
		log.Info(ctx, "Received GetClusterStatusRequest")

		status := osd.PorcessGetClusterStatusMessage(ctx, client)

		response := &msg.Response{
			Union: &msg.Response_GetClusterStatusResponse{
				GetClusterStatusResponse: status,
			},
		}

		// Marshal the GetClusterStatusResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	case *msg.Request_DataStatisticsRequest:
		log.Info(ctx, "Received DataStatisticsRequest")

		data := payload.DataStatisticsRequest.GetData();

		ok := osd.PorcessDataStatisticsMessage(ctx, client, &data)

		response := &msg.Response{
			Union: &msg.Response_DataStatisticsResponse{
				DataStatisticsResponse: &msg.DataStatisticsResponse {
					Ok: ok,
				},
			},
		}

		// Marshal the DataStatisticsResponse
		responseData, err := proto.Marshal(response)
		if err != nil {
			log.Error(ctx, "Error marshaling response:", err)
			return err
		}

		// Write the response data back to the client
		_, err = conn.Write(responseData)
		if err != nil {
			log.Error(ctx, "Error writing response:", err)
			return err
		}

	default:
		log.Info(ctx, "Unknown payload type")
		return fmt.Errorf("Unknown payload type")
	}

	return nil
}

func handleConnection(ctx context.Context, conn net.Conn, client *etcdapi.EtcdClient) {
	defer connections.Delete(conn.RemoteAddr().String())
	defer conn.Close()

	connection := &Connection{
		conn:          conn,
		requestCount:  0,
		lastUpdatedAt: time.Now(),
	}

	// Store the connection in the map
	connections.Store(conn.RemoteAddr().String(), connection)

	const msg_len_size uint64 = 8
	var msg_len uint64
	buffer := make([]byte, 65535)

	read_bytes := uint64(0)
	should_read_bytes := uint64(msg_len_size)
	is_read_len := true

	for {
		select {
		case <-ctx.Done():
			log.Info(ctx, "handleConnection: going to quit\r\n")
			return
		default:
			n, err := conn.Read(buffer[read_bytes:should_read_bytes])
			log.Debug(ctx, "handleConnection: read bytes:", n, ", is_read_len is", is_read_len, ", should_read_bytes is", should_read_bytes, ", read_bytes is ", read_bytes)
			if err != nil {
				log.Error(ctx, "Error reading from connection:", err, ", close this conn")
				return
			}

			read_bytes += uint64(n)
			if read_bytes < should_read_bytes {
				log.Debug(ctx, "wait reading all bytes from connection, read", n, "bytes, wait", should_read_bytes-read_bytes)
				continue
			}
			log.Debug(ctx, "read all bytes from connection, whose size is", should_read_bytes)

			read_bytes = read_bytes - should_read_bytes
			if read_bytes < 0 {
				log.Error(ctx, "read_bytes is", read_bytes, "should_read_bytes is", should_read_bytes, ", close this conn")
				return
			}
			if is_read_len {
				is_read_len = !is_read_len
				reader := bytes.NewReader(buffer[:msg_len_size])
				err := binary.Read(reader, binary.LittleEndian, &msg_len)
				if err != nil {
					log.Error(ctx, "Error parsing message length:", err, ", close this conn")
					return
				}
				should_read_bytes = msg_len
				log.Debug(ctx, "parsed message length:", msg_len)
				continue
			}

			is_read_len = !is_read_len
			connection.requestCount++
			connection.lastUpdatedAt = time.Now()
			request := &msg.Request{}
			err = proto.Unmarshal(buffer[:should_read_bytes], request)
			should_read_bytes = msg_len_size
			if err != nil {
				//discard this message
				log.Error(ctx, "Error unmarshalling request:", err, ", close this conn")
				return
			}

			err = handleRequest(request, ctx, conn, client)
			if err != nil {
				log.Error(ctx, "Error handling request:", err, ", close this conn")
				return
			}
		}
	}
}

func measureRPS(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			// Context canceled, stop measuring RPS
			return
		case <-ticker.C:
			totalRPS.Set(0.0)

			connections.Range(func(key, value interface{}) bool {
				connection := value.(*Connection)

				elapsed := time.Since(connection.lastUpdatedAt)
				rps := float64(connection.requestCount) / elapsed.Seconds()

				totalRPS.Add(rps)
				connection.requestCount = 0

				return true
			})
		}
	}
}

func handleConnections(ctx context.Context, listener net.Listener, c *etcdapi.EtcdClient) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				// Context canceled, stop accepting new connections
				return
			default:
				log.Error(ctx, "Error accepting connection:", err)
				continue
			}
		}

		go handleConnection(ctx, conn, c)
	}
}

func waitForShutdownSignal(ctx context.Context, cancel context.CancelFunc) {
	// Create a channel to listen for OS signals
	sigChan := make(chan os.Signal, 1)

	// Notify the channel on SIGINT and SIGTERM
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	// Block until a signal is received
	<-sigChan
	log.Info(ctx, "server stopped when got stopping signal")
	cancel()
}

func startTcpServer(ctx context.Context, c *etcdapi.EtcdClient) {

	// Create a Prometheus gauge for total RPS
	totalRPS = prometheus.NewGauge(prometheus.GaugeOpts{
		Name: "total_rps",
		Help: "Total requests per second across all connections",
	})

	// Register the gauge with the Prometheus default registry
	prometheus.MustRegister(totalRPS)
	hp := fmt.Sprintf("%s:%d", config.CONFIG.Address, config.CONFIG.Port)

	// Start the TCP server
	listener, err := net.Listen("tcp", hp)
	if err != nil {
		log.Error(ctx, "Error starting TCP server:", err)
	}

	log.Info(ctx, "TCP server started. Listening on ", hp)

	// Start measuring requests per second
	go measureRPS(ctx)

	// Start the Prometheus exporter
	go func() {
		http.Handle("/metrics", promhttp.Handler())
		l := fmt.Sprintf("localhost:%d", config.CONFIG.PrometheusPort)
		http.ListenAndServe(l, nil)
	}()

	// Handle connections in a separate goroutine
	go handleConnections(ctx, listener, c)

	for {
		select {
		case <-ctx.Done():
			log.Info(ctx, "after ctx.Done")

			// Close the listener to stop accepting new connections
			listener.Close()
			log.Info(ctx, "after Close")

			// Wait for existing connections to be closed
			connections.Range(func(key, value interface{}) bool {
				connection := value.(*Connection)
				connection.conn.Close()
				return true
			})

			log.Info(ctx, "after connections closed")
			log.Info(ctx, "TCP Server stopped")
			return
		default:
			time.Sleep(1 * time.Second)
		}
	}
}

func leaderCallback(whoAmI string, ctx context.Context, c *etcdapi.EtcdClient) {
	log.Info(ctx, "i'm the leader, i'm ", whoAmI)
	osd.LoadOSDMapFromEtcd(ctx, c)
	osd.LoadPoolConfig(ctx, c)
	osd.LoadImageConfig(ctx, c)
	//这里一定要先LoadClusterStates,再LoadClusterUnprocessedEvent
	osd.LoadClusterStates(ctx, c)
	osd.LoadClusterUnprocessedEvent(ctx, c)
	osd.GetOSDTree(ctx, true, false)
	go osd.CheckOsdHeartbeat(ctx, c)
	go osd.OsdTaskrun(ctx, c)
	go osd.UpdateClusterIos(ctx, c)
	startTcpServer(ctx, c)
}

func main() {
	configPath := flag.String("conf", "/etc/fastblock/fastblock.json", "path of the config file")
	id := flag.String("id", "", "name of the monitor")
	max_core := flag.Int("max-core", 1, "max cpu core can be used")
	flag.Parse()

	if *id == "" {
		panic("Missing parameter id")
	}
	runtime.GOMAXPROCS(*max_core)
	config.SetupConfig(*configPath, *id)

	// Init the only logger.
	log.NewFileLogger(config.CONFIG.LogPath, config.CONFIG.LogLevel)
	defer log.Close()

	ctx, cancelFunc := context.WithCancel(context.Background())

	log.Info(ctx, "read config file: ", configPath)
	log.Info(ctx, "fastblock monitor started with config:", config.CONFIG)

	e, err := etcdapi.NewServer(&config.CONFIG)
	if err != nil {
		log.Error(ctx, "create etcd server failed: ", err.Error())
		panic(err.Error())
	}
	defer e.Close()
	select {
	case <-e.Server.ReadyNotify():
		log.Info(ctx, "embeded etcd server started, continue to start etcd client and other services")
		//try to be leader, if we are not leader, the tcp server and
		c, err := etcdapi.NewEtcdClient(config.CONFIG.EtcdServer)
		if err != nil {
			log.Error(ctx, "failed to start election, error is:", err)
		}

		electionKey := config.CONFIG.ElectionMasterKey
		leaderElection := election.NewLeaderElection(c, electionKey, config.CONFIG.HostName, leaderCallback, leader.FollowerCallback)

		go func() {
			err := leaderElection.Run(ctx)
			if err != nil {
				log.Error(ctx, "failed to start election, error is:", err)
			}
		}()
		waitForShutdownSignal(ctx, cancelFunc)

	case <-time.After(42 * time.Second):
		e.Server.Stop() // trigger a shutdown
		log.Error(ctx, "too long to start etcd server")

	case err := <-e.Err():
		log.Info(ctx, "failed to start etcd server ", err)

	}

}
