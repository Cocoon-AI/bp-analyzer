package main

import (
	"github.com/spf13/cobra"
)

// editComponentCmd groups all SCS component mutation subcommands.
func editComponentCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "component",
		Short: "Edit Simple Construction Script components",
	}
	cmd.AddCommand(
		editComponentAddCmd(),
		editComponentRemoveCmd(),
		editComponentReparentCmd(),
		editComponentSetPropertyCmd(),
	)
	return cmd
}

func editComponentAddCmd() *cobra.Command {
	var path, name, className, parent string
	cmd := &cobra.Command{
		Use:   "add",
		Short: "Add a component to the SCS",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path":  path,
				"name":  name,
				"class": className,
			}
			if parent != "" {
				params["parent"] = parent
			}
			return callServer("edit.component.add", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Component variable name (required)")
	cmd.Flags().StringVar(&className, "class", "", "Component class (required, e.g. /Script/Engine.StaticMeshComponent)")
	cmd.Flags().StringVar(&parent, "parent", "", "Parent component name (optional; omit to attach at root)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	_ = cmd.MarkFlagRequired("class")
	return cmd
}

func editComponentRemoveCmd() *cobra.Command {
	var (
		path, name      string
		noPromoteChildren bool
	)
	cmd := &cobra.Command{
		Use:   "remove",
		Short: "Remove a component from the SCS",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.component.remove", map[string]interface{}{
				"path":             path,
				"name":             name,
				"promote_children": !noPromoteChildren,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Component name (required)")
	cmd.Flags().BoolVar(&noPromoteChildren, "no-promote-children", false, "Delete children instead of promoting them")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	return cmd
}

func editComponentReparentCmd() *cobra.Command {
	var path, name, newParent string
	cmd := &cobra.Command{
		Use:   "reparent",
		Short: "Move a component under a new parent in the SCS hierarchy",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.component.reparent", map[string]interface{}{
				"path":       path,
				"name":       name,
				"new_parent": newParent,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&name, "name", "", "Component name (required)")
	cmd.Flags().StringVar(&newParent, "new-parent", "", "New parent component name (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("name")
	_ = cmd.MarkFlagRequired("new-parent")
	return cmd
}

func editComponentSetPropertyCmd() *cobra.Command {
	var path, component, property, value string
	cmd := &cobra.Command{
		Use:   "set-property",
		Short: "Set a property on a component template (archetype)",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.component.set_property", map[string]interface{}{
				"path":      path,
				"component": component,
				"property":  property,
				"value":     value,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blueprint asset path (required)")
	cmd.Flags().StringVar(&component, "component", "", "Component name (required)")
	cmd.Flags().StringVar(&property, "property", "", "Property name (required)")
	cmd.Flags().StringVar(&value, "value", "", "Value as UE text format (required, may be empty)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("component")
	_ = cmd.MarkFlagRequired("property")
	return cmd
}
