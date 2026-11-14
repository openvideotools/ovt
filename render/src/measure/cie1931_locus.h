// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.7 (WI-4.454): the spectral locus of the CIE 1931 2° standard colorimetric observer for the CIE xy
// scope overlay, 380-700 nm in 5 nm steps (65 points).
//
// Attribution: data from colour-science (https://www.colour-science.org), Copyright 2013 Colour Developers,
// BSD-3-Clause; listed in tools/derived_files.toml, so THIRD_PARTY_NOTICES.md carries it.
//
// Provenance: chromaticities x = X / (X + Y + Z), y = Y / (X + Y + Z), rounded to 6 decimals, of the CIE 1931 2°
// colour-matching functions of ISO/CIE 11664-1 (CIE S 014-1) as tabulated at 5 nm by the CVRL "Older CIE standards"
// page and redistributed in the colour-science package 0.4.7 (colour/colorimetry/datasets/cmfs.py, BSD-3-Clause).
// The values are the standard's published measurement data, not creative content; the table below is computed from
// them, not copied text (SPEC-ASSUMPTION(#512) item 10). The overlay is drawn by the GUI (chapter 11); libovt-render
// only carries the table.

#ifndef OVT_RENDER_SRC_MEASURE_CIE1931_LOCUS_H
#define OVT_RENDER_SRC_MEASURE_CIE1931_LOCUS_H

#include <array>

namespace ovt::render::detail {

struct LocusPoint {
  int nm;   // wavelength in nanometres
  float x;  // CIE 1931 chromaticity
  float y;
};

inline constexpr std::array<LocusPoint, 65> kCie1931Locus = {{
    {380, 0.174112F, 0.004964F}, {385, 0.174008F, 0.004981F}, {390, 0.173801F, 0.004915F}, {395, 0.173560F, 0.004923F},
    {400, 0.173337F, 0.004797F}, {405, 0.173021F, 0.004775F}, {410, 0.172577F, 0.004799F}, {415, 0.172087F, 0.004833F},
    {420, 0.171407F, 0.005102F}, {425, 0.170301F, 0.005789F}, {430, 0.168878F, 0.006900F}, {435, 0.166895F, 0.008556F},
    {440, 0.164412F, 0.010858F}, {445, 0.161105F, 0.013793F}, {450, 0.156641F, 0.017705F}, {455, 0.150985F, 0.022740F},
    {460, 0.143960F, 0.029703F}, {465, 0.135503F, 0.039879F}, {470, 0.124118F, 0.057803F}, {475, 0.109594F, 0.086843F},
    {480, 0.091294F, 0.132702F}, {485, 0.068706F, 0.200723F}, {490, 0.045391F, 0.294976F}, {495, 0.023460F, 0.412703F},
    {500, 0.008168F, 0.538423F}, {505, 0.003859F, 0.654823F}, {510, 0.013870F, 0.750186F}, {515, 0.038852F, 0.812016F},
    {520, 0.074302F, 0.833803F}, {525, 0.114161F, 0.826207F}, {530, 0.154722F, 0.805864F}, {535, 0.192876F, 0.781629F},
    {540, 0.229620F, 0.754329F}, {545, 0.265775F, 0.724324F}, {550, 0.301604F, 0.692308F}, {555, 0.337363F, 0.658848F},
    {560, 0.373102F, 0.624451F}, {565, 0.408736F, 0.589607F}, {570, 0.444062F, 0.554714F}, {575, 0.478775F, 0.520202F},
    {580, 0.512486F, 0.486591F}, {585, 0.544787F, 0.454434F}, {590, 0.575151F, 0.424232F}, {595, 0.602933F, 0.396497F},
    {600, 0.627037F, 0.372491F}, {605, 0.648233F, 0.351395F}, {610, 0.665764F, 0.334011F}, {615, 0.680079F, 0.319747F},
    {620, 0.691504F, 0.308342F}, {625, 0.700606F, 0.299301F}, {630, 0.707918F, 0.292027F}, {635, 0.714032F, 0.285929F},
    {640, 0.719033F, 0.280935F}, {645, 0.723032F, 0.276948F}, {650, 0.725992F, 0.274008F}, {655, 0.728272F, 0.271728F},
    {660, 0.729969F, 0.270031F}, {665, 0.731089F, 0.268911F}, {670, 0.731993F, 0.268007F}, {675, 0.732719F, 0.267281F},
    {680, 0.733417F, 0.266583F}, {685, 0.734047F, 0.265953F}, {690, 0.734390F, 0.265610F}, {695, 0.734592F, 0.265408F},
    {700, 0.734690F, 0.265310F},
}};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_MEASURE_CIE1931_LOCUS_H
