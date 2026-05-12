package main

import (
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
