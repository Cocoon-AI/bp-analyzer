package rpc

import (
	"encoding/json"
	"fmt"
	"time"

	"github.com/cocoonai/bp-analyzer/internal/pipe"
)

// Request is a JSON-RPC 2.0 request.
type Request struct {
	JSONRPC string      `json:"jsonrpc"`
	Method  string      `json:"method"`
	Params  interface{} `json:"params,omitempty"`
	ID      int         `json:"id"`
}

// Response is a JSON-RPC 2.0 response.
type Response struct {
	JSONRPC string          `json:"jsonrpc"`
	Result  json.RawMessage `json:"result,omitempty"`
	Error   *RPCError       `json:"error,omitempty"`
	ID      int             `json:"id"`
}

// RPCError is a JSON-RPC 2.0 error object.
type RPCError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *RPCError) Error() string {
	return fmt.Sprintf("RPC error %d: %s", e.Code, e.Message)
}

// Call connects to the named pipe, sends a JSON-RPC request, and returns the result.
func Call(pipeName string, method string, params interface{}) (json.RawMessage, error) {
	return CallWithTimeout(pipeName, method, params, 30*time.Second)
}

// CallWithTimeout is like Call but with a configurable timeout.
func CallWithTimeout(pipeName string, method string, params interface{}, timeout time.Duration) (json.RawMessage, error) {
	conn, err := pipe.Connect(pipeName, timeout)
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	// Build request
	req := Request{
		JSONRPC: "2.0",
		Method:  method,
		Params:  params,
		ID:      1,
	}

	reqData, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal request: %w", err)
	}

	// Send
	if err := pipe.Send(conn, reqData); err != nil {
		return nil, err
	}

	// Receive
	respData, err := pipe.Receive(conn)
	if err != nil {
		return nil, err
	}

	// Parse response
	var resp Response
	if err := json.Unmarshal(respData, &resp); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	if resp.Error != nil {
		return nil, resp.Error
	}

	return resp.Result, nil
}
