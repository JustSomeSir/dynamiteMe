// gamefiles copies named files out of the game's archives so their structure can be studied:
// <game dir>/master/**/*.dat -> the file -> (for .fpk/.fpkd) its entries -> (for .fox2) XML.
//
//	go run ./tools/gamefiles -game E:\coop-mgsv\game -out E:\coop-mgsv\build\gamefiles \
//	    /Assets/tpp/pack/buddy/horse/buddy_horse2_common.fpkd
//
// Reads the game copy only. The output is game data: keep it out of every repository and PR.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/unknown321/datfpk/fox2"
	"github.com/unknown321/datfpk/fpk"
	"github.com/unknown321/datfpk/qar"
	"github.com/unknown321/hashing"
)

func fox2ToXML(in string) error {
	input, err := os.Open(in)
	if err != nil {
		return err
	}
	defer input.Close()
	f := &fox2.Fox2{}
	if err = f.Read(input); err != nil {
		return err
	}
	out, err := os.Create(in + ".xml")
	if err != nil {
		return err
	}
	defer out.Close()
	return f.ToXML(out)
}

func unpackFpk(file string) error {
	f := fpk.Fpk{}
	if err := f.ReadFrom(file, false); err != nil {
		return fmt.Errorf("read fpk: %w", err)
	}
	defer f.Close()
	dir := file + "_dir"
	for _, e := range f.Entries {
		name := e.FilePath.Data
		target := filepath.Join(dir, filepath.FromSlash(name))
		if !strings.HasPrefix(filepath.Clean(target), filepath.Clean(dir)+string(os.PathSeparator)) {
			return fmt.Errorf("entry path escapes the output folder: %q", name)
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		out, err := os.Create(target)
		if err != nil {
			return err
		}
		err = f.ExtractTo(name, out)
		out.Close()
		if err != nil {
			return fmt.Errorf("extract %s: %w", name, err)
		}
		fmt.Printf("    %s\n", name)
		if strings.HasSuffix(name, ".fox2") {
			if err = fox2ToXML(target); err != nil {
				fmt.Printf("    (fox2 -> xml failed for %s: %v)\n", name, err)
			}
		}
	}
	return nil
}

func main() {
	game := flag.String("game", "", "game copy folder (the one with mgsvtpp.exe)")
	outDir := flag.String("out", "", "output folder")
	flag.Parse()
	if *game == "" || *outDir == "" || flag.NArg() == 0 {
		fmt.Println("usage: gamefiles -game <dir> -out <dir> /Assets/path/file.ext ...")
		os.Exit(2)
	}
	var dats []string
	for _, pattern := range []string{"master/*.dat", "master/*/*.dat"} {
		m, _ := filepath.Glob(filepath.Join(*game, filepath.FromSlash(pattern)))
		dats = append(dats, m...)
	}
	sort.Strings(dats)
	if len(dats) == 0 {
		fmt.Println("no .dat archives under", filepath.Join(*game, "master"))
		os.Exit(1)
	}
	found := map[string]int{}
	for _, dat := range dats {
		q := qar.Qar{}
		if err := q.ReadFrom(dat); err != nil {
			fmt.Printf("skip %s: %v\n", dat, err)
			continue
		}
		byHash := map[uint64]bool{}
		for _, e := range q.Entries {
			byHash[e.Header.PathHash] = true
		}
		rel, _ := filepath.Rel(*game, dat)
		for _, path := range flag.Args() {
			hash := hashing.HashFileNameWithExtension(path)
			if !byHash[hash] {
				continue
			}
			// one folder per archive: the same file can exist in a base archive and in a patch
			dest := filepath.Join(*outDir, strings.NewReplacer(string(os.PathSeparator), "_", ".", "_").Replace(rel))
			n, err := q.Extract(path, hash, dest)
			if err != nil {
				fmt.Printf("%s: %s: extract failed: %v\n", rel, path, err)
				continue
			}
			found[path]++
			file := filepath.Join(dest, filepath.FromSlash(path))
			fmt.Printf("%s: %s (%d bytes) -> %s\n", rel, path, n, file)
			if strings.HasSuffix(path, ".fpk") || strings.HasSuffix(path, ".fpkd") {
				if err = unpackFpk(file); err != nil {
					fmt.Printf("  unpack failed: %v\n", err)
				}
			}
		}
		q.Close()
	}
	missing := 0
	for _, path := range flag.Args() {
		if found[path] == 0 {
			fmt.Println("NOT FOUND in any archive:", path)
			missing++
		}
	}
	fmt.Printf("done: %d archives, %d of %d files found\n", len(dats), flag.NArg()-missing, flag.NArg())
	if missing > 0 {
		os.Exit(1)
	}
}
