package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"github.com/spf13/cobra"

	"github.com/cocoonai/bp-analyzer/internal/rpc"
	"github.com/cocoonai/bp-analyzer/internal/server"
)

// levelCmd groups read-only level / World Composition / landscape inspection.
//
// The server loads the .umap package headless (LoadPackage + FindWorldInPackage;
// no world init, no streaming). World Composition tiles come from package
// summaries and are never fully loaded. Tile-level actor transforms are
// tile-local as authored; world_* fields add the tile's absolute offset.
func levelCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "level",
		Short: "Inspect level packages: World Composition tiles, actors, landscape samples (read-only)",
		Long: `Read-only JSON export of .umap packages. Loads the package headless
(no world init, no streaming). World Composition tiles are read from package
summaries, never fully loaded. Tile-level actor transforms are tile-local as
authored; 'world_*' fields add the tile's absolute offset.`,
	}
	cmd.AddCommand(levelListCmd(), levelTilesCmd(), levelActorsCmd(), levelLandscapeCmd())
	return cmd
}

func levelListCmd() *cobra.Command {
	var dir string
	cmd := &cobra.Command{
		Use:   "list",
		Short: "List map packages (UWorld assets) under a folder",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("level.list", map[string]interface{}{"dir": dir})
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Content folder, e.g. /Game/Showdown/Maps/Sunrise/Sunrise_SubLevels (required)")
	_ = cmd.MarkFlagRequired("dir")
	return cmd
}

func levelTilesCmd() *cobra.Command {
	var p, out string
	cmd := &cobra.Command{
		Use:   "tiles",
		Short: "World Composition overview: every tile's layer, position, bounds, LODs, streaming class",
		Long: `Loads the persistent level and reads its UWorldComposition tile table
(package summaries only — tiles are not loaded). Also includes the persistent
level's own actors (bounds-only shape). For a non-composition map,
world_composition is false and tiles is empty.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": p}
			if out != "" {
				return callServerToFile("level.tiles", params, out)
			}
			return callServer("level.tiles", params)
		},
	}
	cmd.Flags().StringVar(&p, "path", "", "Persistent level package, e.g. /Game/Showdown/Maps/Sunrise/Sunrise (required)")
	cmd.Flags().StringVar(&out, "out", "", "Write JSON to file instead of stdout")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func levelActorsCmd() *cobra.Command {
	var (
		p, dir, out, classes  string
		boundsOnly, instances bool
	)
	cmd := &cobra.Command{
		Use:   "actors",
		Short: "Dump a level's actors (class chain, folder, tags, transform, bounds, components)",
		Long: `Single level: --path=<map> [--out=file]
Batch:        --dir=<folder> --out=<dir>   (one <ShortName>.json per map; the
              server unloads each map after export so memory stays flat)

Per actor: name/label, class + nearest native_class + blueprint_chain, folder,
tags, transform, bounds, world_location/world_bounds (tile offset applied),
and components: StaticMesh (mesh, materials, bounds), ISM/HISM (instance_count,
--instances adds per-instance transforms), ChildActor (child_actor_class),
Spline (points), Brush/volumes (bounds). Landscape proxies get a 'landscape'
summary (component count, section size, layer names); InstancedFoliageActors
get 'foliage' (type -> instance count); volumes get 'volume'.

--class accepts a comma list; matches the actor class, any Blueprint parent, or
any native ancestor (e.g. --class=Volume,BP_LootGenerator).
--bounds-only omits per-component detail.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			if (p == "") == (dir == "") {
				return fmt.Errorf("exactly one of --path or --dir is required")
			}
			params := map[string]interface{}{"bounds_only": boundsOnly, "instances": instances}
			if classes != "" {
				params["classes"] = splitCSV(classes)
			}
			if p != "" {
				params["path"] = p
				if out != "" {
					return callServerToFile("level.actors", params, out)
				}
				return callServer("level.actors", params)
			}
			if out == "" {
				return fmt.Errorf("--dir requires --out=<directory>")
			}
			return levelActorsBatch(dir, out, params)
		},
	}
	cmd.Flags().StringVar(&p, "path", "", "Level package path")
	cmd.Flags().StringVar(&dir, "dir", "", "Folder of maps to walk (batch mode)")
	cmd.Flags().StringVar(&out, "out", "", "Output file (--path) or directory (--dir)")
	cmd.Flags().StringVar(&classes, "class", "", "Comma-separated class filter")
	cmd.Flags().BoolVar(&boundsOnly, "bounds-only", false, "Omit component detail")
	cmd.Flags().BoolVar(&instances, "instances", false, "Include per-instance transforms for ISM/HISM")
	return cmd
}

