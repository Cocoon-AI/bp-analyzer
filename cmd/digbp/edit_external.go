package main

import (
	"strings"

	"github.com/spf13/cobra"
)

// editExternalCmd groups cross-BP rewrite operations under `digbp edit external`.
// These commands modify Blueprints OTHER than a single named target — typically
// to follow a C++ rename or class move that BP K2Node refs need to track.
func editExternalCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "external",
		Short: "Cross-BP rewrites (follow C++ renames in BP graph nodes)",
	}
	cmd.AddCommand(editExternalRewriteCallCmd())
	cmd.AddCommand(editExternalRewriteDelegateCmd())
	return cmd
}

func editExternalRewriteCallCmd() *cobra.Command {
	var (
		oldClass, oldName string
		newClass, newName string
		scope             string
		dryRun            bool
		noScanExternal    bool
	)
	cmd := &cobra.Command{
		Use:   "rewrite-call",
		Short: "Rewrite K2Node_CallFunction refs across BPs from (old-class, old-name) to (new-class, new-name)",
		Long: `Walks --scope (default /Game/) for K2Node_CallFunction nodes whose
FunctionReference points at the old (class, name), and rewrites them to the
new (class, name). Saves affected BPs.

Use this when a C++ UFUNCTION is being renamed and BP K2Nodes need to follow
without removing the source function. Mirrors the retarget half of
'function remove --retarget-external-to' but is non-destructive.

--new-class is optional and defaults to --old-class for the pure-rename case.
Pass a different --new-class to support class+function renames (e.g.
USDPlayFabClientSubsystem -> USDClientSubsystem).

Response:
  external_bps_affected, external_nodes_retargeted, external_bps[],
  scanned_external, dry_run.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"old_class": oldClass,
				"old_name":  oldName,
				"new_name":  newName,
			}
			if newClass != "" {
				params["new_class"] = newClass
			}
			if dryRun {
				params["dry_run"] = true
			}
			if noScanExternal {
				params["no_scan_external"] = true
			}
			if scope != "" {
				parts := strings.Split(scope, ",")
				trimmed := make([]string, 0, len(parts))
				for _, p := range parts {
					p = strings.TrimSpace(p)
					if p != "" {
						trimmed = append(trimmed, p)
					}
				}
				if len(trimmed) > 0 {
					params["scope"] = trimmed
				}
			}
			return callServer("edit.external.rewrite_call", params)
		},
	}
	cmd.Flags().StringVar(&oldClass, "old-class", "", "Class that owns the function being renamed (required, e.g. USDPlayFabClientSubsystem)")
	cmd.Flags().StringVar(&oldName, "old-name", "", "Current UFUNCTION name (required)")
	cmd.Flags().StringVar(&newClass, "new-class", "", "New class (optional, defaults to --old-class)")
	cmd.Flags().StringVar(&newName, "new-name", "", "New UFUNCTION name (required)")
	cmd.Flags().StringVar(&scope, "scope", "", "Comma-separated paths to scan (default: /Game/)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Report the plan without mutating any BP")
	cmd.Flags().BoolVar(&noScanExternal, "no-scan-external", false, "Skip the scan entirely (returns success with zero counts)")
	_ = cmd.MarkFlagRequired("old-class")
	_ = cmd.MarkFlagRequired("old-name")
	_ = cmd.MarkFlagRequired("new-name")
	return cmd
}

func editExternalRewriteDelegateCmd() *cobra.Command {
	var (
		oldClass, oldName string
		newClass, newName string
		scope             string
		dryRun            bool
		noScanExternal    bool
	)
	cmd := &cobra.Command{
		Use:   "rewrite-delegate",
		Short: "Rewrite K2Node_BaseMCDelegate refs across BPs from (old-class, old-name) to (new-class, new-name)",
		Long: `Walks --scope (default /Game/) for K2Node_AddDelegate /
RemoveDelegate / ClearDelegate / CallDelegate / AssignDelegate nodes whose
DelegateReference points at the old (class, name), and rewrites them to the
new (class, name). Saves affected BPs.

Use this for renaming a BlueprintAssignable multicast delegate UPROPERTY in
C++ when BP K2Nodes binding to it need to follow.

--new-class is optional and defaults to --old-class for the pure-rename case.

Response shape matches rewrite-call:
  external_bps_affected, external_nodes_retargeted, external_bps[],
  scanned_external, dry_run.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"old_class": oldClass,
				"old_name":  oldName,
				"new_name":  newName,
			}
			if newClass != "" {
				params["new_class"] = newClass
			}
			if dryRun {
				params["dry_run"] = true
			}
			if noScanExternal {
				params["no_scan_external"] = true
			}
			if scope != "" {
				parts := strings.Split(scope, ",")
				trimmed := make([]string, 0, len(parts))
				for _, p := range parts {
					p = strings.TrimSpace(p)
					if p != "" {
						trimmed = append(trimmed, p)
					}
				}
				if len(trimmed) > 0 {
					params["scope"] = trimmed
				}
			}
			return callServer("edit.external.rewrite_delegate", params)
		},
	}
	cmd.Flags().StringVar(&oldClass, "old-class", "", "Class that owns the delegate being renamed (required, e.g. USDPlayFabClientSubsystem)")
	cmd.Flags().StringVar(&oldName, "old-name", "", "Current delegate UPROPERTY name (required)")
	cmd.Flags().StringVar(&newClass, "new-class", "", "New class (optional, defaults to --old-class)")
	cmd.Flags().StringVar(&newName, "new-name", "", "New delegate UPROPERTY name (required)")
	cmd.Flags().StringVar(&scope, "scope", "", "Comma-separated paths to scan (default: /Game/)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Report the plan without mutating any BP")
	cmd.Flags().BoolVar(&noScanExternal, "no-scan-external", false, "Skip the scan entirely (returns success with zero counts)")
	_ = cmd.MarkFlagRequired("old-class")
	_ = cmd.MarkFlagRequired("old-name")
	_ = cmd.MarkFlagRequired("new-name")
	return cmd
}
