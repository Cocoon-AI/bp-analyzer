package main

import (
	"encoding/json"
	"fmt"
	"os"
	"time"

	"github.com/cocoonai/bp-analyzer/internal/config"
	"github.com/cocoonai/bp-analyzer/internal/rpc"
	"github.com/cocoonai/bp-analyzer/internal/server"
	"github.com/spf13/cobra"
)

var (
	cfg       *config.Config
	flagPretty bool
)

func main() {
	cfg = config.Load()

	root := &cobra.Command{
		Use:   "digbp",
		Short: "CLI tool for the Blueprint Analyzer server",
		Long:  "digbp communicates with a persistent UE4 Blueprint Analyzer server via named pipes.",
		PersistentPreRun: func(cmd *cobra.Command, args []string) {
			// Apply flag overrides to config
			if v, _ := cmd.Flags().GetString("editor-cmd"); v != "" {
				cfg.EditorCmd = v
			}
			if v, _ := cmd.Flags().GetString("uproject"); v != "" {
				cfg.UProject = v
			}
			if v, _ := cmd.Flags().GetString("pipe-name"); v != "" {
				cfg.PipeName = v
			}
		},
	}

	// Persistent flags
	root.PersistentFlags().String("editor-cmd", "", "Path to UE4Editor-Cmd.exe")
	root.PersistentFlags().String("uproject", "", "Path to .uproject file")
	root.PersistentFlags().String("pipe-name", "", "Named pipe name (default: blueprintexport)")
	root.PersistentFlags().BoolVar(&flagPretty, "pretty", false, "Pretty-print JSON output")

	root.AddCommand(
		startCmd(),
		stopCmd(),
		statusCmd(),
		exportCmd(),
		listCmd(),
		cppusageCmd(),
		referencesCmd(),
		graphCmd(),
		refviewCmd(),
		findcallersCmd(),
		findvarusesCmd(),
		nativeeventsCmd(),
		findeventsCmd(),
		findpropCmd(),
		searchCmd(),
		cppAuditCmd(),
		editCmd(),
		versionCmd(),
	)

	if err := root.Execute(); err != nil {
		os.Exit(1)
	}
}

// callServer ensures the server is running, sends an RPC, and prints the result.
func callServer(method string, params interface{}) error {
	if err := server.EnsureRunning(cfg); err != nil {
		return err
	}
	result, err := rpc.Call(cfg.PipeName, method, params)
	if err != nil {
		return err
	}
	return printResult(result)
}

func printResult(result json.RawMessage) error {
	if flagPretty {
		var v interface{}
		if err := json.Unmarshal(result, &v); err == nil {
			pretty, _ := json.MarshalIndent(v, "", "  ")
			fmt.Println(string(pretty))
			return nil
		}
	}
	fmt.Println(string(result))
	return nil
}

// callServerToFile behaves like callServer but writes the result JSON to a file
// instead of stdout. Pretty-prints if --pretty is set.
func callServerToFile(method string, params interface{}, outPath string) error {
	if err := server.EnsureRunning(cfg); err != nil {
		return err
	}
	result, err := rpc.Call(cfg.PipeName, method, params)
	if err != nil {
		return err
	}
	payload := []byte(result)
	if flagPretty {
		var v interface{}
		if err := json.Unmarshal(result, &v); err == nil {
			if pretty, err := json.MarshalIndent(v, "", "  "); err == nil {
				payload = pretty
			}
		}
	}
	if err := os.WriteFile(outPath, payload, 0644); err != nil {
		return fmt.Errorf("write output file: %w", err)
	}
	fmt.Printf("Wrote %d bytes to %s\n", len(payload), outPath)
	return nil
}

// --- Lifecycle commands ---

func startCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "start",
		Short: "Start the UE4 Blueprint Analyzer server",
		RunE: func(cmd *cobra.Command, args []string) error {
			if server.IsRunning(cfg.PipeName) {
				fmt.Println("Server is already running")
				return nil
			}
			return server.Start(cfg)
		},
	}
}

func stopCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "stop",
		Short: "Stop the UE4 Blueprint Analyzer server",
		RunE: func(cmd *cobra.Command, args []string) error {
			if err := server.Stop(cfg.PipeName); err != nil {
				return fmt.Errorf("failed to stop server: %w", err)
			}
			fmt.Println("Server stopped")
			return nil
		},
	}
}

// versionCmd reports build timestamps for both the local digbp binary and the
// running server (if one exists), so PATH-staleness problems are one command
// away from diagnosable. Motivated by the Cluster 1b.5 incident where steamdev's
// `C:/tools/digbp.exe` was silently behind the `E:/cai/bp-analyzer/` build and
// `findvaruses` reported as "unknown command" until manually resynced.
func versionCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "version",
		Short: "Report digbp CLI and server build timestamps",
		Long: `Prints:
  - the mtime of the digbp.exe binary currently being run (CLI build)
  - the __DATE__/__TIME__ the plugin was compiled (server build), if the
    server is running and responds to the build_info RPC
  - a MATCH/MISMATCH line helping diagnose PATH staleness

Does not start the server; if the server is not running, only CLI info is
printed and the server section reports "not running".`,
		Run: func(cmd *cobra.Command, args []string) {
			// --- CLI side: mtime of the running binary ---
			cliBuildTime := "(unknown)"
			exe, err := os.Executable()
			if err == nil {
				if info, err := os.Stat(exe); err == nil {
					cliBuildTime = info.ModTime().UTC().Format(time.RFC3339)
				}
			}
			fmt.Println("digbp CLI")
			fmt.Printf("  binary:     %s\n", exe)
			fmt.Printf("  build time: %s\n", cliBuildTime)

			// --- Server side: probe build_info over the pipe ---
			fmt.Println()
			fmt.Println("BlueprintAnalyzer server")
			if !server.IsRunning(cfg.PipeName) {
				fmt.Println("  status:     not running (start the server and re-run `digbp version` to compare)")
				return
			}
			result, err := rpc.Call(cfg.PipeName, "build_info", nil)
			if err != nil {
				// Likely an older server that predates build_info — that itself is
				// a diagnosable staleness signal.
				fmt.Printf("  status:     running, but build_info RPC failed: %v\n", err)
				fmt.Println("  note:       server plugin predates the build_info method; rebuild the plugin to compare")
				return
			}
			var info struct {
				Success    bool   `json:"success"`
				BuildDate  string `json:"build_date"`
				BuildTime  string `json:"build_time"`
				ModuleName string `json:"module_name"`
			}
			if err := json.Unmarshal(result, &info); err != nil {
				fmt.Printf("  status:     running, but build_info response malformed: %v\n", err)
				return
			}
			serverStamp := fmt.Sprintf("%s %s", info.BuildDate, info.BuildTime)
			fmt.Printf("  module:     %s\n", info.ModuleName)
			fmt.Printf("  build time: %s (compiled in)\n", serverStamp)

			// --- Comparison ---
			// We can't compare wall-clock directly because the CLI is mtime
			// and the server is __DATE__/__TIME__ at compile. But we can at
			// least flag "CLI is older than any date on the server build" as
			// a strong likely-stale signal.
			fmt.Println()
			if t, err := time.Parse(time.RFC3339, cliBuildTime); err == nil {
				serverT, err := time.Parse("Jan _2 2006 15:04:05", serverStamp)
				if err == nil && serverT.After(t.Add(5*time.Minute)) {
					fmt.Println("HINT: server build is newer than CLI binary by more than 5 min.")
					fmt.Println("      Your `digbp` may be stale — check that the binary on your PATH matches")
					fmt.Println("      the latest `go build -o digbp.exe ./cmd/digbp/` output.")
					return
				}
			}
			fmt.Println("(no staleness hint — CLI appears current relative to server)")
		},
	}
}

func statusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "Check if the server is running",
		Run: func(cmd *cobra.Command, args []string) {
			if server.IsRunning(cfg.PipeName) {
				fmt.Println("Server is running")
			} else {
				fmt.Println("Server is not running")
				os.Exit(1)
			}
		},
	}
}

// --- Operation commands ---

func exportCmd() *cobra.Command {
	var (
		path    string
		analyze bool
		mode    string
	)
	cmd := &cobra.Command{
		Use:   "export",
		Short: "Export a Blueprint",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path}
			if analyze {
				params["analyze"] = true
			}
			if mode != "" {
				params["mode"] = mode
			}
			return callServer("export", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().BoolVar(&analyze, "analyze", false, "Include complexity analysis")
	cmd.Flags().StringVar(&mode, "mode", "", "Output mode: json, compact, skeleton (default: json)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func listCmd() *cobra.Command {
	var (
		dir       string
		noRecurse bool
	)
	cmd := &cobra.Command{
		Use:   "list",
		Short: "List Blueprints in a directory",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"dir":       dir,
				"recursive": !noRecurse,
			}
			return callServer("list", params)
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory path (required)")
	cmd.Flags().BoolVar(&noRecurse, "no-recurse", false, "Don't search subdirectories")
	_ = cmd.MarkFlagRequired("dir")
	return cmd
}

func cppusageCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "cppusage",
		Short: "Get C++ function usage for a Blueprint",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("cppusage", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func referencesCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "references",
		Short: "Get asset references from a Blueprint",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("references", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func graphCmd() *cobra.Command {
	var (
		path  string
		depth int
	)
	cmd := &cobra.Command{
		Use:   "graph",
		Short: "Export dependency graph",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path":  path,
				"depth": depth,
			}
			return callServer("graph", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Root Blueprint path (required)")
	cmd.Flags().IntVar(&depth, "depth", 3, "Maximum graph depth")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func refviewCmd() *cobra.Command {
	var (
		path       string
		refDepth   int
		referDepth int
		bpOnly     bool
	)
	cmd := &cobra.Command{
		Use:   "refview",
		Short: "Reference viewer (bidirectional dependency graph)",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path":       path,
				"refdepth":   refDepth,
				"referdepth": referDepth,
				"bponly":     bpOnly,
			}
			return callServer("refview", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Asset path (required)")
	cmd.Flags().IntVar(&refDepth, "refdepth", 3, "Dependency traversal depth")
	cmd.Flags().IntVar(&referDepth, "referdepth", 3, "Referencer traversal depth")
	cmd.Flags().BoolVar(&bpOnly, "bponly", false, "Only include Blueprint assets")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func findcallersCmd() *cobra.Command {
	var (
		dir       string
		function  string
		className string
	)
	cmd := &cobra.Command{
		Use:   "findcallers",
		Short: "Find Blueprints calling a specific function",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"dir":  dir,
				"func": function,
			}
			if className != "" {
				params["class"] = className
			}
			return callServer("findcallers", params)
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory to search (required)")
	cmd.Flags().StringVar(&function, "func", "", "Function name to find (required)")
	cmd.Flags().StringVar(&className, "class", "", "Filter by class name")
	_ = cmd.MarkFlagRequired("dir")
	_ = cmd.MarkFlagRequired("func")
	return cmd
}

func findvarusesCmd() *cobra.Command {
	var (
		dir  string
		varName string
		kind string
	)
	cmd := &cobra.Command{
		Use:   "findvaruses",
		Short: "Find Blueprints reading or writing a named variable",
		Long: `Scans K2Node_VariableGet/Set sites across FunctionGraphs, UbergraphPages,
and MacroGraphs for the given variable name. Use --kind=get|set to filter
to only one access direction (default: any).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"dir": dir,
				"var": varName,
			}
			if kind != "" {
				params["kind"] = kind
			}
			return callServer("findvaruses", params)
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory to search (required)")
	cmd.Flags().StringVar(&varName, "var", "", "Variable name to find (required)")
	cmd.Flags().StringVar(&kind, "kind", "", "Access kind filter: get, set, any (default: any)")
	_ = cmd.MarkFlagRequired("dir")
	_ = cmd.MarkFlagRequired("var")
	return cmd
}

func nativeeventsCmd() *cobra.Command {
	var dir string
	cmd := &cobra.Command{
		Use:   "nativeevents",
		Short: "Find native event implementations",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("nativeevents", map[string]interface{}{"dir": dir})
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory to search (required)")
	_ = cmd.MarkFlagRequired("dir")
	return cmd
}

func findeventsCmd() *cobra.Command {
	var (
		dir   string
		event string
	)
	cmd := &cobra.Command{
		Use:   "findevents",
		Short: "Find implementable event implementations",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"dir":   dir,
				"event": event,
			}
			return callServer("findevents", params)
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory to search (required)")
	cmd.Flags().StringVar(&event, "event", "", "Event name to find (required)")
	_ = cmd.MarkFlagRequired("dir")
	_ = cmd.MarkFlagRequired("event")
	return cmd
}

func findpropCmd() *cobra.Command {
	var (
		dir         string
		prop        string
		value       string
		parentClass string
	)
	cmd := &cobra.Command{
		Use:   "findprop",
		Short: "Find Blueprints with a specific CDO property",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"dir":  dir,
				"prop": prop,
			}
			if value != "" {
				params["value"] = value
			}
			if parentClass != "" {
				params["parentclass"] = parentClass
			}
			return callServer("findprop", params)
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory to search (required)")
	cmd.Flags().StringVar(&prop, "prop", "", "Property name to find (required)")
	cmd.Flags().StringVar(&value, "value", "", "Filter by property value")
	cmd.Flags().StringVar(&parentClass, "parentclass", "", "Filter by parent class")
	_ = cmd.MarkFlagRequired("dir")
	_ = cmd.MarkFlagRequired("prop")
	return cmd
}

func searchCmd() *cobra.Command {
	var (
		dir   string
		query string
	)
	cmd := &cobra.Command{
		Use:   "search",
		Short: "Search text across Blueprints (node titles, comments, pin names/defaults, variable names)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("search", map[string]interface{}{
				"dir":   dir,
				"query": query,
			})
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory to search (required)")
	cmd.Flags().StringVar(&query, "query", "", "Text to search for (required)")
	_ = cmd.MarkFlagRequired("dir")
	_ = cmd.MarkFlagRequired("query")
	return cmd
}

func cppAuditCmd() *cobra.Command {
	var (
		dir string
		out string
	)
	cmd := &cobra.Command{
		Use:   "cpp-audit",
		Short: "Build a reverse index of every native C++ symbol referenced by any Blueprint under --dir",
		Long: `Walks every Blueprint under --dir and enumerates each reference to a native
C++ symbol: parent class, K2Node_CallFunction targets, K2Node_VariableGet/Set
on native UPROPERTYs, BlueprintAssignable delegate bindings, DynamicCast
targets, Make/BreakStruct on native USTRUCTs, and BlueprintImplementable/
NativeEvent overrides. Emits a reverse index (symbol -> BP callers) plus a
forward index (BP -> symbols it touches).

Use this before a C++ deletion to find what BPs will break.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			if out != "" {
				return callServerToFile("cpp_audit", map[string]interface{}{"dir": dir}, out)
			}
			return callServer("cpp_audit", map[string]interface{}{"dir": dir})
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Directory to search (required, e.g. /Game/)")
	cmd.Flags().StringVar(&out, "out", "", "Write output to file instead of stdout")
	_ = cmd.MarkFlagRequired("dir")
	return cmd
}
