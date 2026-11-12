package main

// Compiles the D-Horse game object definition with two TppHorse2 instances (one per player) to fox2.
// The installer puts it into /Assets/tpp/pack/buddy/horse/buddy_horse2_00.fpkd (moddata.ReplaceInFPK).

import (
	"bytes"
	"log/slog"
	"os"

	"github.com/unknown321/datfpk/fox2"
	"github.com/unknown321/datfpk/util"
)

func main() {
	in := "moddata/buddy_horse/buddy_horse2_00.fox2.xml"
	out := "moddata/buddy_horse/buddy_horse2_00.fox2"
	data, err := os.ReadFile(in)
	if err != nil {
		slog.Error("cannot read", "name", in, "error", err.Error())
		os.Exit(1)
	}
	fox := fox2.Fox2{}
	if err = fox.FromXML(bytes.NewReader(data)); err != nil {
		slog.Error("xml", "error", err.Error(), "name", in)
		os.Exit(1)
	}
	buf := util.NewByteArrayReaderWriter([]byte{})
	if err = fox.Write(buf); err != nil {
		slog.Error("write", "error", err.Error(), "name", in)
		os.Exit(1)
	}
	if err = os.WriteFile(out, buf.Bytes(), 0644); err != nil {
		slog.Error("write", "error", err.Error(), "name", out)
		os.Exit(1)
	}
	slog.Info("generated", "file", out)
}
