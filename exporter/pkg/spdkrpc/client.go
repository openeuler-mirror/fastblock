package spdkrpc

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
)

type Caller interface {
	Call(ctx context.Context, method string, params any, result any) error
}

type Client struct {
	socketPath string
	dialer     *net.Dialer
}

type request struct {
	JSONRPC string `json:"jsonrpc"`
	ID      int    `json:"id"`
	Method  string `json:"method"`
	Params  any    `json:"params,omitempty"`
}

type response struct {
	ID      int             `json:"id"`
	Error   *ResponseError  `json:"error,omitempty"`
	Result  json.RawMessage `json:"result,omitempty"`
	JSONRPC string          `json:"jsonrpc"`
}

type ResponseError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *ResponseError) Error() string {
	return fmt.Sprintf("spdk rpc error %d: %s", e.Code, e.Message)
}

func New(socketPath string) *Client {
	return &Client{
		socketPath: socketPath,
		dialer:     &net.Dialer{},
	}
}

func (c *Client) Call(ctx context.Context, method string, params any, result any) error {
	conn, err := c.dialer.DialContext(ctx, "unix", c.socketPath)
	if err != nil {
		return err
	}
	defer conn.Close()

	if deadline, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(deadline)
	}

	if err := json.NewEncoder(conn).Encode(request{
		JSONRPC: "2.0",
		ID:      1,
		Method:  method,
		Params:  params,
	}); err != nil {
		return err
	}

	var resp response
	if err := json.NewDecoder(conn).Decode(&resp); err != nil {
		return err
	}
	if resp.Error != nil {
		return resp.Error
	}
	if result == nil || len(resp.Result) == 0 {
		return nil
	}
	return json.Unmarshal(resp.Result, result)
}
