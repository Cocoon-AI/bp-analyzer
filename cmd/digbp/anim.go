package main

import (
	"github.com/spf13/cobra"
)

// animCmd groups animation-asset inspection under `digbp anim`.
//
// `digbp export` only speaks UBlueprint; blend spaces, anim sequences, and
// montages are UAnimationAssets with no graph, so they get their own
// read-only export that round-trips via the server's `anim.export` JSON-RPC
// method and dispatches on asset class.
func animCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "anim",
		Short: "Inspect non-Blueprint animation assets (BlendSpace, AnimSequence, AnimMontage)",
		Long: `Read-only JSON export of animation assets that 'digbp export' rejects.

Supported classes: BlendSpace, BlendSpace1D, AimOffsetBlendSpace,
AimOffsetBlendSpace1D, AnimSequence, AnimMontage.`,
	}
	cmd.AddCommand(animExportCmd())
	return cmd
}

func animExportCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "export",
		Short: "Export an animation asset as JSON (dispatches on asset class)",
		Long: `Dumps an animation asset as JSON. The payload key depends on the class:

  blend_space    BlendSpace / BlendSpace1D / AimOffsetBlendSpace(1D):
                 axes (display_name, min, max, grid_num, per-axis input
                 interpolation), samples (animation path, x/y, rate_scale,
                 snap_to_grid), target-weight interpolation speed, notify
                 trigger mode, axis_to_scale_animation, preview_base_pose.
  anim_sequence  length, num_frames, frame_rate, rate_scale, interpolation,
                 root-motion settings, additive settings, sync_markers,
                 notifies, curve names.
  anim_montage   length, blend in/out, sync group, slots (segments with
                 animation path and timing), sections, notifies, curve names.

Always present: path, name, asset_class, skeleton.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("anim.export", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Animation asset path (required, e.g. /Game/Characters/Animation/BS_Run)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}
