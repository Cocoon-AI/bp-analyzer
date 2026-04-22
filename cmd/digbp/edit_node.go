package main

import (
	"github.com/spf13/cobra"
)

// editNodeCmd groups K2 node graph editing operations.
func editNodeCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "node",
		Short: "Edit K2 nodes in a Blueprint graph",
	}
	cmd.AddCommand(
		// Generic
		editNodeRemoveCmd(),
		editNodeRemoveBrokenCmd(),
		editNodeRefreshVariablesCmd(),
		editNodeMoveCmd(),
		editNodeAddGenericCmd(),
		// High-level builders
		editNodeAddVarGetCmd(),
		editNodeAddVarSetCmd(),
		editNodeAddFuncCallCmd(),
		editNodeAddBranchCmd(),
		editNodeAddSequenceCmd(),
		editNodeAddForLoopCmd(),
		editNodeAddCastCmd(),
		editNodeAddMakeStructCmd(),
		editNodeAddBreakStructCmd(),
		editNodeAddLiteralCmd(),
	)
	return cmd
}

// editPinCmd groups pin wiring operations.
func editPinCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "pin",
		Short: "Edit pin connections and default values",
	}
	cmd.AddCommand(
		editPinSetDefaultCmd(),
		editPinConnectCmd(),
		editPinDisconnectCmd(),
		editPinBreakAllCmd(),
	)
	return cmd
}

// --- shared flag plumbing ---