func splitCSV(s string) []string {
	var out []string
	for _, part := range strings.Split(s, ",") {
		if t := strings.TrimSpace(part); t != "" {
			out = append(out, t)
		}
	}
	return out
}

// levelActorsBatch walks every map under dir (via level.list) and writes one
// JSON per map into outDir. Per-map failures are reported and skipped.
func levelActorsBatch(dir, outDir string, base map[string]interface{}) error {
	if err := server.EnsureRunning(cfg); err != nil {
		return err
	}
	raw, err := rpc.Call(cfg.PipeName, "level.list", map[string]interface{}{"dir": dir})
	if err != nil {
		return err
	}
	var list struct {
		Success bool     `json:"success"`
		Error   string   `json:"error"`
		Maps    []string `json:"maps"`
	}
	if err := json.Unmarshal(raw, &list); err != nil {
		return fmt.Errorf("level.list: %w", err)
	}
	if !list.Success {
		return fmt.Errorf("level.list: %s", list.Error)
	}
	resolved := normalizeOutPath(outDir)
	if abs, err := filepath.Abs(resolved); err == nil {
		resolved = abs
	}
	if err := os.MkdirAll(resolved, 0755); err != nil {
		return err
	}
	failed := 0
	for i, m := range list.Maps {
		params := map[string]interface{}{}
		for k, v := range base {
			params[k] = v
		}
		params["path"] = m
		params["unload"] = true
		name := path.Base(m) + ".json"
		res, err := rpc.Call(cfg.PipeName, "level.actors", params)
		if err != nil {
			failed++
			fmt.Fprintf(os.Stderr, "[%d/%d] %s: %v\n", i+1, len(list.Maps), m, err)
			continue
		}
		payload := []byte(res)
		if flagPretty {
			var v interface{}
			if json.Unmarshal(res, &v) == nil {
				if pp, err := json.MarshalIndent(v, "", "  "); err == nil {
					payload = pp
				}
			}
		}
		if err := os.WriteFile(filepath.Join(resolved, name), payload, 0644); err != nil {
			return err
		}
		fmt.Printf("[%d/%d] %s -> %s (%d bytes)\n", i+1, len(list.Maps), m, name, len(payload))
	}
	fmt.Printf("Wrote %d/%d maps to %s (%d failed)\n", len(list.Maps)-failed, len(list.Maps), resolved, failed)
	if failed > 0 {
		return fmt.Errorf("%d map(s) failed", failed)
	}
	return nil
}

