// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PictureHost passthrough (WI-1.405): the surface's texture is already sRGB-encoded RGBA8 at the item's pixel size,
// so the fragment copies one texel unchanged.
#version 440

layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D picture;

void main() {
    fragColor = texture(picture, v_texcoord);
}
