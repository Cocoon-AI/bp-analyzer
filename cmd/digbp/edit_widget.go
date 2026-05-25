package main

import (
	"github.com/spf13/cobra"
)

// editWidgetCmd groups UMG WidgetTree edit subcommands under
// `digbp edit widget`. Companion to `digbp edit component` (SCS), but for
// UMG widgets in a UWidgetBlueprint.
//
// Read side lives in `digbp export` for WidgetBlueprints — the widget_tree
// field on the export response carries the recursive tree with selected
// style/identification properties.
func editWidgetCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "widget",
		Short: "Edit UMG widgets in a WidgetBlueprint's WidgetTree",
	}
	cmd.AddCommand(editWidgetSetPropertyCmd())
	cmd.AddCommand(editWidgetRenameCmd())
	return cmd
}

func editWidgetRenameCmd() *cobra.Command {
	var path, oldName, newName string
	cmd := &cobra.Command{
		Use:   "rename",
		Short: "Rename a UMG widget and retarget every internal reference",
		Long: `Renames a named UMG widget inside a WidgetBlueprint's WidgetTree and
retargets all internal references in one shot — a headless port of the UMG
editor's own rename. Retargets:
  - the widget's FName + its auto-generated UWidget* member variable
  - K2Node_VariableGet/Set nodes that read the widget (Get/Set <WidgetName>)
  - K2Node_ComponentBoundEvent bindings tied to the widget's delegates
  - property/event delegate bindings, widget-animation bindings, navigation bindings

The new name is treated as a display name and sanitized into an FName the same
way the editor does (spaces/illegal characters stripped); the resolved FName is
returned as new_name.

BindWidget: when the parent class already declares a meta=(BindWidget) UWidget*
property of the new name (with a compatible widget class), the usual name-
collision check is bypassed. This is the rename-to-match-a-C++-BindWidget case
for reparenting BP widgets onto a C++ base.

Examples:
  digbp edit widget rename --path=/Game/Showdown/UI/Elements/SD_EmoteSelectMenu --old-name=EmoteCardHolder --new-name=CardHolder

Workflow note: p4 edit the .uasset before mutation. The rename marks the
Blueprint structurally modified; follow with 'edit save' (or save-and-compile)
to persist.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.widget.rename", map[string]interface{}{
				"path":     path,
				"old_name": oldName,
				"new_name": newName,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "WidgetBlueprint asset path (required)")
	cmd.Flags().StringVar(&oldName, "old-name", "", "Current widget Name (FName) in the WidgetTree (required)")
	cmd.Flags().StringVar(&newName, "new-name", "", "New widget name (required; sanitized to an FName like the editor)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("old-name")
	_ = cmd.MarkFlagRequired("new-name")
	return cmd
}

func editWidgetSetPropertyCmd() *cobra.Command {
	var path, widget, property, value string
	cmd := &cobra.Command{
		Use:   "set-property",
		Short: "Set a property on a UMG widget archetype",
		Long: `Mutates a property on a named UMG widget inside a WidgetBlueprint's
WidgetTree. Mirrors 'digbp edit component set-property' but targets UMG
widgets instead of SCS components.

--property accepts dotted paths through struct properties:
  digbp edit widget set-property --path=/Game/UI/MyWidget_BP --widget=StatusText --property=Text --value="Hello"
  digbp edit widget set-property --path=/Game/UI/MyWidget_BP --widget=StatusText --property=Font.Size --value=24
  digbp edit widget set-property --path=/Game/UI/MyWidget_BP --widget=StatusText --property=Font.FontObject --value=/Game/UI/Fonts/Title.Title
  digbp edit widget set-property --path=/Game/UI/MyWidget_BP --widget=StatusText --property=ColorAndOpacity --value="(R=1.0,G=0.8,B=0.4,A=1.0)"

Value uses UE text format (same as 'cdo set' / 'component set-property').

Workflow note: p4 edit the .uasset before mutation, then 'edit save-and-
compile' to round-trip. No dry-run, no in-tool undo.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("edit.widget.set_property", map[string]interface{}{
				"path":     path,
				"widget":   widget,
				"property": property,
				"value":    value,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "WidgetBlueprint asset path (required)")
	cmd.Flags().StringVar(&widget, "widget", "", "Widget Name (FName) inside the WidgetTree (required)")
	cmd.Flags().StringVar(&property, "property", "", "Property to set, dotted for struct fields (required, e.g. Font.Size)")
	cmd.Flags().StringVar(&value, "value", "", "New value in UE text format (required, may be empty string for clearing)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("widget")
	_ = cmd.MarkFlagRequired("property")
	return cmd
}