func levelLandscapeCmd() *cobra.Command {
	var (
		p, out, csvPath, points string
		grid                    int
		layers                  bool
	)
	cmd := &cobra.Command{
		Use:   "landscape",
		Short: "Sample landscape height / normal / slope / layer weights on a grid or at world points",
		Long: `Loads the landscape level, rebuilds ULandscapeInfo, and samples heightmap and
weightmap source data.

--grid=N       N x N vertex grid over the landscape extent (N <= 1024).
--points=x,y;… world XY points -> nearest landscape vertex (weights always on).
--layers       per-layer weights + dominant_layer in grid mode (cost scales
               with layer count).
--csv=file     also write samples as CSV:
               qx,qy,wx,wy,wz,slope_deg,dominant,<one column per layer>

Height: world z of the vertex. normal/slope_deg from central differences over
+/-1 quad. height_raw is the uint16 heightmap value (z_local = (raw-32768)/128).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			if (grid > 0) == (points != "") {
				return fmt.Errorf("exactly one of --grid or --points is required")
			}
			params := map[string]interface{}{"path": p, "layers": layers}
			if grid > 0 {
				params["grid"] = grid
			} else {
				pts, err := parsePoints(points)
				if err != nil {
					return err
				}
				params["points"] = pts
			}
			if csvPath == "" {
				if out != "" {
					return callServerToFile("level.landscape", params, out)
				}
				return callServer("level.landscape", params)
			}
			if err := server.EnsureRunning(cfg); err != nil {
				return err
			}
			res, err := rpc.Call(cfg.PipeName, "level.landscape", params)
			if err != nil {
				return err
			}
			csvResolved := normalizeOutPath(csvPath)
			f, err := os.Create(csvResolved)
			if err != nil {
				return err
			}
			defer f.Close()
			if err := writeLandscapeCSV(f, res); err != nil {
				return err
			}
			fmt.Printf("Wrote CSV to %s\n", csvResolved)
			if out != "" {
				outResolved := normalizeOutPath(out)
				if err := os.WriteFile(outResolved, res, 0644); err != nil {
					return err
				}
				fmt.Printf("Wrote %d bytes to %s\n", len(res), outResolved)
			}
			return nil
		},
	}
	cmd.Flags().StringVar(&p, "path", "", "Landscape level package, e.g. /Game/Showdown/Maps/Landscape/Sunrise_Landscape (required)")
	cmd.Flags().IntVar(&grid, "grid", 0, "Grid resolution N (N x N samples, max 1024)")
	cmd.Flags().StringVar(&points, "points", "", "World XY points: x,y;x,y")
	cmd.Flags().BoolVar(&layers, "layers", false, "Include per-layer weights (grid mode)")
	cmd.Flags().StringVar(&out, "out", "", "Write JSON to file")
	cmd.Flags().StringVar(&csvPath, "csv", "", "Also write samples as CSV")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

// parsePoints parses "x,y;x,y" into pairs.
func parsePoints(s string) ([][2]float64, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return nil, fmt.Errorf("--points is empty")
	}
	var out [][2]float64
	for _, pair := range strings.Split(s, ";") {
		xy := strings.Split(strings.TrimSpace(pair), ",")
		if len(xy) != 2 {
			return nil, fmt.Errorf("bad point %q (want x,y)", pair)
		}
		x, err := strconv.ParseFloat(strings.TrimSpace(xy[0]), 64)
		if err != nil {
			return nil, fmt.Errorf("bad x in %q: %w", pair, err)
		}
		y, err := strconv.ParseFloat(strings.TrimSpace(xy[1]), 64)
		if err != nil {
			return nil, fmt.Errorf("bad y in %q: %w", pair, err)
		}
		out = append(out, [2]float64{x, y})
	}
	return out, nil
}

// writeLandscapeCSV flattens level.landscape samples (first landscape) to CSV.
func writeLandscapeCSV(w io.Writer, result []byte) error {
	var r struct {
		Success    bool   `json:"success"`
		Error      string `json:"error"`
		Landscapes []struct {
			Samples struct {
				Points []struct {
					Qx       int                `json:"qx"`
					Qy       int                `json:"qy"`
					World    map[string]float64 `json:"world"`
					Slope    float64            `json:"slope_deg"`
					Dominant string             `json:"dominant_layer"`
					Weights  map[string]float64 `json:"weights"`
				} `json:"points"`
			} `json:"samples"`
		} `json:"landscapes"`
	}
	if err := json.Unmarshal(result, &r); err != nil {
		return err
	}
	if !r.Success {
		return fmt.Errorf("level.landscape: %s", r.Error)
	}
	if len(r.Landscapes) == 0 {
		return fmt.Errorf("no landscapes in result")
	}
	pts := r.Landscapes[0].Samples.Points
	layerSet := map[string]bool{}
	for _, p := range pts {
		for k := range p.Weights {
			layerSet[k] = true
		}
	}
	layers := make([]string, 0, len(layerSet))
	for k := range layerSet {
		layers = append(layers, k)
	}
	sort.Strings(layers)
	header := "qx,qy,wx,wy,wz,slope_deg,dominant"
	if len(layers) > 0 {
		header += "," + strings.Join(layers, ",")
	}
	if _, err := fmt.Fprintln(w, header); err != nil {
		return err
	}
	for _, p := range pts {
		fields := []string{
			strconv.Itoa(p.Qx), strconv.Itoa(p.Qy),
			ff(p.World["x"]), ff(p.World["y"]), ff(p.World["z"]),
			ff(p.Slope), p.Dominant,
		}
		for _, l := range layers {
			fields = append(fields, ff(p.Weights[l]))
		}
		if _, err := fmt.Fprintln(w, strings.Join(fields, ",")); err != nil {
			return err
		}
	}
	return nil
}

func ff(v float64) string { return strconv.FormatFloat(v, 'f', -1, 64) }
