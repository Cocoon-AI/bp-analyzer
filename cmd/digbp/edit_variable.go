package main

import (
	"strings"

	"github.com/spf13/cobra"
)

// editVariableCmd groups all variable mutation subcommands under `digbp edit variable`.
func editVariableCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "variable",
		Short: "Edit Blueprint member variables",
	}
	cmd.AddCommand(
		editVariableListCmd(),
		editVariableAddCmd(),
		editVariableRemoveCmd(),
		editVariableRenameCmd(),
		editVariableUnshadowCmd(),
		editVariableLiftCmd(),
		editVariableSetTypeCmd(),
		editVariableSetDefaultCmd(),
		editVariableSetFlagsCmd(),
		editVariableSetMetadataCmd(),
	)
	return cmd
}

// editCdoCmd groups CDO property get/set under `digbp edit cdo`.
func editCdoCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "cdo",
		Short: "Get/set parent-class default properties on the CDO",
	}
	cmd.AddCommand(
		editCdoGetCmd(),
		editCdoSetCmd(),
	)
	return cmd
}

// --- variable subcommands ---

func editVariableListCmd() *cobra.Command {
	var (
		path          string
		includeBroken bool
	)
	cmd := &cobra.Command{
		Use:   "list",
		Short: "List member variables (use --include-broken for phantom/deleted-type vars)",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path}
			if includeBroken {
				params["include_broken"] = true
			}
			return callServer("edit.variable.list", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().BoolVar(&includeBroken, "include-broken", false, "Include variables with broken/deleted types")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func editVariableAddCmd() *cobra.Command {
	var (
		path, name, typeStr          string
		defaultValue, category, tip  string
		isPublic, isReadOnly, isRepl bool
	)
	cmd := &cobra.Command{
		Use:   "add",
		Short: "Add a new member variable",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path": path,
				"name": name,
				"type": typeStr,
			}
			if defaultValue != "" {
				params["default_value"] = defaultValue
			}
			if category != "" {
				params["category"] = category
			}
			if tip != "" {
				params["tooltip"] = tip
			}
			if cmd.Flags().Changed("public") {
				params["is_public"] = isPublic
			}
			if cmd.Flags().Changed("readonly") {
				params["is_readonly"] = isReadOnly
			}
			if cmd.Flags().Changed("replicated") {
				params["is_replicated"] = isRepl
			}
			return callServer("edit.variable.add", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Variable name (required)")
	cmd.Flags().StringVar(&typeStr, "type", "", "Variable type, e.g. bool, float, object<Actor>, TArray<int> (required)")
	cmd.Flags().StringVar(&defaultValue, "default-value", "", "Default value as text")
	cmd.Flags().StringVar(&category, "category", "", "Variable category")
	cmd.Flags().StringVar(&tip, "tooltip", "", "Tooltip string")
	cmd.Flags().BoolVar(&isPublic, "public", false, "Blueprint-visible / editable in Defaults")
	cmd.Flags().BoolVar(&isReadOnly, "readonly", false, "Read-only in Blueprint")
	cmd.Flags().BoolVar(&isRepl, "replicated", false, "Replicated variable")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	_ = cmd.MarkFlagRequired("type")
	return cmd
}

func editVariableRemoveCmd() *cobra.Command {
	var (
		path, name string
		force      bool
	)
	cmd := &cobra.Command{
		Use:   "remove",
		Short: "Remove a member variable (use --force for phantom/broken-type vars)",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path": path, "name": name,
			}
			if force {
				params["force"] = true
			}
			return callServer("edit.variable.remove", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Variable name (required)")
	cmd.Flags().BoolVar(&force, "force", false, "Force-remove even if type is broken/unresolvable")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editVariableRenameCmd() *cobra.Command {
	var path, oldName, newName string
	cmd := &cobra.Command{
		Use:   "rename",
		Short: "Rename a member variable",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.variable.rename", map[string]interface{}{
				"path":     path,
				"old_name": oldName,
				"new_name": newName,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&oldName, "old-name", "", "Current variable name (required)")
	cmd.Flags().StringVar(&newName, "new-name", "", "New variable name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("old-name")
	_ = cmd.MarkFlagRequired("new-name")
	return cmd
}

func editVariableUnshadowCmd() *cobra.Command {
	var (
		path   string
		dryRun bool
	)
	cmd := &cobra.Command{
		Use:   "unshadow",
		Short: "Retarget K2Node refs from <X>_0 shadow vars back to parent <X>; remove shadows",
		Long: `When a BP author adds a C++ UPROPERTY to the parent class while the BP still
has a member variable of the same name, UE renames the BP var to <X>_0 AND
retargets every K2Node_VariableGet/Set node to the new _0 name. This op
detects that state and puts it right:

  - Finds BP vars named <X>_0 where the parent class exposes <X>
  - Retargets K2Node_Variable and K2Node_BaseMCDelegate nodes from <X>_0 to <X>
  - Removes the <X>_0 vars
  - Saves (no compile — caller compiles separately)

Idempotent: running on an already-clean BP reports zero actions and success.
Use --dry-run to preview without mutating.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path}
			if dryRun {
				params["dry_run"] = true
			}
			return callServer("edit.variable.unshadow", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Report the plan without mutating the BP")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func editVariableLiftCmd() *cobra.Command {
	var (
		path            string
		vars            string
		scope           string
		dryRun          bool
		noScanExternal  bool
	)
	cmd := &cobra.Command{
		Use:   "lift",
		Short: "Atomic multi-var lift: rename each var to a C++-friendly name, then remove, and retarget external BP callers",
		Long: `For each comma-separated var, renames to a C++-identifier-friendly form
(strips spaces, uppercases the first letter of each whitespace-separated
segment; "XP Level Threshold" → "XPLevelThreshold") and then removes the
variable. Collapses the first three steps of the BP→C++ lift workflow into
one atomic call.

External-BP retargeting (default ON): after the rename+remove, scans --scope
for K2Node_VariableGet/Set in OTHER BPs that bound against the pre-rename
name on this BP's class, retargets them to the new name, marks the affected
BPs structurally modified, and saves them. Without this, external callers
silently orphan when the BP loses the old var name. Use --no-scan-external
to skip the scan (fast, but you should verify zero external refs with
'digbp findvaruses --var=<OldName>' first).

Reports final names, any requested vars that weren't found (with the list
of available vars for fuzzy matching client-side), any target-name
collisions, and the count of external BPs retargeted. Does not compile —
caller compiles separately after writing the replacement C++ UPROPERTYs.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			// Split + trim the user-supplied CSV. Keeps spaces inside var names
			// (e.g. "XP Level Threshold") intact.
			parts := strings.Split(vars, ",")
			trimmed := make([]string, 0, len(parts))
			for _, p := range parts {
				p = strings.TrimSpace(p)
				if p != "" {
					trimmed = append(trimmed, p)
				}
			}
			params := map[string]interface{}{
				"path": path,
				"vars": trimmed,
			}
			if dryRun {
				params["dry_run"] = true
			}
			if noScanExternal {
				params["no_scan_external"] = true
			}
			if scope != "" {
				scopeParts := strings.Split(scope, ",")
				scopeTrimmed := make([]string, 0, len(scopeParts))
				for _, s := range scopeParts {
					s = strings.TrimSpace(s)
					if s != "" {
						scopeTrimmed = append(scopeTrimmed, s)
					}
				}
				if len(scopeTrimmed) > 0 {
					params["scope"] = scopeTrimmed
				}
			}
			return callServer("edit.variable.lift", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&vars, "vars", "", `Comma-separated BP variable names to lift, e.g. --vars="XP Level Threshold,Current XP,Beta XP Key" (required)`)
	cmd.Flags().StringVar(&scope, "scope", "", "Comma-separated paths to scan for external BP callers (default: /Game/)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Report the plan without mutating the BP (also skips the external-BP scan)")
	cmd.Flags().BoolVar(&noScanExternal, "no-scan-external", false, "Skip scanning OTHER BPs for K2Node refs to retarget (default: scan is ON)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("vars")
	return cmd
}

func editVariableSetTypeCmd() *cobra.Command {
	var path, name, typeStr string
	cmd := &cobra.Command{
		Use:   "set-type",
		Short: "Change a variable's type",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.variable.set_type", map[string]interface{}{
				"path": path, "name": name, "type": typeStr,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Variable name (required)")
	cmd.Flags().StringVar(&typeStr, "type", "", "New type string (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	_ = cmd.MarkFlagRequired("type")
	return cmd
}

func editVariableSetDefaultCmd() *cobra.Command {
	var path, name, value string
	cmd := &cobra.Command{
		Use:   "set-default",
		Short: "Set a variable's default value",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.variable.set_default", map[string]interface{}{
				"path": path, "name": name, "default_value": value,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Variable name (required)")
	cmd.Flags().StringVar(&value, "value", "", "New default value (required, may be empty string)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editVariableSetFlagsCmd() *cobra.Command {
	var (
		path, name, replCond      string
		isPublic, isReadOnly, isRepl bool
	)
	cmd := &cobra.Command{
		Use:   "set-flags",
		Short: "Update variable flags (public/readonly/replicated)",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path, "name": name}
			if cmd.Flags().Changed("public") {
				params["is_public"] = isPublic
			}
			if cmd.Flags().Changed("readonly") {
				params["is_readonly"] = isReadOnly
			}
			if cmd.Flags().Changed("replicated") {
				params["is_replicated"] = isRepl
			}
			if replCond != "" {
				params["replication_condition"] = replCond
			}
			return callServer("edit.variable.set_flags", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Variable name (required)")
	cmd.Flags().BoolVar(&isPublic, "public", false, "Blueprint-visible / editable in Defaults")
	cmd.Flags().BoolVar(&isReadOnly, "readonly", false, "Read-only in Blueprint")
	cmd.Flags().BoolVar(&isRepl, "replicated", false, "Replicated")
	cmd.Flags().StringVar(&replCond, "replication-condition", "", "ELifetimeCondition name (e.g. OwnerOnly)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editVariableSetMetadataCmd() *cobra.Command {
	var path, name, key, value string
	cmd := &cobra.Command{
		Use:   "set-metadata",
		Short: "Set a metadata key/value on a variable",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.variable.set_metadata", map[string]interface{}{
				"path": path, "name": name, "key": key, "value": value,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Variable name (required)")
	cmd.Flags().StringVar(&key, "key", "", "Metadata key (required)")
	cmd.Flags().StringVar(&value, "value", "", "Metadata value (required, may be empty string)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	_ = cmd.MarkFlagRequired("key")
	return cmd
}

// --- CDO subcommands ---

func editCdoGetCmd() *cobra.Command {
	var path, prop string
	cmd := &cobra.Command{
		Use:   "get",
		Short: "Read a property value from the CDO",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.cdo.get_property", map[string]interface{}{
				"path": path, "property_name": prop,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&prop, "property", "", "Property name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("property")
	return cmd
}

func editCdoSetCmd() *cobra.Command {
	var path, prop, value string
	cmd := &cobra.Command{
		Use:   "set",
		Short: "Set a property value on the CDO (e.g. bCanBeDamaged=true on AActor)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.cdo.set_property", map[string]interface{}{
				"path": path, "property_name": prop, "value": value,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&prop, "property", "", "Property name (required)")
	cmd.Flags().StringVar(&value, "value", "", "Value as UE text format (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("property")
	return cmd
}
