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

func handleConnection(ctx context.Context, conn net.Conn, client *etcdapi.EtcdClient) {
	defer conn.Close()

	connection := &Connection{
		conn:          conn,
		requestCount:  0,
		lastUpdatedAt: time.Now(),
	}

	// Store the connection in the map
	connections.Store(conn.RemoteAddr().String(), connection)

	for {
		select {
		case <-ctx.Done():
			log.Info(ctx, "handleConnection: going to quit\r\n")
			return
		default:
			// Read data from the connection
			buffer := make([]byte, 65536)
			n, err := conn.Read(buffer)
			if err != nil {
				log.Error(ctx, "Error reading from connection:", err)
				return
			}

			log.Info(ctx, "got bytes:", n)

			// Increment the request count for the connection
			connection.requestCount++

			// Update the last update timestamp
			connection.lastUpdatedAt = time.Now()

			// Unmarshal the received data into a Request message
			request := &msg.Request{}
			err = proto.Unmarshal(buffer[:n], request)
			if err != nil {
				//discard this message
				log.Error(ctx, "Error unmarshaling request:", err)
				continue
			}

			// Handle the request based on the message type
			switch payload := request.Union.(type) {
			case *msg.Request_CreatePoolRequest:
				// Access the fields of the CreatePoolRequest
				log.Info(ctx, "Received CreatePoolRequest")
				pn := payload.CreatePoolRequest.GetName()
				ps := payload.CreatePoolRequest.GetPgsize()
				pc := payload.CreatePoolRequest.GetPgcount()
				fd := payload.CreatePoolRequest.GetFailuredomain()
				root := payload.CreatePoolRequest.GetRoot()

				isok := true
				pid, err := osd.ProcessCreatePoolMessage(ctx, client, pn, int(ps), int(pc), fd, root)
				if err != nil {
					pid = -1
					isok = false
				}

				response := &msg.Response{
					Union: &msg.Response_CreatePoolResponse{
						CreatePoolResponse: &msg.CreatePoolResponse{
							Poolid: int32(pid),
							Ok:     isok,
						},
					},
				}

				// Marshal the Apply
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
				}

			case *msg.Request_DeletePoolRequest:
				// Access the fields of the CreatePoolRequest
				log.Info(ctx, "Received CreatePoolRequest")
				pn := payload.DeletePoolRequest.GetName()

				isok := true
				err := osd.ProcessDeletePoolMessage(ctx, client, pn)
				if err != nil {
					isok = false
				}

				response := &msg.Response{
					Union: &msg.Response_DeletePoolResponse{
						DeletePoolResponse: &msg.DeletePoolResponse{
							Ok: isok,
						},
					},
				}

				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
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

				// Marshal the Apply
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
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

				// Marshal the Apply
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
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

				isok := true
				err := osd.ProcessBootMessage(ctx, client, id, uuid, size, port, host, addr)
				if err != nil {
					isok = false
				}

				// Create a BootResponse
				response := &msg.Response{
					Union: &msg.Response_BootResponse{
						BootResponse: &msg.BootResponse{
							Ok: isok,
						},
					},
				}

				// Marshal the BootResponse
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
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
					continue
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					continue
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
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
				}

			case *msg.Request_GetClusterMapRequest:
				log.Info(ctx, "Received GetClusterMapRequest:")
				pv := payload.GetClusterMapRequest.GpmRequest.GetPoolVersions()

				gpmr, err := osd.ProcessGetPgMapMessage(ctx, pv)

				cv := payload.GetClusterMapRequest.GomRequest.GetCurrentversion()
				osdid := payload.GetClusterMapRequest.GomRequest.GetOsdid()
				odi, mapversion, rc := osd.ProcessGetOsdMapMessage(ctx, cv, osdid)

				// Create a GetPgMapResponse
				response := &msg.Response{
					Union: &msg.Response_GetClusterMapResponse{
						GetClusterMapResponse: &msg.GetClusterMapResponse{
							GomResponse: &msg.GetOsdMapResponse{
								Errorcode:     rc,
								Osdmapversion: mapversion,
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
					continue
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					continue
				}

			case *msg.Request_GetPgmapRequest:
				log.Info(ctx, "Received GetPgmapRequest:")
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
					continue
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					continue
				}
			case *msg.Request_GetOsdmapRequest:
				log.Info(ctx, "Received GetOsdmapRequest:")
				cv := payload.GetOsdmapRequest.GetCurrentversion()
				osdid := payload.GetOsdmapRequest.GetOsdid()
				odi, mapversion, rc := osd.ProcessGetOsdMapMessage(ctx, cv, osdid)

				// Create a GetPgMapResponse
				response := &msg.Response{
					Union: &msg.Response_GetOsdmapResponse{
						GetOsdmapResponse: &msg.GetOsdMapResponse{
							Errorcode:     rc,
							Osdmapversion: mapversion,
							Osds:          odi,
						},
					},
				}

				// Marshal the GetPgMapResponse
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					continue
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					continue
				}
			case *msg.Request_CreateImageRequest:

				log.Info(ctx, "Received CreateImageRequest")
				poolname := payload.CreateImageRequest.GetPoolname()
				imagename := payload.CreateImageRequest.GetImagename()
				size := payload.CreateImageRequest.GetSize_()
				object_size := payload.CreateImageRequest.GetObjectSize()
				//CreateImageRequest

				create_img_rc := osd.ProcessCreateImageMessage(ctx, client, imagename, poolname, size, object_size)

				var imageinfo = &msg.ImageInfo{}
				imageinfo.Poolname = poolname
				imageinfo.Imagename = imagename
				imageinfo.Size_ = size
				imageinfo.ObjectSize = object_size

				response := &msg.Response{
					Union: &msg.Response_CreateImageResponse{
						CreateImageResponse: &msg.CreateImageResponse{
							Errorcode: create_img_rc,
							ImageInfo: imageinfo,
						},
					},
				}
				// Marshal the BootResponse
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
				}

			case *msg.Request_RemoveImageRequest:
				log.Info(ctx, "Received RemoveImageRequest")
				poolname := payload.RemoveImageRequest.GetPoolname()
				imagename := payload.RemoveImageRequest.GetImagename()

				errRE, info := osd.ProcessRemoveImageMessage(ctx, client, imagename, poolname)
				var imageinfo = &msg.ImageInfo{}
				if errRE == msg.RemoveImageErrorCode_removeImageOk {
					imageinfo.Poolname = info.Poolname
					imageinfo.Imagename = info.Imagename
					imageinfo.Size_ = info.Imagesize
					imageinfo.ObjectSize = info.Objectsize
				}
				response := &msg.Response{
					Union: &msg.Response_RemoveImageResponse{
						RemoveImageResponse: &msg.RemoveImageResponse{
							Errorcode: errRE,
							ImageInfo: imageinfo,
						},
					},
				}

				// Marshal the BootResponse
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
				}

			case *msg.Request_ResizeImageRequest:
				log.Info(ctx, "Received ResizeImageRequest")
				poolname := payload.ResizeImageRequest.GetPoolname()
				imagename := payload.ResizeImageRequest.GetImagename()
				size := payload.ResizeImageRequest.GetSize_()

				var imageinfo = &msg.ImageInfo{}
				err_code, info := osd.ProcessResizeImageMessage(ctx, client, imagename, poolname, size)
				//CreateImageRequest
				if err_code == msg.ResizeImageErrorCode_resizeImageOk {
					imageinfo.Poolname = info.Poolname
					imageinfo.Imagename = info.Imagename
					imageinfo.Size_ = info.Imagesize
					imageinfo.ObjectSize = info.Objectsize

				}

				response := &msg.Response{
					Union: &msg.Response_ResizeImageResponse{
						ResizeImageResponse: &msg.ResizeImageResponse{
							Errorcode: err_code,
							ImageInfo: imageinfo,
						},
					},
				}
				// Marshal the BootResponse
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
				}
			case *msg.Request_Get_ImageInfo_Request:
				log.Info(ctx, "Received Get_ImageInfo_Request")

				poolname := payload.Get_ImageInfo_Request.GetPoolname()
				imagename := payload.Get_ImageInfo_Request.GetImagename()

				var imageinfo = &msg.ImageInfo{}
				err_code, info := osd.ProcessGetImageMessage(ctx, imagename, poolname)

				if err_code == msg.GetImageErrorCode_getImageOk {
					imageinfo.Poolname = info.Poolname
					imageinfo.Imagename = info.Imagename
					imageinfo.Size_ = info.Imagesize
					imageinfo.ObjectSize = info.Objectsize
				}

				response := &msg.Response{
					Union: &msg.Response_GetImageInfoResponse{
						GetImageInfoResponse: &msg.GetImageInfoResponse{
							Errorcode: err_code,
							ImageInfo: imageinfo,
						},
					},
				}
				// Marshal the BootResponse
				responseData, err := proto.Marshal(response)
				if err != nil {
					log.Error(ctx, "Error marshaling response:", err)
					return
				}

				// Write the response data back to the client
				_, err = conn.Write(responseData)
				if err != nil {
					log.Error(ctx, "Error writing response:", err)
					return
				}
			default:
				log.Info(ctx, "Unknown payload type")
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

