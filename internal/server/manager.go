package server

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"time"

	"github.com/cocoonai/bp-analyzer/internal/config"
	"github.com/cocoonai/bp-analyzer/internal/rpc"
)

// IsRunning checks if the server is responding on the given pipe.
func IsRunning(pipeName string) bool {
	_, err := rpc.CallWithTimeout(pipeName, "ping", nil, 3*time.Second)
	return err == nil
}

// Start launches the UE4 commandlet in server mode as a detached process.
// It waits for the pipe to become available or timeout.
func Start(cfg *config.Config) error {
	if err := cfg.Validate(); err != nil {
		return err
	}

	args := []string{
		cfg.UProject,
		"-run=BlueprintExport",
		"-pipeserver",
		fmt.Sprintf("-pipename=%s", cfg.PipeName),
		"-unattended",
		"-nopause",
	}

	cmd := exec.Command(cfg.EditorCmd, args...)

	// Detach the process so it outlives digbp
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: syscall.CREATE_NEW_PROCESS_GROUP,
	}

	// Capture both stdout and stderr — UE4 UE_LOG output can go to either
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return fmt.Errorf("failed to create stdout pipe: %w", err)
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return fmt.Errorf("failed to create stderr pipe: %w", err)
	}

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("failed to start UE4 server: %w", err)
	}

	fmt.Fprintf(os.Stderr, "digbp: launching UE4 server (pid %d)...\n", cmd.Process.Pid)

	// Scan both stdout and stderr for the ready marker
	readyCh := make(chan bool, 2)
	scanOutput := func(r io.Reader) {
		scanner := bufio.NewScanner(r)
		scanner.Buffer(make([]byte, 1024*1024), 1024*1024)
		for scanner.Scan() {
			line := scanner.Text()
			if strings.Contains(line, "__SERVER_READY__") {
				readyCh <- true
				return
			}
		}
		readyCh <- false
	}
	go scanOutput(stdout)
	go scanOutput(stderr)

	// Also poll the named pipe directly in case we miss the marker in output
	pollCh := make(chan bool, 1)
	go func() {
		deadline := time.Now().Add(cfg.StartTimeout)
		for time.Now().Before(deadline) {
			time.Sleep(2 * time.Second)
			if IsRunning(cfg.PipeName) {
				pollCh <- true
				return
			}
		}
		pollCh <- false
	}()

	// Wait for any signal: marker found, pipe responding, or timeout
	exitCh := make(chan error, 1)
	go func() {
		exitCh <- cmd.Wait()
	}()

	for {
		select {
		case ready := <-readyCh:
			if ready {
				time.Sleep(500 * time.Millisecond)
				fmt.Fprintf(os.Stderr, "digbp: server ready\n")
				return nil
			}
			// One stream ended without marker — the other might still find it,
			// or the process may have exited. Don't return yet; fall through to
			// check exitCh on next iteration.
		case ready := <-pollCh:
			if ready {
				fmt.Fprintf(os.Stderr, "digbp: server ready (detected via pipe)\n")
				return nil
			}
			return fmt.Errorf("timed out waiting for UE4 server to start (timeout: %s)", cfg.StartTimeout)
		case err := <-exitCh:
			if err != nil {
				return fmt.Errorf("UE4 server process exited with error: %w", err)
			}
			return fmt.Errorf("UE4 server process exited before becoming ready")
		case <-time.After(cfg.StartTimeout):
			return fmt.Errorf("timed out waiting for UE4 server to start (timeout: %s)", cfg.StartTimeout)
		}
	}
}

// Stop sends a shutdown command to the server.
// The server may close the pipe before we read the response, so we
// treat pipe errors as success and verify by checking IsRunning.
func Stop(pipeName string) error {
	_, err := rpc.Call(pipeName, "shutdown", nil)
	if err != nil {
		// If we can't even connect, server is already stopped
		if !IsRunning(pipeName) {
			return nil
		}
		return err
	}
	return nil
}

// EnsureRunning starts the server if it's not already running.
func EnsureRunning(cfg *config.Config) error {
	if IsRunning(cfg.PipeName) {
		return nil
	}
	fmt.Fprintf(os.Stderr, "digbp: server not running, starting...\n")
	return Start(cfg)
}
