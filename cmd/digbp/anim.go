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
AimOffsetBlendSpace1D, AnimSequence, AnimMontage.

Mutations ('anim import', 'anim edit ...') follow the datatable/font model:
'anim import' saves immediately; 'anim edit' ops stage in memory and persist
with 'anim save'. SCC (p4 edit / git) is the caller's job.`,
	}
	cmd.AddCommand(
		animExportCmd(),
		animImportCmd(),
		animEditCmd(),
		animSaveCmd(),
	)
	return cmd
}

func animImportCmd() *cobra.Command {
	var (
		fbx           string
		skeleton      string
		dest          string
		preserveLocal bool
	)
	cmd := &cobra.Command{
		Use:   "import",
		Short: "Import an FBX animation as a UAnimSequence on a skeleton",
		Long: `Automated FBX animation import (no dialogs): creates or overwrites the
UAnimSequence at --dest, binding it to --skeleton. Imports animation only
(no mesh/materials/textures), exported-time length, bone tracks on.

Unlike other anim mutations this SAVES the asset immediately — batch loops
don't stage hundreds of imports in server memory. Overwrite of an existing
asset is in-place (reimport semantics). SCC checkout is the caller's job.

--preserve-local-transform imports the authored FBX local transforms
directly instead of UE's default local-from-global reconstruction. Use it
when the default path corrupts specific bones (observed signature: one
bone wrong by some angle, every direct child inheriting exactly that
angle, grandchildren unaffected). Note the root bone's transform then also
comes straight from the FBX — any axis-conversion behavior baked into the
default path's root reconstruction (e.g. a +90deg X) may change, so
re-baseline root handling when flipping this on.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"fbx":      fbx,
				"skeleton": skeleton,
				"dest":     dest,
			}
			if preserveLocal {
				params["preserve_local"] = true
			}
			return callServer("anim.import", params)
		},
	}
	cmd.Flags().BoolVar(&preserveLocal, "preserve-local-transform", false, "Import authored FBX locals directly (skip local-from-global reconstruction)")
	cmd.Flags().StringVar(&fbx, "fbx", "", "Absolute path to the FBX file (required)")
	cmd.Flags().StringVar(&skeleton, "skeleton", "", "Skeleton asset path (required, e.g. /Game/Showdown/Characters/Animation/SKEL_Gunslinger)")
	cmd.Flags().StringVar(&dest, "dest", "", "Destination asset path incl. name (required, e.g. /Game/Showdown/Characters/Animation/Cowboy/Mounted/AS_Pose_01)")
	_ = cmd.MarkFlagRequired("fbx")
	_ = cmd.MarkFlagRequired("skeleton")
	_ = cmd.MarkFlagRequired("dest")
	return cmd
}

func animEditCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "edit",
		Short: "Mutate animation assets (staged; persist with 'anim save')",
	}
	cmd.AddCommand(
		animEditSetAdditiveCmd(),
		animEditBlendspaceCmd(),
	)
	return cmd
}

func animEditSetAdditiveCmd() *cobra.Command {
	var (
		path         string
		additiveType string
		basePose     string
		basePoseType string
		refFrame     int
	)
	cmd := &cobra.Command{
		Use:   "set-additive",
		Short: "Set additive settings on an AnimSequence",
		Long: `Sets AdditiveAnimType (+ optional base pose) with the editor's commit
bracketing, which triggers the additive rebake. --type=None clears all
additive settings (base pose type/animation/frame reset too).

Only flags you pass are applied; omitted base-pose flags leave the current
values. Stages in memory; follow with 'anim save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path": path,
				"type": additiveType,
			}
			if basePose != "" {
				params["base_pose"] = basePose
			}
			if basePoseType != "" {
				params["base_pose_type"] = basePoseType
			}
			if cmd.Flags().Changed("ref-frame") {
				params["ref_frame"] = refFrame
			}
			return callServer("anim.set_additive", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "AnimSequence asset path (required)")
	cmd.Flags().StringVar(&additiveType, "type", "", "Additive type: None | LocalSpaceBase | RotationOffsetMeshSpace (required)")
	cmd.Flags().StringVar(&basePose, "base-pose", "", "Base pose AnimSequence asset path")
	cmd.Flags().StringVar(&basePoseType, "base-pose-type", "", "Base pose type: None | RefPose | AnimScaled | AnimFrame")
	cmd.Flags().IntVar(&refFrame, "ref-frame", 0, "Base pose frame index (with --base-pose-type=AnimFrame)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("type")
	return cmd
}

func animEditBlendspaceCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "blendspace",
		Short: "Mutate blend space assets (grid retriangulated on every change)",
		Long: `Blend space mutations. Every op ends with a full grid resample using the
engine's own triangulation (vendored from the Persona blend space editor),
so the asset blends correctly at runtime without ever being opened in the
editor. Stages in memory; persist with 'anim save'.`,
	}
	cmd.AddCommand(
		animEditBlendspaceCreateCmd(),
		animEditBlendspaceSetAxisCmd(),
		animEditBlendspaceAddSampleCmd(),
		animEditBlendspaceRemoveSampleCmd(),
	)
	return cmd
}

