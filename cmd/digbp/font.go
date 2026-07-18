package main

import (
	"strings"

	"github.com/spf13/cobra"
)

// fontCmd groups UFont composite-font inspection + mutation under `digbp font`.
//
// UFont::CompositeFont is a protected UPROPERTY, so editor Python can't touch
// it — these commands round-trip via the server's `font.{export,add_subfont,
// set_fallback,remove_subfont,save}` JSON-RPC methods, which reach the struct
// through native reflection. Built for l10n fallback: route CJK/Cyrillic
// codepoints to project FontFace assets (Noto) instead of engine
// DroidSansFallback.
func fontCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "font",
		Short: "Inspect and mutate UFont composite fonts (l10n fallback typefaces)",
		Long: `Inspect and mutate the CompositeFont of a runtime-cached UFont asset.

Slate resolves each codepoint against sub-typefaces first (in array order,
first CharacterRanges+Cultures match wins), then the fallback typeface, then
the engine's last-resort font. Typeface-entry names ("Regular", "Black") are
matched loosely: if the requested face name has no entry in the matched
sub-font/fallback, Slate uses the best/first entry — so a single Regular
entry serves all styles of the base font.

Mutations stage in memory; follow with 'font save' to persist. SCC (p4 edit /
git) is the caller's job.`,
	}
	cmd.AddCommand(
		fontExportCmd(),
		fontAddSubfontCmd(),
		fontSetFallbackCmd(),
		fontRemoveSubfontCmd(),
		fontSaveCmd(),
	)
	return cmd
}

func fontExportCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "export",
		Short: "Dump a UFont's CompositeFont as JSON",
		Long: `Returns { font_cache_type, composite_font: { default_typeface, fallback,
sub_typefaces } }. Sub-typeface indices in the output are the handles that
'font remove-subfont --index' takes. Character-range bounds are normalized
to inclusive codepoints and reported both decimal and hex.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("font.export", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "UFont asset path (required, e.g. /Game/Showdown/UI/Fonts/DisplayFont)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func fontAddSubfontCmd() *cobra.Command {
	var (
		path          string
		fontFace      string
		ranges        []string
		cultures      []string
		name          string
		editorName    string
		scalingFactor float64
		before        int
	)
	cmd := &cobra.Command{
		Use:   "add-subfont",
		Short: "Append a culture/range-filtered sub-typeface to a UFont",
		Long: `Appends an FCompositeSubFont whose typeface points at the given FontFace
asset. At least one --range is required: Slate only consults sub-typefaces
through their character-range table, so a range-less sub-font never matches
(use 'font set-fallback' for a catch-all instead).

Ranges are unicode-hex, inclusive, comma-separated or repeated:
  --range=4E00-9FFF --range=3040-30FF,31F0-31FF --range=3005
Common blocks: CJK Unified 4E00-9FFF (+Ext-A 3400-4DBF), Hiragana 3040-309F,
Katakana 30A0-30FF, Hangul AC00-D7AF (+Jamo 1100-11FF), CJK punctuation
3000-303F, full/half-width forms FF00-FFEF, Cyrillic 0400-04FF.

--cultures is an optional filter (e.g. --cultures=zh-Hans,zh-Hant); empty
means the sub-font applies in every culture. Sub-fonts are matched in array
order — first match wins. By default this command appends; when ranges
overlap an existing sub-font, use --before=N to insert ahead of it (indices
from 'font export').

Stages in memory; follow with 'font save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{
				"path":      path,
				"font_face": fontFace,
				"ranges":    strings.Join(ranges, ","),
			}
			if len(cultures) > 0 {
				params["cultures"] = strings.Join(cultures, ",")
			}
			if name != "" {
				params["name"] = name
			}
			if editorName != "" {
				params["editor_name"] = editorName
			}
			params["scaling_factor"] = scalingFactor
			if before >= 0 {
				params["before"] = before
			}
			return callServer("font.add_subfont", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "UFont asset path (required)")
	cmd.Flags().StringVar(&fontFace, "font-face", "", "FontFace asset path (required, e.g. /Game/Showdown/UI/Fonts/NotoSansCJKjp-Regular)")
	cmd.Flags().StringSliceVar(&ranges, "range", nil, "Unicode-hex character range(s), inclusive (required, e.g. 4E00-9FFF; repeat or comma-separate)")
	cmd.Flags().StringSliceVar(&cultures, "cultures", nil, "Culture filter (optional, e.g. zh-Hans,zh-Hant; empty = all cultures)")
	cmd.Flags().StringVar(&name, "name", "Regular", "Typeface entry name")
	cmd.Flags().StringVar(&editorName, "editor-name", "", "Editor-UI display name for the sub-font (default: FontFace asset name)")
	cmd.Flags().Float64Var(&scalingFactor, "scaling-factor", 1.0, "Scale of this sub-font relative to the base font")
	cmd.Flags().IntVar(&before, "before", -1, "Insert ahead of this sub-typeface index instead of appending (first match wins)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("font-face")
	_ = cmd.MarkFlagRequired("range")
	return cmd
}

func fontSetFallbackCmd() *cobra.Command {
	var (
		path          string
		fontFace      string
		name          string
		scalingFactor float64
	)
	cmd := &cobra.Command{
		Use:   "set-fallback",
		Short: "Set/replace a UFont's last-resort fallback typeface",
		Long: `Replaces the FallbackTypeface with a single entry pointing at the given
FontFace asset. The fallback is consulted for any codepoint no default or
sub-typeface covers — the catch-all tier above the engine's own
DroidSansFallback. The replaced entries are echoed back as
'previous_typeface' in the response.

Stages in memory; follow with 'font save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("font.set_fallback", map[string]interface{}{
				"path":           path,
				"font_face":      fontFace,
				"name":           name,
				"scaling_factor": scalingFactor,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "UFont asset path (required)")
	cmd.Flags().StringVar(&fontFace, "font-face", "", "FontFace asset path (required)")
	cmd.Flags().StringVar(&name, "name", "Regular", "Typeface entry name")
	cmd.Flags().Float64Var(&scalingFactor, "scaling-factor", 1.0, "Scale of the fallback relative to the base font")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("font-face")
	return cmd
}

func fontRemoveSubfontCmd() *cobra.Command {
	var (
		path  string
		index int
	)
	cmd := &cobra.Command{
		Use:   "remove-subfont",
		Short: "Remove a sub-typeface by index",
		Long: `Removes the sub-typeface at the given index (as reported by 'font export').
Later sub-typefaces shift down one — re-export before removing another.
The removed sub-font is echoed back in the response for reconstruction.

Stages in memory; follow with 'font save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("font.remove_subfont", map[string]interface{}{
				"path":  path,
				"index": index,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "UFont asset path (required)")
	cmd.Flags().IntVar(&index, "index", -1, "Sub-typeface index from 'font export' (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("index")
	return cmd
}

func fontSaveCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "save",
		Short: "Persist a dirty UFont's package to disk",
		Long: `Generic asset save (fonts are not Blueprints; 'edit save' does not apply).
Same semantics as 'datatable save' — bOnlyDirty=true, and {saved: false,
not_dirty: true} is a distinct success when the package is already clean.
SCC checkout (p4 edit / git) is the caller's job.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("font.save", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "UFont asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}
