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