// addGraphFlags registers the common `--path` and `--graph` required flags on a command.
func addGraphFlags(cmd *cobra.Command, path, graph *string) {
	cmd.Flags().StringVar(path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(graph, "graph", "", "Graph name (function name or 'EventGraph') (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("graph")
}

// addPositionFlags registers optional --x and --y flags.
func addPositionFlags(cmd *cobra.Command, x, y *int) {
	cmd.Flags().IntVar(x, "x", 0, "X position in the graph")
	cmd.Flags().IntVar(y, "y", 0, "Y position in the graph")
}

// --- generic node ops ---

func editNodeRemoveCmd() *cobra.Command {
	var path, graph, guid string
	cmd := &cobra.Command{
		Use:   "remove",
		Short: "Remove a node by GUID",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.remove", map[string]interface{}{
				"path": path, "graph": graph, "node_guid": guid,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&guid, "node-guid", "", "Node GUID (required)")
	_ = cmd.MarkFlagRequired("node-guid")
	return cmd
}

func editNodeRemoveBrokenCmd() *cobra.Command {
	var (
		path   string
		dryRun bool
	)
	cmd := &cobra.Command{
		Use:   "remove-broken",
		Short: "Remove all nodes with broken/deleted struct types (use --dry-run to preview)",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path}
			if dryRun {
				params["dry_run"] = true
			}
			return callServer("edit.node.remove_broken", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "List broken nodes without removing them")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func editNodeRefreshVariablesCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "refresh-variables",
		Short: "ReconstructNode on every K2Node_Variable / CallFunction / BaseMCDelegate in the BP",
		Long: `Forces a pin-topology + display-name rebuild on every variable-ref, function-
call, and delegate-bind node in the target BP. Manual cleanup pass for when
an external variable lift (or cross-BP rename) leaves stale display names
("Get Is Locked" alongside "Get IsLocked" post-lift) or pin layouts that
UE's own compile-time ReconstructAllNodes didn't catch.

Typical use: run on each external BP in lift's external_bps[] response
after a variable lift that had compile errors on the caller side. Then
'edit save-and-compile' the BP to persist + verify.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.refresh_variables", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func editNodeMoveCmd() *cobra.Command {
	var (
		path, graph, guid string
		x, y              int
	)
	cmd := &cobra.Command{
		Use:   "move",
		Short: "Move a node to a new position",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.move", map[string]interface{}{
				"path": path, "graph": graph, "node_guid": guid, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&guid, "node-guid", "", "Node GUID (required)")
	cmd.Flags().IntVar(&x, "x", 0, "New X position (required)")
	cmd.Flags().IntVar(&y, "y", 0, "New Y position (required)")
	_ = cmd.MarkFlagRequired("node-guid")
	_ = cmd.MarkFlagRequired("x")
	_ = cmd.MarkFlagRequired("y")
	return cmd
}

func editNodeAddGenericCmd() *cobra.Command {
	var (
		path, graph, nodeClass string
		x, y                   int
	)
	cmd := &cobra.Command{
		Use:   "add-generic",
		Short: "Add a K2Node subclass by class path (escape hatch for anything not in add-branch/add-function-call/etc.)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_generic", map[string]interface{}{
				"path": path, "graph": graph, "node_class": nodeClass, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&nodeClass, "node-class", "", "K2Node class path, e.g. /Script/BlueprintGraph.K2Node_Self (required)")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("node-class")
	return cmd
}

// --- high-level builders ---

func editNodeAddVarGetCmd() *cobra.Command {
	var (
		path, graph, variable string
		x, y                  int
	)
	cmd := &cobra.Command{
		Use:   "add-variable-get",
		Short: "Add a VariableGet node",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_variable_get", map[string]interface{}{
				"path": path, "graph": graph, "variable": variable, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&variable, "variable", "", "Variable name (required)")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("variable")
	return cmd
}

func editNodeAddVarSetCmd() *cobra.Command {
	var (
		path, graph, variable string
		x, y                  int
	)
	cmd := &cobra.Command{
		Use:   "add-variable-set",
		Short: "Add a VariableSet node",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_variable_set", map[string]interface{}{
				"path": path, "graph": graph, "variable": variable, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&variable, "variable", "", "Variable name (required)")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("variable")
	return cmd
}

func editNodeAddFuncCallCmd() *cobra.Command {
	var (
		path, graph, fn, className string
		x, y                       int
	)
	cmd := &cobra.Command{
		Use:   "add-function-call",
		Short: "Add a CallFunction node",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path": path, "graph": graph, "function": fn, "x": x, "y": y,
			}
			if className != "" {
				params["class"] = className
			}
			return callServer("edit.node.add_function_call", params)
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&fn, "function", "", "Function name (required)")
	cmd.Flags().StringVar(&className, "class", "", "Owning class (defaults to Blueprint's parent class)")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("function")
	return cmd
}

func editNodeAddBranchCmd() *cobra.Command {
	var (
		path, graph string
		x, y        int
	)
	cmd := &cobra.Command{
		Use:   "add-branch",
		Short: "Add an If/Then/Else branch node",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_branch", map[string]interface{}{
				"path": path, "graph": graph, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	addPositionFlags(cmd, &x, &y)
	return cmd
}

func editNodeAddSequenceCmd() *cobra.Command {
	var (
		path, graph string
		x, y, pins  int
	)
	cmd := &cobra.Command{
		Use:   "add-sequence",
		Short: "Add an ExecutionSequence node",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_sequence", map[string]interface{}{
				"path": path, "graph": graph, "x": x, "y": y, "then_pin_count": pins,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	addPositionFlags(cmd, &x, &y)
	cmd.Flags().IntVar(&pins, "then-pin-count", 2, "Number of Then output pins (>=2)")
	return cmd
}

func editNodeAddForLoopCmd() *cobra.Command {
	var (
		path, graph string
		x, y        int
	)
	cmd := &cobra.Command{
		Use:   "add-for-loop",
		Short: "Add a ForLoop macro instance (stub; use add-generic until implemented)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_for_loop", map[string]interface{}{
				"path": path, "graph": graph, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	addPositionFlags(cmd, &x, &y)
	return cmd
}

func editNodeAddCastCmd() *cobra.Command {
	var (
		path, graph, target string
		x, y                int
		pure                bool
	)
	cmd := &cobra.Command{
		Use:   "add-cast",
		Short: "Add a DynamicCast node",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_cast", map[string]interface{}{
				"path": path, "graph": graph, "target_class": target,
				"x": x, "y": y, "pure": pure,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&target, "target-class", "", "Target class (required)")
	cmd.Flags().BoolVar(&pure, "pure", false, "Create as a pure cast")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("target-class")
	return cmd
}

func editNodeAddMakeStructCmd() *cobra.Command {
	var (
		path, graph, structName string
		x, y                    int
	)
	cmd := &cobra.Command{
		Use:   "add-make-struct",
		Short: "Add a MakeStruct node",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_make_struct", map[string]interface{}{
				"path": path, "graph": graph, "struct": structName, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&structName, "struct", "", "Struct name (required)")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("struct")
	return cmd
}

func editNodeAddBreakStructCmd() *cobra.Command {
	var (
		path, graph, structName string
		x, y                    int
	)
	cmd := &cobra.Command{
		Use:   "add-break-struct",
		Short: "Add a BreakStruct node",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_break_struct", map[string]interface{}{
				"path": path, "graph": graph, "struct": structName, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&structName, "struct", "", "Struct name (required)")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("struct")
	return cmd
}

func editNodeAddLiteralCmd() *cobra.Command {
	var (
		path, graph, objectPath string
		x, y                    int
	)
	cmd := &cobra.Command{
		Use:   "add-literal",
		Short: "Add an object Literal node (for scalar constants use 'edit pin set-default' instead)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.node.add_literal", map[string]interface{}{
				"path": path, "graph": graph, "object": objectPath, "x": x, "y": y,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&objectPath, "object", "", "Object path to reference (required)")
	addPositionFlags(cmd, &x, &y)
	_ = cmd.MarkFlagRequired("object")
	return cmd
}

// --- pin ops ---

func editPinSetDefaultCmd() *cobra.Command {
	var path, graph, guid, pinName, value string
	cmd := &cobra.Command{
		Use:   "set-default",
		Short: "Set the default value on an input pin",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.pin.set_default", map[string]interface{}{
				"path": path, "graph": graph, "node_guid": guid,
				"pin_name": pinName, "value": value,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&guid, "node-guid", "", "Node GUID (required)")
	cmd.Flags().StringVar(&pinName, "pin-name", "", "Pin name (required)")
	cmd.Flags().StringVar(&value, "value", "", "Default value as UE text format (required, may be empty)")
	_ = cmd.MarkFlagRequired("node-guid")
	_ = cmd.MarkFlagRequired("pin-name")
	return cmd
}

func editPinConnectCmd() *cobra.Command {
	var path, graph, fromNode, fromPin, toNode, toPin string
	cmd := &cobra.Command{
		Use:   "connect",
		Short: "Connect two pins (output -> input)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.pin.connect", map[string]interface{}{
				"path": path, "graph": graph,
				"from_node": fromNode, "from_pin": fromPin,
				"to_node": toNode, "to_pin": toPin,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&fromNode, "from-node", "", "Source node GUID (required)")
	cmd.Flags().StringVar(&fromPin, "from-pin", "", "Source output pin name (required)")
	cmd.Flags().StringVar(&toNode, "to-node", "", "Target node GUID (required)")
	cmd.Flags().StringVar(&toPin, "to-pin", "", "Target input pin name (required)")
	_ = cmd.MarkFlagRequired("from-node")
	_ = cmd.MarkFlagRequired("from-pin")
	_ = cmd.MarkFlagRequired("to-node")
	_ = cmd.MarkFlagRequired("to-pin")
	return cmd
}

func editPinDisconnectCmd() *cobra.Command {
	var path, graph, fromNode, fromPin, toNode, toPin string
	cmd := &cobra.Command{
		Use:   "disconnect",
		Short: "Break a specific pin-to-pin connection",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.pin.disconnect", map[string]interface{}{
				"path": path, "graph": graph,
				"from_node": fromNode, "from_pin": fromPin,
				"to_node": toNode, "to_pin": toPin,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&fromNode, "from-node", "", "Source node GUID (required)")
	cmd.Flags().StringVar(&fromPin, "from-pin", "", "Source output pin name (required)")
	cmd.Flags().StringVar(&toNode, "to-node", "", "Target node GUID (required)")
	cmd.Flags().StringVar(&toPin, "to-pin", "", "Target input pin name (required)")
	_ = cmd.MarkFlagRequired("from-node")
	_ = cmd.MarkFlagRequired("from-pin")
	_ = cmd.MarkFlagRequired("to-node")
	_ = cmd.MarkFlagRequired("to-pin")
	return cmd
}

func editPinBreakAllCmd() *cobra.Command {
	var path, graph, guid, pinName string
	cmd := &cobra.Command{
		Use:   "break-all-links",
		Short: "Break all connections on a pin",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.pin.break_all_links", map[string]interface{}{
				"path": path, "graph": graph, "node_guid": guid, "pin_name": pinName,
			})
		},
	}
	addGraphFlags(cmd, &path, &graph)
	cmd.Flags().StringVar(&guid, "node-guid", "", "Node GUID (required)")
	cmd.Flags().StringVar(&pinName, "pin-name", "", "Pin name (required)")
	_ = cmd.MarkFlagRequired("node-guid")
	_ = cmd.MarkFlagRequired("pin-name")
	return cmd
}