func leaderCallback(whoami string, ctx context.Context, c *etcdapi.EtcdClient) {
	log.Info(ctx, "i'm the leader, i'm ", whoami)
	osd.LoadOSDStateFromEtcd(ctx, c)
	osd.LoadPoolConfig(ctx, c)
	osd.LoadImageConfig(ctx, c)
	osd.GetOSDTreeUp(ctx)
	go osd.CheckOsdHeartbeat(ctx, c)
	go osd.RecheckPGs(ctx, c)
	startTcpServer(ctx, c)
}

func main() {

	configpath := flag.String("conf", "/etc/fastblock/monitor.toml", "path of the config file")
	flag.Parse()

	config.SetupConfig(*configpath)

	// Init the only logger.
	log.NewFileLogger(config.CONFIG.LogPath, config.CONFIG.LogLevel)
	defer log.Close()

	ctx, cancelFunc := context.WithCancel(context.Background())

	log.Info(ctx, "read config file: ", configpath)
	log.Info(ctx, "fastblock monitor started with config:", config.CONFIG)

	e, err := etcdapi.NewServer(&config.CONFIG)
	if err != nil {
		log.Error(ctx, "create etcd server failed: ", err.Error())
		panic(err.Error())
	}
	defer e.Close()
	select {
	case <-e.Server.ReadyNotify():
		log.Info(ctx, "embeded etcd server started, continue to start etcdclient and other services")
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
		log.Info(ctx, "failed to start etcdserver ", err)

	}

}
