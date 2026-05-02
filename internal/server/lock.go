// Cross-process file lock used to serialize digbp's server-start path so
// parallel CLI invocations don't all try to spawn UE4Editor-Cmd at once.
//
// Without this lock, N concurrent `digbp <command>` calls all observe
// IsRunning()==false (server isn't up yet), all call Start(), and each
// Start() reaps "stale" processes via the shared pidfile — which means
// every process kills every other process's UE4 launch. The cascade ends
// with all calls reporting "exit status 1" or "EOF".
//
// LockFileEx with LOCKFILE_EXCLUSIVE_LOCK gives us a kernel-managed lock
// that blocks contending callers and releases automatically if the holder
// dies (e.g. user Ctrl-C's mid-launch). Lockfile path lives in %TEMP%
// alongside the pidfile.
//
//go:build windows

package server

import (
	"fmt"
	"os"
	"path/filepath"

	"golang.org/x/sys/windows"
)

type startLock struct {
	f *os.File
}

func startLockPath(pipeName string) string {
	return filepath.Join(os.TempDir(), fmt.Sprintf("digbp-%s.lock", pipeName))
}

// acquireStartLock blocks until the per-pipe start lock is available.
// Returns a handle that must be released via .Release().
func acquireStartLock(pipeName string) (*startLock, error) {
	path := startLockPath(pipeName)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0644)
	if err != nil {
		return nil, fmt.Errorf("open start lockfile %s: %w", path, err)
	}
	var ol windows.Overlapped
	if err := windows.LockFileEx(
		windows.Handle(f.Fd()),
		windows.LOCKFILE_EXCLUSIVE_LOCK,
		0,
		0xFFFFFFFF, 0xFFFFFFFF,
		&ol,
	); err != nil {
		f.Close()
		return nil, fmt.Errorf("lock start lockfile %s: %w", path, err)
	}
	return &startLock{f: f}, nil
}

func (l *startLock) Release() {
	if l == nil || l.f == nil {
		return
	}
	var ol windows.Overlapped
	_ = windows.UnlockFileEx(
		windows.Handle(l.f.Fd()),
		0,
		0xFFFFFFFF, 0xFFFFFFFF,
		&ol,
	)
	_ = l.f.Close()
	l.f = nil
}
