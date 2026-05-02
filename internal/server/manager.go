package server

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/cocoonai/bp-analyzer/internal/config"
	"github.com/cocoonai/bp-analyzer/internal/rpc"
)

// How long Stop() waits for the UE4 process to actually exit after the
// shutdown RPC returns (or fails to reach the pipe). If it's still alive
// after this, we fall back to taskkill /F.
const stopWaitTimeout = 15 * time.Second

// IsRunning checks if the server is responding on the given pipe.
func IsRunning(pipeName string) bool {
	_, err := rpc.CallWithTimeout(pipeName, "ping", nil, 3*time.Second)
	return err == nil
}

// pidFilePath returns %TEMP%/digbp-<pipeName>.pid — a stable per-pipe location
// that survives across digbp invocations so we can reap stragglers.
func pidFilePath(pipeName string) string {
	return filepath.Join(os.TempDir(), fmt.Sprintf("digbp-%s.pid", pipeName))
}

func writePidFile(pipeName string, pid int) error {
	return os.WriteFile(pidFilePath(pipeName), []byte(strconv.Itoa(pid)), 0644)
}

func readPidFile(pipeName string) (int, error) {
	data, err := os.ReadFile(pidFilePath(pipeName))
	if err != nil {
		return 0, err
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil {
		return 0, err
	}
	return pid, nil
}

func removePidFile(pipeName string) {
	_ = os.Remove(pidFilePath(pipeName))
}

// processExists checks whether a process with the given PID is still alive.
//
// Go's os.Process.Signal() is effectively useless on Windows — it returns
// "not supported" for every signal except os.Kill, so a naive "send signal 0
// and check for error" probe always reports dead. Instead we call Windows'
// OpenProcess(SYNCHRONIZE, ...) and check GetExitCodeProcess for STILL_ACTIVE.
func processExists(pid int) bool {
	if pid <= 0 {
		return false
	}
	const PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
	const STILL_ACTIVE = 259
	h, err := syscall.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, uint32(pid))
	if err != nil {
		// Access-denied also means the PID was recycled into a system process
		// we can't touch, which is effectively "not our process" → treat as dead.
		return false
	}
	defer syscall.CloseHandle(h)

	var exitCode uint32
	if err := syscall.GetExitCodeProcess(h, &exitCode); err != nil {
		return false
	}
	return exitCode == STILL_ACTIVE
}

// killProcess uses taskkill /F on Windows to forcibly terminate a process tree.
// os.Process.Kill works but doesn't reliably kill UE4 child processes; taskkill
// with /T handles the whole tree.
func killProcess(pid int) error {
	cmd := exec.Command("taskkill", "/F", "/T", "/PID", strconv.Itoa(pid))
	return cmd.Run()
}

// waitForProcessExit polls until the PID is gone or the timeout elapses.
func waitForProcessExit(pid int, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if !processExists(pid) {
			return true
		}
		time.Sleep(200 * time.Millisecond)
	}
	return !processExists(pid)
}

// reapStale checks for a stale process recorded in the pidfile whose pipe is
// no longer responding (Stop crashed, user killed digbp mid-start, etc.) and
// kills it so Start can proceed with a fresh launch.
//
// Returns true if something was actually reaped.
func reapStale(cfg *config.Config) bool {
	pid, err := readPidFile(cfg.PipeName)
	if err != nil {
		return false
	}
	if !processExists(pid) {
		// pidfile is stale but process is already gone — clean up the file.
		removePidFile(cfg.PipeName)
		return false
	}
	if IsRunning(cfg.PipeName) {
		// Process is alive AND pipe is responding: this isn't stale, it's a
		// healthy server. EnsureRunning should have short-circuited; treat as
		// not-reaped and let the caller notice.
		return false
	}
	// Process alive but pipe dead → stale. Kill it.
	fmt.Fprintf(os.Stderr, "digbp: reaping stale UE4 server (pid %d, pipe not responding)...\n", pid)
	if err := killProcess(pid); err != nil {
		fmt.Fprintf(os.Stderr, "digbp: taskkill failed for pid %d: %v\n", pid, err)
	}
	// Give Windows a moment to release the pipe name.
	waitForProcessExit(pid, 5*time.Second)
	removePidFile(cfg.PipeName)
	return true
}

