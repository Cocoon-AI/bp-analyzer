package main

import (
	"github.com/spf13/cobra"
)

// editFunctionCmd groups function and custom-event subcommands under
// `digbp edit function` and `digbp edit event` (the two live in the same file
// because they share the ubergraph concept and the same set of params).
func editFunctionCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "function",
		Short: "Edit Blueprint functions",
	}
	cmd.AddCommand(
		editFunctionAddCmd(),
		editFunctionRemoveCmd(),
		editFunctionRenameCmd(),
		editFunctionAddParamCmd(),
		editFunctionRemoveParamCmd(),
		editFunctionSetFlagsCmd(),
		editFunctionOverrideCmd(),
	)
	return cmd
}

func editEventCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "event",
		Short: "Edit Blueprint events (custom events and inherited event implementations)",
	}
	cmd.AddCommand(
		editEventAddCustomCmd(),
		editEventRemoveCmd(),
		editEventImplementCmd(),
	)
	return cmd
}

// --- function subcommands ---

func editFunctionAddCmd() *cobra.Command {
	var (
		path, name, category string
		isPure, isConst      bool
	)
	cmd := &cobra.Command{
		Use:   "add",
		Short: "Add a new user function",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path, "name": name}
			if cmd.Flags().Changed("pure") {
				params["is_pure"] = isPure
			}
			if cmd.Flags().Changed("const") {
				params["is_const"] = isConst
			}
			if category != "" {
				params["category"] = category
			}
			return callServer("edit.function.add", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Function name (required)")
	cmd.Flags().BoolVar(&isPure, "pure", false, "Mark as BlueprintPure")
	cmd.Flags().BoolVar(&isConst, "const", false, "Mark as Const")
	cmd.Flags().StringVar(&category, "category", "", "Function category")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editFunctionRemoveCmd() *cobra.Command {
	var path, name string
	cmd := &cobra.Command{
		Use:   "remove",
		Short: "Remove a user function",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.function.remove", map[string]interface{}{
				"path": path, "name": name,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Function name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editFunctionRenameCmd() *cobra.Command {
	var path, oldName, newName string
	cmd := &cobra.Command{
		Use:   "rename",
		Short: "Rename a user function",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.function.rename", map[string]interface{}{
				"path": path, "old_name": oldName, "new_name": newName,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&oldName, "old-name", "", "Current function name (required)")
	cmd.Flags().StringVar(&newName, "new-name", "", "New function name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("old-name")
	_ = cmd.MarkFlagRequired("new-name")
	return cmd
}

func editFunctionAddParamCmd() *cobra.Command {
	var path, fn, name, typeStr, direction string
	cmd := &cobra.Command{
		Use:   "add-param",
		Short: "Add an input or output parameter to a function",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.function.add_param", map[string]interface{}{
				"path":      path,
				"function":  fn,
				"name":      name,
				"type":      typeStr,
				"direction": direction,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&fn, "function", "", "Function name (required)")
	cmd.Flags().StringVar(&name, "name", "", "Parameter name (required)")
	cmd.Flags().StringVar(&typeStr, "type", "", "Parameter type (required)")
	cmd.Flags().StringVar(&direction, "direction", "input", "input or output")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("function")
	_ = cmd.MarkFlagRequired("name")
	_ = cmd.MarkFlagRequired("type")
	return cmd
}

func editFunctionRemoveParamCmd() *cobra.Command {
	var path, fn, name string
	cmd := &cobra.Command{
		Use:   "remove-param",
		Short: "Remove a parameter from a function",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.function.remove_param", map[string]interface{}{
				"path": path, "function": fn, "name": name,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&fn, "function", "", "Function name (required)")
	cmd.Flags().StringVar(&name, "name", "", "Parameter name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("function")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editFunctionSetFlagsCmd() *cobra.Command {
	var (
		path, fn        string
		isPure, isConst bool
	)
	cmd := &cobra.Command{
		Use:   "set-flags",
		Short: "Update function flags (pure/const)",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path, "function": fn}
			if cmd.Flags().Changed("pure") {
				params["is_pure"] = isPure
			}
			if cmd.Flags().Changed("const") {
				params["is_const"] = isConst
			}
			return callServer("edit.function.set_flags", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&fn, "function", "", "Function name (required)")
	cmd.Flags().BoolVar(&isPure, "pure", false, "Mark as BlueprintPure")
	cmd.Flags().BoolVar(&isConst, "const", false, "Mark as Const")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("function")
	return cmd
}

func editFunctionOverrideCmd() *cobra.Command {
	var path, fn string
	cmd := &cobra.Command{
		Use:   "override",
		Short: "Create an override graph for a parent-class function",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.function.override", map[string]interface{}{
				"path": path, "function": fn,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&fn, "function", "", "Parent function name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("function")
	return cmd
}

// --- event subcommands ---

func editEventAddCustomCmd() *cobra.Command {
	var (
		path, name string
		x, y       int
		paramSpecs []string
	)
	cmd := &cobra.Command{
		Use:   "add-custom",
		Short: "Add a custom event to the event graph",
		Long: `Params may be given as --param Name:Type, repeatable. Example:
  digbp edit event add-custom --path=... --name=OnDeath \
    --param="Killer:object<Controller>" --param="Damage:float"`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path, "name": name}
			if cmd.Flags().Changed("x") {
				params["x"] = x
			}
			if cmd.Flags().Changed("y") {
				params["y"] = y
			}
			if len(paramSpecs) > 0 {
				parsed := make([]map[string]interface{}, 0, len(paramSpecs))
				for _, spec := range paramSpecs {
					// spec is "name:type" — split on first colon.
					idx := -1
					for i := 0; i < len(spec); i++ {
						if spec[i] == ':' {
							idx = i
							break
						}
					}
					if idx < 0 {
						continue
					}
					parsed = append(parsed, map[string]interface{}{
						"name": spec[:idx],
						"type": spec[idx+1:],
					})
				}
				params["params"] = parsed
			}
			return callServer("edit.event.add_custom", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Custom event name (required)")
	cmd.Flags().IntVar(&x, "x", 0, "X position in the graph")
	cmd.Flags().IntVar(&y, "y", 0, "Y position in the graph")
	cmd.Flags().StringArrayVar(&paramSpecs, "param", nil, "Parameter as Name:Type (repeatable)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editEventRemoveCmd() *cobra.Command {
	var path, name string
	cmd := &cobra.Command{
		Use:   "remove",
		Short: "Remove a custom or implemented event",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.event.remove", map[string]interface{}{
				"path": path, "name": name,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Event name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editEventImplementCmd() *cobra.Command {
	var path, eventName string
	cmd := &cobra.Command{
		Use:   "implement",
		Short: "Add an implementation stub for a parent BlueprintEvent",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.event.implement", map[string]interface{}{
				"path": path, "event": eventName,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&eventName, "event", "", "Parent event name, e.g. ReceiveBeginPlay (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("event")
	return cmd
}
