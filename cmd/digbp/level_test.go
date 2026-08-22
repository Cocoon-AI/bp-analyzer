package main

import (
	"bytes"
	"strings"
	"testing"
)

func TestParsePoints(t *testing.T) {
	pts, err := parsePoints("1,2;3.5,-4")
	if err != nil {
		t.Fatal(err)
	}
	if len(pts) != 2 || pts[0] != [2]float64{1, 2} || pts[1] != [2]float64{3.5, -4} {
		t.Fatalf("got %v", pts)
	}
	if _, err := parsePoints("1,2;3"); err == nil {
		t.Fatal("expected error on malformed pair")
	}
	if _, err := parsePoints(""); err == nil {
		t.Fatal("expected error on empty")
	}
}

func TestWriteLandscapeCSV(t *testing.T) {
	in := []byte(`{"success":true,"landscapes":[{"samples":{"points":[
		{"qx":0,"qy":0,"world":{"x":1,"y":2,"z":3},"slope_deg":4.5,"dominant_layer":"Grass","weights":{"Grass":0.75,"Soil":0.25}},
		{"qx":1,"qy":0,"world":{"x":5,"y":2,"z":6},"slope_deg":0,"dominant_layer":"Soil","weights":{"Grass":0.1,"Soil":0.9}}
	]}}]}`)
	var buf bytes.Buffer
	if err := writeLandscapeCSV(&buf, in); err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(buf.String()), "\n")
	if lines[0] != "qx,qy,wx,wy,wz,slope_deg,dominant,Grass,Soil" {
		t.Fatalf("header: %q", lines[0])
	}
	if lines[1] != "0,0,1,2,3,4.5,Grass,0.75,0.25" {
		t.Fatalf("row1: %q", lines[1])
	}
	if len(lines) != 3 {
		t.Fatalf("rows: %d", len(lines))
	}
}

func TestWriteLandscapeCSVError(t *testing.T) {
	var buf bytes.Buffer
	if err := writeLandscapeCSV(&buf, []byte(`{"success":false,"error":"nope"}`)); err == nil {
		t.Fatal("expected error envelope to surface")
	}
}
