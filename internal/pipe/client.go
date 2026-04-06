package pipe

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"time"

	winio "github.com/Microsoft/go-winio"
)

// PipePath returns the full named pipe path for a given name.
func PipePath(name string) string {
	return `\\.\pipe\` + name
}

// Connect opens a connection to the named pipe.
func Connect(pipeName string, timeout time.Duration) (net.Conn, error) {
	path := PipePath(pipeName)
	conn, err := winio.DialPipe(path, &timeout)
	if err != nil {
		return nil, fmt.Errorf("failed to connect to pipe %s: %w", path, err)
	}
	return conn, nil
}

// Send writes a length-prefixed message to the connection.
func Send(conn net.Conn, data []byte) error {
	// Write 4-byte little-endian length
	length := uint32(len(data))
	if err := binary.Write(conn, binary.LittleEndian, length); err != nil {
		return fmt.Errorf("failed to write message length: %w", err)
	}

	// Write payload
	_, err := conn.Write(data)
	if err != nil {
		return fmt.Errorf("failed to write message payload: %w", err)
	}
	return nil
}

// Receive reads a length-prefixed message from the connection.
func Receive(conn net.Conn) ([]byte, error) {
	// Read 4-byte little-endian length
	var length uint32
	if err := binary.Read(conn, binary.LittleEndian, &length); err != nil {
		return nil, fmt.Errorf("failed to read message length: %w", err)
	}

	// Sanity check (max 64MB)
	if length > 64*1024*1024 {
		return nil, fmt.Errorf("message too large: %d bytes", length)
	}

	// Read the payload
	buf := make([]byte, length)
	if _, err := io.ReadFull(conn, buf); err != nil {
		return nil, fmt.Errorf("failed to read message payload: %w", err)
	}

	return buf, nil
}
