#version 450
// The frame's way to the window with sharpening (sharpen.cpp): FidelityFX
// Super Resolution 1.0's RCAS (robust contrast adaptive sharpening), FsrRcasF
// of https://github.com/GPUOpen-Effects/FidelityFX-FSR (ffx-fsr/ffx_fsr1.h),
// written out without its portability macros. Its five taps are a window
// pixel apart and read through the linear sampler, so over a frame smaller
// than its place in the window it sharpens the linear picture; where the two
// are one size the taps are the texels themselves, as published.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

precision mediump float;
precision highp int;

layout(push_constant) uniform Push
{
    highp vec4 place;    // the frame's left and top in the window, and one over its width and height there
    highp vec4 params;   // the sharpening, 0 to 1; the rest unused
} push;
layout(set = 0, binding = 0) uniform mediump sampler2D frame;
layout(location = 0) out vec4 colour;

// This is set at the limit of providing unnatural results for sharpening.
const float kLimit = 0.25 - (1.0 / 16.0);

void main()
{
    //    b
    //  d e f
    //    h
    highp vec2 pixel = push.place.zw;
    highp vec2 at = (gl_FragCoord.xy - push.place.xy) * pixel;
    vec3 b = textureLod(frame, at + vec2(0.0, -pixel.y), 0.0).rgb;
    vec3 d = textureLod(frame, at + vec2(-pixel.x, 0.0), 0.0).rgb;
    vec3 e = textureLod(frame, at, 0.0).rgb;
    vec3 f = textureLod(frame, at + vec2(pixel.x, 0.0), 0.0).rgb;
    vec3 h = textureLod(frame, at + vec2(0.0, pixel.y), 0.0).rgb;

    // Min and max of ring.
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    // Limiters.
    vec3 hitMin = min(mn4, e) / (4.0 * mx4);
    vec3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0);
    vec3 lobeRGB = max(-hitMin, hitMax);
    // The published sharpness is in stops of reduction, 0 the most: none at
    // the player's most, two towards the least.
    float lobe = max(-kLimit, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * exp2(-2.0 * (1.0 - push.params.x));
    colour = vec4((lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0), 1.0);
}
