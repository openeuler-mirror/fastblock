package rawserver

import (
	"context"
	"fmt"
	"io"
	"monitor/config"
	"monitor/etcdapi"
	"monitor/log"
	"monitor/msg"
	"monitor/osd"
	"monitor/rawproto"
	"net"
)

func Start(ctx context.Context, client *etcdapi.EtcdClient) {
	_ = client
	addr := fmt.Sprintf("%s:%d", config.CONFIG.Address, config.CONFIG.RawPort)
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		log.Error(ctx, "Error starting raw TCP server:", err)
		return
	}
	defer listener.Close()

	log.Info(ctx, "raw TCP server started. Listening on ", addr)
	go handleConnections(ctx, listener)

	<-ctx.Done()
	listener.Close()
	log.Info(ctx, "raw TCP server stopped")
}

func handleConnections(ctx context.Context, listener net.Listener) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				return
			default:
				log.Error(ctx, "Error accepting raw connection:", err)
				continue
			}
		}
		go handleConnection(ctx, conn)
	}
}

func handleConnection(ctx context.Context, conn net.Conn) {
	defer conn.Close()
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		hdr, err := rawproto.ReadHeader(conn)
		if err != nil {
			if err != io.EOF {
				log.Error(ctx, "Error reading raw header:", err)
			}
			return
		}
		body, err := rawproto.ReadBody(conn, hdr.BodyLen)
		if err != nil {
			log.Error(ctx, "Error reading raw body:", err)
			return
		}
		if err := rawproto.ValidateRequestHeader(hdr); err != nil {
			_ = rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInvalidRequest)
			continue
		}
		if err := handleRequest(ctx, conn, hdr, body); err != nil {
			log.Error(ctx, "Error handling raw request:", err)
			_ = rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInternalError)
		}
	}
}

func handleRequest(ctx context.Context, conn net.Conn, hdr *rawproto.Header, body []byte) error {
	switch hdr.Opcode {
	case rawproto.OpGetImageInfo:
		return handleGetImageInfo(ctx, conn, hdr, body)
	case rawproto.OpGetClusterMap:
		return handleGetClusterMap(ctx, conn, hdr, body)
	default:
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInvalidRequest)
	}
}

func handleGetImageInfo(ctx context.Context, conn net.Conn, hdr *rawproto.Header, body []byte) error {
	req, err := rawproto.DecodeGetImageInfoRequest(body)
	if err != nil {
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInvalidRequest)
	}
	code, imageInfo := osd.ProcessGetImageMessage(ctx, req.ImageName, req.PoolName)
	if code != msg.GetImageErrorCode_getImageOk || imageInfo == nil {
		rspHdr, rspBody, encodeErr := rawproto.EncodeGetImageInfoResponse(
			hdr.Seq,
			rawproto.StatusNotFound,
			0,
			0,
			4096,
			0,
			0,
			0,
		)
		if encodeErr != nil {
			return encodeErr
		}
		return rawproto.WriteMessage(conn, rspHdr, rspBody)
	}

	poolID, ok := osd.GetPoolIDByName(imageInfo.Poolname)
	if !ok {
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInternalError)
	}

	rspHdr, rspBody, err := rawproto.EncodeGetImageInfoResponse(
		hdr.Seq,
		rawproto.StatusOK,
		0,
		uint32(poolID),
		4096,
		uint32(imageInfo.Objectsize),
		0,
		uint64(imageInfo.Imagesize),
	)
	if err != nil {
		return err
	}
	return rawproto.WriteMessage(conn, rspHdr, rspBody)
}

func handleGetClusterMap(ctx context.Context, conn net.Conn, hdr *rawproto.Header, body []byte) error {
	req, err := rawproto.DecodeGetClusterMapRequest(body)
	if err != nil {
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInvalidRequest)
	}
	if req.PoolID == 0 {
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInvalidRequest)
	}
	currentOSDMapVersion := osd.GetOSDMapVersion()
	currentPGMapVersion, ok := osd.GetPoolPGMapVersion(int32(req.PoolID))
	if !ok {
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusNotFound)
	}
	if int64(req.OSDMapEpoch) == currentOSDMapVersion &&
		int64(req.PGMapEpoch) == currentPGMapVersion {
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusStaleEpoch)
	}
	pgResp, err := osd.ProcessGetPgMapMessage(ctx, map[int32]int64{int32(req.PoolID): -1})
	if err != nil {
		return rawproto.WriteErrorResponse(conn, hdr, rawproto.StatusInternalError)
	}
	osds, osdMapVersion, rc := osd.ProcessGetOsdMapMessage(ctx, -1, 0)
	status := rawproto.StatusOK
	if rc != msg.OsdMapErrorCode_ok && rc != msg.OsdMapErrorCode_noOsdsExist {
		status = rawproto.StatusInternalError
	}
	pgMapVersion := maxPGMapVersion(pgResp)
	rspHdr, rspBody, err := rawproto.EncodeGetClusterMapResponse(
		hdr.Seq,
		status,
		uint64(osdMapVersion),
		uint64(pgMapVersion),
		osds,
		pgResp,
	)
	if err != nil {
		return err
	}
	return rawproto.WriteMessage(conn, rspHdr, rspBody)
}

func maxPGMapVersion(pgResp *msg.GetPgMapResponse) int64 {
	var maxVersion int64
	if pgResp == nil {
		return 0
	}
	for _, version := range pgResp.GetPoolidPgmapversion() {
		if version > maxVersion {
			maxVersion = version
		}
	}
	return maxVersion
}
