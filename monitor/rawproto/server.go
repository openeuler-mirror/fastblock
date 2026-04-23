package rawproto

import "net"

func WriteErrorResponse(conn net.Conn, req *Header, status uint32) error {
	return WriteMessage(conn, MakeResponseHeader(req, status), nil)
}
