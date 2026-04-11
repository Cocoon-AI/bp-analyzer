package main

import (
	"github.com/spf13/cobra"
)

// editVariableCmd groups all variable mutation subcommands under `digbp edit variable`.
func editVariableCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "variable",
		Short: "Edit Blueprint member variables",
	}
	cmd.AddCommand(
		editVariableAddCmd(),
		editVariableRemoveCmd(),
		editVariableRenameCmd(),
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
	var path, name string
	cmd := &cobra.Command{
		Use:   "remove",
		Short: "Remove a member variable",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.variable.remove", map[string]interface{}{
				"path": path, "name": name,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Variable name (required)")
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
