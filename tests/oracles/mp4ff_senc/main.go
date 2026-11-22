// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Dumps the per-sample IV and subsample list of the moov-level `senc` of every track, with mp4ff (the oracle of
// WI-1.221 AT1). Usage: go run ./tests/oracles/mp4ff_senc <file.mp4> (from inside tests/oracles/mp4ff_senc: go run . <file.mp4>). Pinned to the mp4ff version of tests/golden/isobmff/ORACLES.json.
package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"

	"github.com/Eyevinn/mp4ff/mp4"
)

type sub struct {
	Clear     uint16 `json:"clear"`
	Protected uint32 `json:"protected"`
}
type sample struct {
	IV         string `json:"iv"`
	Subsamples []sub  `json:"subsamples"`
}
type track struct {
	TrackID uint32   `json:"track_id"`
	IVSize  int      `json:"per_sample_iv_size"`
	KID     string   `json:"default_kid"`
	Samples []sample `json:"samples"`
}

func main() {
	f, err := os.Open(os.Args[1])
	if err != nil {
		panic(err)
	}
	defer f.Close()
	parsed, err := mp4.DecodeFile(f, mp4.WithDecodeMode(mp4.DecModeLazyMdat))
	if err != nil {
		panic(err)
	}
	var out []track
	for _, trak := range parsed.Moov.Traks {
		t := track{TrackID: trak.Tkhd.TrackID}
		stsd := trak.Mdia.Minf.Stbl.Stsd
		for _, c := range stsd.Children {
			if v, ok := c.(*mp4.VisualSampleEntryBox); ok && v.Sinf != nil && v.Sinf.Schi != nil && v.Sinf.Schi.Tenc != nil {
				t.IVSize = int(v.Sinf.Schi.Tenc.DefaultPerSampleIVSize)
				t.KID = hex.EncodeToString(v.Sinf.Schi.Tenc.DefaultKID)
			}
		}
		for _, c := range trak.Children {
			if senc, ok := c.(*mp4.SencBox); ok {
				if err := senc.ParseReadBox(byte(t.IVSize), trak.Mdia.Minf.Stbl.Saiz); err != nil {
					panic(err)
				}
				for i := range senc.IVs {
					s := sample{IV: hex.EncodeToString(senc.IVs[i]), Subsamples: []sub{}}
					if i < len(senc.SubSamples) {
						for _, p := range senc.SubSamples[i] {
							s.Subsamples = append(s.Subsamples, sub{p.BytesOfClearData, p.BytesOfProtectedData})
						}
					}
					t.Samples = append(t.Samples, s)
				}
			}
		}
		out = append(out, t)
	}
	b, _ := json.MarshalIndent(map[string]any{"oracle": "mp4ff v0.48.0 senc", "tracks": out}, "", " ")
	fmt.Println(string(b))
}
