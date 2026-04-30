package main

import (
	"github.com/spf13/cobra"
)

// editCmd is the parent for all blueprint mutation subcommands. It has no
// RunE of its own — cobra prints help when invoked bare.
func editCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "edit",
		Short: "Mutate Blueprint assets (variables, functions, nodes, ...)",
		Long: `Edit operations mutate Blueprint assets through the running UE4 server.
Changes stage in memory until explicitly committed with 'edit compile' and
'edit save'. Source control is the rollback story — use 'p4 revert' or
'git restore' on the .uasset file if you need to undo.`,
	}

	cmd.AddCommand(
		// Phase A (lifecycle + metadata)
		editCompileCmd(),
		editSaveCmd(),
		editSaveAndCompileCmd(),
		editReparentCmd(),
		editAddInterfaceCmd(),
		editRemoveInterfaceCmd(),
		editSetFlagsCmd(),
		// Phase B (variables + CDO)
		editVariableCmd(),
		editCdoCmd(),
		editPurgePhantomCmd(),
		// Phase C (functions, events, dispatchers, components)
		editFunctionCmd(),
		editEventCmd(),
		editDispatcherCmd(),
		editComponentCmd(),
		// Phase D (node graph editing)
		editNodeCmd(),
		editPinCmd(),
		// Cross-BP rewrites
		editExternalCmd(),
	)

	return cmd
}

// --- Lifecycle ---

func editCompileCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "compile",
		Short: "Compile a Blueprint",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.compile", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func editSaveCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "save",
		Short: "Save a Blueprint to disk",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.save", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func editSaveAndCompileCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "save-and-compile",
		Short: "Compile and then save a Blueprint",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.save_and_compile", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

// --- Metadata ---

func editReparentCmd() *cobra.Command {
	var path, parent string
	cmd := &cobra.Command{
		Use:   "reparent",
		Short: "Change a Blueprint's parent class",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.reparent", map[string]interface{}{
				"path":         path,
				"parent_class": parent,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&parent, "parent-class", "", "New parent class path (required, e.g. /Script/Engine.Actor)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("parent-class")
	return cmd
}

func editAddInterfaceCmd() *cobra.Command {
	var path, iface string
	cmd := &cobra.Command{
		Use:   "add-interface",
		Short: "Implement a new interface on a Blueprint",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.add_interface", map[string]interface{}{
				"path":      path,
				"interface": iface,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&iface, "interface", "", "Interface class path or name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("interface")
	return cmd
}

func editRemoveInterfaceCmd() *cobra.Command {
	var (
		path, iface       string
		preserveFunctions bool
	)
	cmd := &cobra.Command{
		Use:   "remove-interface",
		Short: "Remove an implemented interface from a Blueprint",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.remove_interface", map[string]interface{}{
				"path":               path,
				"interface":          iface,
				"preserve_functions": preserveFunctions,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&iface, "interface", "", "Interface class path or name (required)")
	cmd.Flags().BoolVar(&preserveFunctions, "preserve-functions", false, "Keep the interface's function graphs as local functions")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("interface")
	return cmd
}

func editPurgePhantomCmd() *cobra.Command {
	var (
		path, property string
		dryRun         bool
	)
	cmd := &cobra.Command{
		Use:   "purge-phantom",
		Short: "Find and remove phantom properties (NewVariables, graph LocalVariables, GeneratedClass)",
		Long: `Searches for a named property across all locations where it can hide:
  1. Blueprint NewVariables (member variables)
  2. Graph LocalVariables (function-scoped locals)
  3. GeneratedClass (compiled FProperty, stale from prior compile)

Removes from 1 and 2, then forces a full recompile to clear 3.
Use --dry-run to see where the property lives without mutating.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path":     path,
				"property": property,
			}
			if dryRun {
				params["dry_run"] = true
			}
			return callServer("edit.purge_phantom", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&property, "property", "", "Property name to find and remove (required)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Report locations without removing")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("property")
	return cmd
}

func editSetFlagsCmd() *cobra.Command {
	var (
		path                     string
		description, category    string
		isAbstract, isDeprecated bool
	)
	cmd := &cobra.Command{
		Use:   "set-flags",
		Short: "Set Blueprint flags (abstract, deprecated, description, category)",
		Long: `Only flags whose values are explicitly provided are applied.
Use --is-abstract=true/false to set, omit to leave unchanged.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path}
			// Detect whether each bool flag was actually set on the command line,
			// so omission means "leave unchanged" rather than "set to false".
			if cmd.Flags().Changed("is-abstract") {
				params["is_abstract"] = isAbstract
			}
			if cmd.Flags().Changed("is-deprecated") {
				params["is_deprecated"] = isDeprecated
			}
			if description != "" {
				params["description"] = description
			}
			if category != "" {
				params["category"] = category
			}
			return callServer("edit.set_flags", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().BoolVar(&isAbstract, "is-abstract", false, "Mark blueprint abstract (cannot be instantiated)")
	cmd.Flags().BoolVar(&isDeprecated, "is-deprecated", false, "Mark blueprint deprecated")
	cmd.Flags().StringVar(&description, "description", "", "Blueprint description string")
	cmd.Flags().StringVar(&category, "category", "", "Blueprint category")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}
