package main

import (
	"encoding/json"
	"fmt"
	"os"

	"github.com/spf13/cobra"
)

// datatableCmd groups read-only DataTable inspection under `digbp datatable`.
//
// All subcommands round-trip via the server's `datatable.{schema,rows,get,dump}`
// JSON-RPC methods. Row values are stringified UE text-format (the same format
// `digbp edit cdo get` returns), so nested structs/arrays/object refs are
// recognisable to callers that already parse cdo-get output.
func datatableCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "datatable",
		Short: "Read DataTable schema, row keys, individual rows, or full dumps",
		Long: `Inspect UDataTable assets. Use 'schema' to discover the RowStruct + columns,
'rows' to enumerate row keys, 'get' for a single row by name, or 'dump' for
the whole table. Output JSON; row values are stringified UE text-format.

Use --out=file.json on 'dump' for large tables to avoid blowing past stdout
buffering.`,
	}
	cmd.AddCommand(
		datatableSchemaCmd(),
		datatableRowsCmd(),
		datatableGetCmd(),
		datatableDumpCmd(),
		datatableSetCmd(),
		datatableSetManyCmd(),
		datatableSaveCmd(),
		datatableRewritePathsCmd(),
	)
	return cmd
}

func datatableSchemaCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "schema",
		Short: "Report the RowStruct + column list for a DataTable",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("datatable.schema", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required, e.g. /Game/Data/MyTable)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func datatableRowsCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "rows",
		Short: "List row keys for a DataTable",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("datatable.rows", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func datatableGetCmd() *cobra.Command {
	var (
		path string
		row  string
	)
	cmd := &cobra.Command{
		Use:   "get",
		Short: "Read a single row by name from a DataTable",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("datatable.get", map[string]interface{}{
				"path": path,
				"row":  row,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required)")
	cmd.Flags().StringVar(&row, "row", "", "Row key/name (required, e.g. Row_1, bnd_pass01_01)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("row")
	return cmd
}

func datatableDumpCmd() *cobra.Command {
	var (
		path string
		out  string
	)
	cmd := &cobra.Command{
		Use:   "dump",
		Short: "Dump all rows from a DataTable",
		Long: `Returns { rows: { row_key: { col: value, ... }, ... } }. For large tables
prefer --out=file.json over stdout.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": path}
			if out != "" {
				return callServerToFile("datatable.dump", params, out)
			}
			return callServer("datatable.dump", params)
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required)")
	cmd.Flags().StringVar(&out, "out", "", "Write output to file instead of stdout")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func datatableSetCmd() *cobra.Command {
	var (
		path   string
		row    string
		column string
		value  string
	)
	cmd := &cobra.Command{
		Use:   "set",
		Short: "Set a single cell in a DataTable row",
		Long: `Mutates the row in memory and marks the package dirty. Follow up with
'datatable save' to persist. SCC (p4 edit / git) is the caller's job.

--value uses the same UE text-format that 'datatable get' returns, so
round-tripping a get output works:
  bool:            True / False
  number:          24
  FName:           MyName     (or "Quoted Name" — quotes are optional)
  FString/FText:   "some text"
  FVector:         (X=1,Y=2,Z=3)
  FLinearColor:    (R=1,G=0.8,B=0.4,A=1)
  Soft object ref: /Game/Path/Asset.Asset
  Array:           (a,b,c)    — empty array: ()
  Map:             ((k=v),(k2=v2))

Empty arrays via "()" are explicitly normalized to zero-element on the
server side; ImportText would otherwise produce a single-empty-element
array for FArrayProperty<FName>.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("datatable.set", map[string]interface{}{
				"path":   path,
				"row":    row,
				"column": column,
				"value":  value,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required)")
	cmd.Flags().StringVar(&row, "row", "", "Row key (required)")
	cmd.Flags().StringVar(&column, "column", "", "Column name (required)")
	cmd.Flags().StringVar(&value, "value", "", "UE text-format value (required, may be empty string)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("row")
	_ = cmd.MarkFlagRequired("column")
	return cmd
}

func datatableSetManyCmd() *cobra.Command {
	var (
		path     string
		jsonPath string
	)
	cmd := &cobra.Command{
		Use:   "set-many",
		Short: "Batch-set many cells from a JSON file",
		Long: `Reads a JSON file of shape:
  { "rows": { "<row_key>": { "<column>": "<value>", ... }, ... } }
and applies every (row, column) update. Targets are pre-validated against
the row map and row-struct schema before any write happens — so a typo in
a row key or column name aborts cleanly. ImportText failures mid-batch
are reported with failed_row + failed_column; writes that already landed
before the failure stay landed.

Marks the package dirty once at the end. Follow with 'datatable save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			data, err := os.ReadFile(jsonPath)
			if err != nil {
				return fmt.Errorf("read --json %s: %w", jsonPath, err)
			}
			var parsed struct {
				Rows map[string]map[string]interface{} `json:"rows"`
			}
			if err := json.Unmarshal(data, &parsed); err != nil {
				return fmt.Errorf("parse --json %s: %w", jsonPath, err)
			}
			if parsed.Rows == nil {
				return fmt.Errorf("--json file missing top-level 'rows' object")
			}
			return callServer("datatable.set_many", map[string]interface{}{
				"path": path,
				"rows": parsed.Rows,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required)")
	cmd.Flags().StringVar(&jsonPath, "json", "", "Path to JSON file with updates (required)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("json")
	return cmd
}

func datatableSaveCmd() *cobra.Command {
	var path string
	cmd := &cobra.Command{
		Use:   "save",
		Short: "Persist a dirty DataTable's package to disk",
		Long: `Same semantics as 'edit save' — bOnlyDirty=true. Returns
{saved: false, not_dirty: true} as a distinct success if the package is
already clean. SCC checkout (p4 edit / git) is the caller's job.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("datatable.save", map[string]interface{}{"path": path})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required)")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func datatableRewritePathsCmd() *cobra.Command {
	var (
		path string
		from string
		to   string
	)
	cmd := &cobra.Command{
		Use:   "rewrite-paths",
		Short: "Substring-rewrite all FSoftObjectProperty/FSoftClassProperty paths in a DataTable",
		Long: `Walks every row, for every FSoftObjectProperty/FSoftClassProperty column,
does a substring replace From -> To on the stored asset path. Useful for
asset-move cleanup (e.g. /Game/Showdown/NFT/... -> /Game/Showdown/Characters/...
in one call instead of N 'datatable set' invocations).

Returns { rewritten_count, rows_touched, changes: [{row, column, from, to}] }.
Marks dirty if any rewrite occurred. Follow with 'datatable save'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("datatable.rewrite_paths", map[string]interface{}{
				"path": path,
				"from": from,
				"to":   to,
			})
		},
	}
	cmd.Flags().StringVar(&path, "path", "", "DataTable asset path (required)")
	cmd.Flags().StringVar(&from, "from", "", "Substring to find (required)")
	cmd.Flags().StringVar(&to, "to", "", "Substring to replace with (required, may be empty)")
	_ = cmd.MarkFlagRequired("path")
	_ = cmd.MarkFlagRequired("from")
	return cmd
}