func animEditBlendspaceCreateCmd() *cobra.Command {
	var (
		dest     string
		class    string
		skeleton string
	)
	cmd := &cobra.Command{
		Use:   "create",
		Short: "Create a new empty blend space asset",
		Long: `Creates an empty blend space of the given class on the skeleton. Follow
with 'set-axis' + 'add-sample' + 'anim save'. Fails if the asset exists.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("anim.blendspace_create", map[string]interface{}{
				"dest":     dest,
				"class":    class,
				"skeleton": skeleton,
			})
		},
	}
	cmd.Flags().StringVar(&dest, "dest", "", "Destination asset path incl. name (required)")
	cmd.Flags().StringVar(&class, "class", "", "BlendSpace | BlendSpace1D | AimOffsetBlendSpace | AimOffsetBlendSpace1D (required)")
	cmd.Flags().StringVar(&skeleton, "skeleton", "", "Skeleton asset path (required)")
	_ = cmd.MarkFlagRequired("dest")
	_ = cmd.MarkFlagRequired("class")
	_ = cmd.MarkFlagRequired("skeleton")
	return cmd
}

func animEditBlendspaceSetAxisCmd() *cobra.Command {
	var (
		path    string
		axis    int
		name    string
		min     float64
		max     float64
		gridNum int
	)
	cmd := &cobra.Command{
		Use:   "set-axis",
		Short: "Set a blend space axis (name, range, grid divisions)",
		Long: `Sets axis properties; only flags you pass are applied. Axis 0 is the only
valid axis on 1D classes. Changing min/max remaps existing sample positions
into the new range and changing grid-num snaps samples to the new grid —
both editor-parity behaviors. The grid is retriangulated afterward.

Stages in memory; follow with 'anim save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path": path,
				"axis": axis,
			}
			if name != "" {
				params["name"] = name
			}
			if cmd.Flags().Changed("min") {
				params["min"] = min
			}
			if cmd.Flags().Changed("max") {
				params["max"] = max
			}
			if cmd.Flags().Changed("grid-num") {
				params["grid_num"] = gridNum
			}
			return callServer("anim.blendspace_set_axis", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blend space asset path (required)")
	cmd.Flags().IntVar(&axis, "axis", -1, "Axis index: 0 or 1 (required)")
	cmd.Flags().StringVar(&name, "name", "", "Axis display name")
	cmd.Flags().Float64Var(&min, "min", 0, "Axis minimum value")
	cmd.Flags().Float64Var(&max, "max", 0, "Axis maximum value")
	cmd.Flags().IntVar(&gridNum, "grid-num", 0, "Number of grid divisions (>= 1)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("axis")
	return cmd
}

func animEditBlendspaceAddSampleCmd() *cobra.Command {
	var (
		path      string
		animation string
		x         float64
		y         float64
	)
	cmd := &cobra.Command{
		Use:   "add-sample",
		Short: "Add an animation sample at (x, y)",
		Long: `Adds a sample via the engine's validated AddSample: rejects skeleton
mismatches, additive-type mismatches with existing samples, out-of-range
values, and near-duplicate points. --y is ignored for 1D classes. The grid
is retriangulated afterward.

Stages in memory; follow with 'anim save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("anim.blendspace_add_sample", map[string]interface{}{
				"path":      path,
				"animation": animation,
				"x":         x,
				"y":         y,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blend space asset path (required)")
	cmd.Flags().StringVar(&animation, "animation", "", "AnimSequence asset path (required)")
	cmd.Flags().Float64Var(&x, "x", 0, "Sample coordinate on axis 0 (required)")
	cmd.Flags().Float64Var(&y, "y", 0, "Sample coordinate on axis 1 (2D classes)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("animation")
	_ = cmd.MarkFlagRequired("x")
	return cmd
}

func animEditBlendspaceRemoveSampleCmd() *cobra.Command {
	var (
		path  string
		index int
	)
	cmd := &cobra.Command{
		Use:   "remove-sample",
		Short: "Remove a sample by index",
		Long: `Removes the sample at the given index (as reported by 'anim export').
WARNING: engine removal swaps the last sample into the removed slot, so
remaining indices change — re-run 'anim export' before removing another.
The grid is retriangulated afterward.

Stages in memory; follow with 'anim save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("anim.blendspace_remove_sample", map[string]interface{}{
				"path":  path,
				"index": index,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Blend space asset path (required)")
	cmd.Flags().IntVar(&index, "index", -1, "Sample index from 'anim export' (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("index")
	return cmd
}

func animSaveCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "save",
		Short: "Persist a dirty animation asset's package to disk",
		Long: `Generic asset save (anim assets are not Blueprints; 'edit save' does not
apply). Same semantics as 'font save' — bOnlyDirty=true, and {saved: false,
not_dirty: true} is a distinct success when the package is already clean.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("anim.save", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Animation asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func animExportCmd() *cobra.Command {
	var (
		path   string
		tracks bool
		frame  int
	)
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

Always present: path, name, asset_class, skeleton.

--tracks (AnimSequence only) adds tracks, tracks_frame, and tracks_space as
TOP-LEVEL response keys (siblings of anim_sequence, not nested inside it):
tracks: [{bone, skeleton_bone_index, pos:[x,y,z], rot:[x,y,z,w],
scale:[x,y,z]}] for the frame given by --frame (default 0). Transforms are bone-local (relative to parent), read from the
uncompressed raw import keys — compression does not affect them, so they are
suitable as a round-trip fidelity oracle against source poses. Only animated
(authored-track) bones appear; constant tracks report their single key.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path}
			if tracks {
				params["tracks"] = true
				params["frame"] = frame
			}
			return callServer("anim.export", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "Animation asset path (required, e.g. /Game/Characters/Animation/BS_Run)")
	cmd.Flags().BoolVar(&tracks, "tracks", false, "Include per-bone local-space transforms at --frame (AnimSequence only)")
	cmd.Flags().IntVar(&frame, "frame", 0, "Frame to sample for --tracks (0-based)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}