// Start launches the UE4 commandlet in server mode as a detached process.
// It waits for the pipe to become available or timeout.
//
// If a stale process from a previous run is holding the pipe name (pidfile
// exists, process alive, pipe dead), it is reaped first.
func Start(cfg *config.Config) error {
	if err := cfg.Validate(); err != nil {
		return err
	}

	// Reap any lingering process from a prior run before we try to launch.
	reapStale(cfg)

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

	// Record the PID so Stop() / reapStale() can find the process later.
	if err := writePidFile(cfg.PipeName, cmd.Process.Pid); err != nil {
		fmt.Fprintf(os.Stderr, "digbp: warning — failed to write pidfile: %v\n", err)
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
			removePidFile(cfg.PipeName)
			return fmt.Errorf("timed out waiting for UE4 server to start (timeout: %s)", cfg.StartTimeout)
		case err := <-exitCh:
			removePidFile(cfg.PipeName)
			if err != nil {
				return fmt.Errorf("UE4 server process exited with error: %w", err)
			}
			return fmt.Errorf("UE4 server process exited before becoming ready")
		case <-time.After(cfg.StartTimeout):
			removePidFile(cfg.PipeName)
			return fmt.Errorf("timed out waiting for UE4 server to start (timeout: %s)", cfg.StartTimeout)
		}
	}
}

// Stop sends a shutdown command to the server AND waits for the underlying
// process to actually exit. If the process is still alive after stopWaitTimeout,
// it is force-killed via taskkill /F so the next Start() has a clean slate.
//
// The pidfile is used to track the actual UE4 process — prior versions relied
// solely on the RPC shutdown, which left stragglers when the editor process
// ignored the shutdown or the pipe had already closed.
func Stop(pipeName string) error {
	// Read the pidfile first so we know what to wait for even if the RPC fails.
	pid, pidErr := readPidFile(pipeName)

	// Send shutdown RPC. Pipe errors are treated as "already stopped" only if
	// we can confirm via IsRunning; otherwise we fall through to the pid-based
	// wait-and-kill path.
	_, rpcErr := rpc.Call(pipeName, "shutdown", nil)
	if rpcErr != nil && !IsRunning(pipeName) && pidErr != nil {
		// No pidfile, no pipe → nothing to stop.
		return nil
	}

	if pidErr == nil && pid > 0 {
		// Wait for the actual process to exit.
		if waitForProcessExit(pid, stopWaitTimeout) {
			removePidFile(pipeName)
			return nil
		}
		// Process is still alive after the graceful-shutdown window — force kill.
		fmt.Fprintf(os.Stderr, "digbp: process %d did not exit within %s, force-killing\n", pid, stopWaitTimeout)
		if err := killProcess(pid); err != nil {
			return fmt.Errorf("shutdown RPC sent but process %d still alive and taskkill failed: %w", pid, err)
		}
		waitForProcessExit(pid, 5*time.Second)
		removePidFile(pipeName)
		return nil
	}

	// No pidfile available — best we can do is report RPC status.
	if rpcErr != nil {
		return rpcErr
	}
	return nil
}

// EnsureRunning starts the server if it's not already running.
//
// Uses a file lock around the start path so parallel digbp invocations
// don't all try to launch UE4Editor-Cmd at once. Without the lock, N
// concurrent calls all see IsRunning==false, all enter Start(), and
// reapStale() in each Start treats the others' partially-launched
// processes as stale and kills them. The lock + re-check pattern means
// the first caller launches the server while the rest queue, then all
// subsequent callers short-circuit on the now-running server.
func EnsureRunning(cfg *config.Config) error {
	// Fast path: cheap pipe ping. Avoids touching the lock for the common
	// case where the server is already up.
	if IsRunning(cfg.PipeName) {
		return nil
	}

	lock, err := acquireStartLock(cfg.PipeName)
	if err != nil {
		return fmt.Errorf("digbp: failed to acquire server-start lock: %w", err)
	}
	defer lock.Release()

	// Re-check under lock. Another digbp process may have started the
	// server while we were blocked on lock acquisition.
	if IsRunning(cfg.PipeName) {
		return nil
	}

	fmt.Fprintf(os.Stderr, "digbp: server not running, starting...\n")
	return Start(cfg)
}
