#pragma once

/**
 * Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
 * Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
 * Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
 * Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
 * Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to
 * do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software. As clarification, there
 * is no requirement that the copyright notice and permission be included in
 * binary distributions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// SMAA 1x ported to WGSL from smaa_shader.h with d3d12.cpp's configuration: luma edges with local contrast adaptation,
// threshold 0.05, 16 search steps, 8 diagonal steps, corner rounding 25, no predication. Each pass draws a fullscreen
// triangle of three vertices. Every sample is textureSampleLevel at level 0, so loops and discard pass uniformity.
//
// One bind group layout fits all three passes, group 0:
//   0 uniform vec4<f32> metrics {1 / w, 1 / h, w, h}
//   1 linear clamp sampler
//   2 point clamp sampler
//   3 texture_2d<f32>: edges and blend read the colour (rgba8unorm), weights read the edges
//   4 texture_2d<f32>: weights read the area texture (rg8unorm 160 x 560), blend reads the weights; edges ignores it
//   5 texture_2d<f32>: weights read the search texture (r8unorm 64 x 16); edges and blend ignore it
// Targets: edges rg8unorm cleared to zero each frame (the pass discards), weights rgba8unorm cleared, blend the frame's.

namespace gpu::smaa {

inline constexpr const char* kEdgesWgsl = R"wgsl(
@group(0) @binding(0) var<uniform> metrics: vec4<f32>;
@group(0) @binding(1) var linear_sampler: sampler;
@group(0) @binding(2) var point_sampler: sampler;
@group(0) @binding(3) var color_tex: texture_2d<f32>;
@group(0) @binding(4) var unused_tex: texture_2d<f32>;
@group(0) @binding(5) var unused_search_tex: texture_2d<f32>;

const kThreshold = 0.05;
const kLocalContrastAdaptationFactor = 2.0;
const kLumaWeights = vec3<f32>(0.2126, 0.7152, 0.0722);

struct EdgeVertex {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) offset0: vec4<f32>,
  @location(2) offset1: vec4<f32>,
  @location(3) offset2: vec4<f32>,
}

@vertex fn vs_main(@builtin(vertex_index) index: u32) -> EdgeVertex {
  let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
  var v: EdgeVertex;
  v.position = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
  v.uv = uv;
  v.offset0 = metrics.xyxy * vec4<f32>(-1.0, 0.0, 0.0, -1.0) + uv.xyxy;
  v.offset1 = metrics.xyxy * vec4<f32>(1.0, 0.0, 0.0, 1.0) + uv.xyxy;
  v.offset2 = metrics.xyxy * vec4<f32>(-2.0, 0.0, 0.0, -2.0) + uv.xyxy;
  return v;
}

fn luma(uv: vec2<f32>) -> f32 {
  return dot(textureSampleLevel(color_tex, point_sampler, uv, 0.0).rgb, kLumaWeights);
}

@fragment fn fs_main(v: EdgeVertex) -> @location(0) vec4<f32> {
  let threshold = vec2<f32>(kThreshold);
  let l = luma(v.uv);
  let l_left_top = vec2<f32>(luma(v.offset0.xy), luma(v.offset0.zw));
  let delta_left_top = abs(l - l_left_top);
  var edges = step(threshold, delta_left_top);
  if (dot(edges, vec2<f32>(1.0)) == 0.0) {
    discard;
  }
  let l_right_bottom = vec2<f32>(luma(v.offset1.xy), luma(v.offset1.zw));
  var max_delta = max(delta_left_top, abs(l - l_right_bottom));
  let l_far_left_top = vec2<f32>(luma(v.offset2.xy), luma(v.offset2.zw));
  max_delta = max(max_delta, abs(l_left_top - l_far_left_top));
  let final_delta = max(max_delta.x, max_delta.y);
  edges *= step(vec2<f32>(final_delta), kLocalContrastAdaptationFactor * delta_left_top);
  return vec4<f32>(edges, 0.0, 0.0);
}
)wgsl";

inline constexpr const char* kWeightsWgsl = R"wgsl(
@group(0) @binding(0) var<uniform> metrics: vec4<f32>;
@group(0) @binding(1) var linear_sampler: sampler;
@group(0) @binding(2) var point_sampler: sampler;
@group(0) @binding(3) var edges_tex: texture_2d<f32>;
@group(0) @binding(4) var area_tex: texture_2d<f32>;
@group(0) @binding(5) var search_tex: texture_2d<f32>;

const kMaxSearchSteps = 16.0;
const kMaxSearchStepsDiag = 8.0;
const kCornerRoundingNorm = 25.0 / 100.0;
const kAreaTexMaxDistance = 16.0;
const kAreaTexMaxDistanceDiag = 20.0;
const kAreaTexPixelSize = vec2<f32>(1.0 / 160.0, 1.0 / 560.0);
const kAreaTexSubtexSize = 1.0 / 7.0;
const kSearchTexSize = vec2<f32>(66.0, 33.0);
const kSearchTexPackedSize = vec2<f32>(64.0, 16.0);

struct WeightVertex {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) pixcoord: vec2<f32>,
  @location(2) offset0: vec4<f32>,
  @location(3) offset1: vec4<f32>,
  @location(4) offset2: vec4<f32>,
}

@vertex fn vs_main(@builtin(vertex_index) index: u32) -> WeightVertex {
  let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
  var v: WeightVertex;
  v.position = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
  v.uv = uv;
  v.pixcoord = uv * metrics.zw;
  v.offset0 = metrics.xyxy * vec4<f32>(-0.25, -0.125, 1.25, -0.125) + uv.xyxy;
  v.offset1 = metrics.xyxy * vec4<f32>(-0.125, -0.25, -0.125, 1.25) + uv.xyxy;
  v.offset2 = metrics.xxyy * (vec4<f32>(-2.0, 2.0, -2.0, 2.0) * kMaxSearchSteps)
    + vec4<f32>(v.offset0.xz, v.offset1.yw);
  return v;
}

fn edges_at(uv: vec2<f32>) -> vec2<f32> {
  return textureSampleLevel(edges_tex, linear_sampler, uv, 0.0).rg;
}

fn decode_diag_bilinear2(e: vec2<f32>) -> vec2<f32> {
  return round(vec2<f32>(e.r * abs(5.0 * e.r - 5.0 * 0.75), e.g));
}

fn decode_diag_bilinear4(e: vec4<f32>) -> vec4<f32> {
  let rb = e.xz * abs(5.0 * e.xz - 5.0 * 0.75);
  return round(vec4<f32>(rb.x, e.y, rb.y, e.w));
}

struct DiagSearch {
  coord: vec2<f32>,
  end: vec2<f32>,
}

fn search_diag1(uv: vec2<f32>, dir: vec2<f32>) -> DiagSearch {
  var coord = vec4<f32>(uv, -1.0, 1.0);
  let t = vec3<f32>(metrics.xy, 1.0);
  var e = vec2<f32>(0.0);
  while (coord.z < kMaxSearchStepsDiag - 1.0 && coord.w > 0.9) {
    coord = vec4<f32>(t * vec3<f32>(dir, 1.0) + coord.xyz, coord.w);
    e = edges_at(coord.xy);
    coord.w = dot(e, vec2<f32>(0.5));
  }
  return DiagSearch(coord.zw, e);
}

fn search_diag2(uv: vec2<f32>, dir: vec2<f32>) -> DiagSearch {
  var coord = vec4<f32>(uv, -1.0, 1.0);
  coord.x += 0.25 * metrics.x;
  let t = vec3<f32>(metrics.xy, 1.0);
  var e = vec2<f32>(0.0);
  while (coord.z < kMaxSearchStepsDiag - 1.0 && coord.w > 0.9) {
    coord = vec4<f32>(t * vec3<f32>(dir, 1.0) + coord.xyz, coord.w);
    e = decode_diag_bilinear2(edges_at(coord.xy));
    coord.w = dot(e, vec2<f32>(0.5));
  }
  return DiagSearch(coord.zw, e);
}

fn area_diag(dist: vec2<f32>, e: vec2<f32>, offset: f32) -> vec2<f32> {
  var uv = vec2<f32>(kAreaTexMaxDistanceDiag) * e + dist;
  uv = kAreaTexPixelSize * uv + 0.5 * kAreaTexPixelSize;
  uv.x += 0.5;
  uv.y += kAreaTexSubtexSize * offset;
  return textureSampleLevel(area_tex, linear_sampler, uv, 0.0).rg;
}

fn calculate_diag_weights(uv: vec2<f32>, e: vec2<f32>) -> vec2<f32> {
  var weights = vec2<f32>(0.0);
  var d = vec4<f32>(0.0);
  if (e.r > 0.0) {
    let s = search_diag1(uv, vec2<f32>(-1.0, 1.0));
    d.x = s.coord.x + f32(s.end.y > 0.9);
    d.z = s.coord.y;
  }
  let s = search_diag1(uv, vec2<f32>(1.0, -1.0));
  d.y = s.coord.x;
  d.w = s.coord.y;

  if (d.x + d.y > 2.0) {
    let coords = vec4<f32>(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * metrics.xyxy + uv.xyxy;
    let c = decode_diag_bilinear4(vec4<f32>(
      textureSampleLevel(edges_tex, linear_sampler, coords.xy, 0.0, vec2<i32>(-1, 0)).rg,
      textureSampleLevel(edges_tex, linear_sampler, coords.zw, 0.0, vec2<i32>(1, 0)).rg)).yxwz;
    var cc = vec2<f32>(2.0) * c.xz + c.yw;
    cc = select(cc, vec2<f32>(0.0), d.zw >= vec2<f32>(0.9));
    weights += area_diag(d.xy, cc, 0.0);
  }

  let s2 = search_diag2(uv, vec2<f32>(-1.0, -1.0));
  d.x = s2.coord.x;
  d.z = s2.coord.y;
  d.y = 0.0;
  d.w = 0.0;
  if (textureSampleLevel(edges_tex, linear_sampler, uv, 0.0, vec2<i32>(1, 0)).r > 0.0) {
    let s3 = search_diag2(uv, vec2<f32>(1.0, 1.0));
    d.y = s3.coord.x + f32(s3.end.y > 0.9);
    d.w = s3.coord.y;
  }

  if (d.x + d.y > 2.0) {
    let coords = vec4<f32>(-d.x, -d.x, d.y, d.y) * metrics.xyxy + uv.xyxy;
    let c = vec4<f32>(
      textureSampleLevel(edges_tex, linear_sampler, coords.xy, 0.0, vec2<i32>(-1, 0)).g,
      textureSampleLevel(edges_tex, linear_sampler, coords.xy, 0.0, vec2<i32>(0, -1)).r,
      textureSampleLevel(edges_tex, linear_sampler, coords.zw, 0.0, vec2<i32>(1, 0)).gr);
    var cc = vec2<f32>(2.0) * c.xz + c.yw;
    cc = select(cc, vec2<f32>(0.0), d.zw >= vec2<f32>(0.9));
    weights += area_diag(d.xy, cc, 0.0).gr;
  }
  return weights;
}

fn search_length(e: vec2<f32>, offset: f32) -> f32 {
  var scale = kSearchTexSize * vec2<f32>(0.5, -1.0);
  var bias = kSearchTexSize * vec2<f32>(offset, 1.0);
  scale += vec2<f32>(-1.0, 1.0);
  bias += vec2<f32>(0.5, -0.5);
  scale *= 1.0 / kSearchTexPackedSize;
  bias *= 1.0 / kSearchTexPackedSize;
  return textureSampleLevel(search_tex, linear_sampler, scale * e + bias, 0.0).r;
}

fn search_x_left(start: vec2<f32>, end: f32) -> f32 {
  var uv = start;
  var e = vec2<f32>(0.0, 1.0);
  while (uv.x > end && e.g > 0.8281 && e.r == 0.0) {
    e = edges_at(uv);
    uv = -vec2<f32>(2.0, 0.0) * metrics.xy + uv;
  }
  let offset = -(255.0 / 127.0) * search_length(e, 0.0) + 3.25;
  return metrics.x * offset + uv.x;
}

fn search_x_right(start: vec2<f32>, end: f32) -> f32 {
  var uv = start;
  var e = vec2<f32>(0.0, 1.0);
  while (uv.x < end && e.g > 0.8281 && e.r == 0.0) {
    e = edges_at(uv);
    uv = vec2<f32>(2.0, 0.0) * metrics.xy + uv;
  }
  let offset = -(255.0 / 127.0) * search_length(e, 0.5) + 3.25;
  return -metrics.x * offset + uv.x;
}

fn search_y_up(start: vec2<f32>, end: f32) -> f32 {
  var uv = start;
  var e = vec2<f32>(1.0, 0.0);
  while (uv.y > end && e.r > 0.8281 && e.g == 0.0) {
    e = edges_at(uv);
    uv = -vec2<f32>(0.0, 2.0) * metrics.xy + uv;
  }
  let offset = -(255.0 / 127.0) * search_length(e.gr, 0.0) + 3.25;
  return metrics.y * offset + uv.y;
}

fn search_y_down(start: vec2<f32>, end: f32) -> f32 {
  var uv = start;
  var e = vec2<f32>(1.0, 0.0);
  while (uv.y < end && e.r > 0.8281 && e.g == 0.0) {
    e = edges_at(uv);
    uv = vec2<f32>(0.0, 2.0) * metrics.xy + uv;
  }
  let offset = -(255.0 / 127.0) * search_length(e.gr, 0.5) + 3.25;
  return -metrics.y * offset + uv.y;
}

fn area(dist: vec2<f32>, e1: f32, e2: f32, offset: f32) -> vec2<f32> {
  var uv = vec2<f32>(kAreaTexMaxDistance) * round(4.0 * vec2<f32>(e1, e2)) + dist;
  uv = kAreaTexPixelSize * uv + 0.5 * kAreaTexPixelSize;
  uv.y = kAreaTexSubtexSize * offset + uv.y;
  return textureSampleLevel(area_tex, linear_sampler, uv, 0.0).rg;
}

fn detect_horizontal_corner_pattern(weights: vec2<f32>, uv: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
  let left_right = step(d.xy, d.yx);
  var rounding = (1.0 - kCornerRoundingNorm) * left_right;
  rounding /= left_right.x + left_right.y;
  var factor = vec2<f32>(1.0);
  factor.x -= rounding.x * textureSampleLevel(edges_tex, linear_sampler, uv.xy, 0.0, vec2<i32>(0, 1)).r;
  factor.x -= rounding.y * textureSampleLevel(edges_tex, linear_sampler, uv.zw, 0.0, vec2<i32>(1, 1)).r;
  factor.y -= rounding.x * textureSampleLevel(edges_tex, linear_sampler, uv.xy, 0.0, vec2<i32>(0, -2)).r;
  factor.y -= rounding.y * textureSampleLevel(edges_tex, linear_sampler, uv.zw, 0.0, vec2<i32>(1, -2)).r;
  return weights * saturate(factor);
}

fn detect_vertical_corner_pattern(weights: vec2<f32>, uv: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
  let left_right = step(d.xy, d.yx);
  var rounding = (1.0 - kCornerRoundingNorm) * left_right;
  rounding /= left_right.x + left_right.y;
  var factor = vec2<f32>(1.0);
  factor.x -= rounding.x * textureSampleLevel(edges_tex, linear_sampler, uv.xy, 0.0, vec2<i32>(1, 0)).g;
  factor.x -= rounding.y * textureSampleLevel(edges_tex, linear_sampler, uv.zw, 0.0, vec2<i32>(1, 1)).g;
  factor.y -= rounding.x * textureSampleLevel(edges_tex, linear_sampler, uv.xy, 0.0, vec2<i32>(-2, 0)).g;
  factor.y -= rounding.y * textureSampleLevel(edges_tex, linear_sampler, uv.zw, 0.0, vec2<i32>(-2, 1)).g;
  return weights * saturate(factor);
}

@fragment fn fs_main(v: WeightVertex) -> @location(0) vec4<f32> {
  var weights = vec4<f32>(0.0);
  var e = edges_at(v.uv);

  if (e.g > 0.0) {
    let diag = calculate_diag_weights(v.uv, e);
    weights = vec4<f32>(diag, weights.zw);
    if (diag.r == -diag.g) {
      var coords = vec3<f32>(search_x_left(v.offset0.xy, v.offset2.x), v.offset1.y, 0.0);
      let e1 = edges_at(coords.xy).r;
      coords.z = search_x_right(v.offset0.zw, v.offset2.y);
      let d = abs(round(metrics.zz * vec2<f32>(coords.x, coords.z) - v.pixcoord.xx));
      let sqrt_d = sqrt(d);
      let e2 = textureSampleLevel(edges_tex, linear_sampler, coords.zy, 0.0, vec2<i32>(1, 0)).r;
      coords.y = v.uv.y;
      let rg = detect_horizontal_corner_pattern(area(sqrt_d, e1, e2, 0.0), coords.xyzy, d);
      weights = vec4<f32>(rg, weights.zw);
    } else {
      e.r = 0.0;
    }
  }

  if (e.r > 0.0) {
    var coords = vec3<f32>(v.offset0.x, search_y_up(v.offset1.xy, v.offset2.z), 0.0);
    let e1 = edges_at(coords.xy).g;
    coords.z = search_y_down(v.offset1.zw, v.offset2.w);
    let d = abs(round(metrics.ww * vec2<f32>(coords.y, coords.z) - v.pixcoord.yy));
    let sqrt_d = sqrt(d);
    let e2 = textureSampleLevel(edges_tex, linear_sampler, coords.xz, 0.0, vec2<i32>(0, 1)).g;
    coords.x = v.uv.x;
    let ba = detect_vertical_corner_pattern(area(sqrt_d, e1, e2, 0.0), coords.xyxz, d);
    weights = vec4<f32>(weights.xy, ba);
  }
  return weights;
}
)wgsl";

inline constexpr const char* kBlendWgsl = R"wgsl(
@group(0) @binding(0) var<uniform> metrics: vec4<f32>;
@group(0) @binding(1) var linear_sampler: sampler;
@group(0) @binding(2) var point_sampler: sampler;
@group(0) @binding(3) var color_tex: texture_2d<f32>;
@group(0) @binding(4) var blend_tex: texture_2d<f32>;
@group(0) @binding(5) var unused_search_tex: texture_2d<f32>;

struct BlendVertex {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
  @location(1) offset: vec4<f32>,
}

@vertex fn vs_main(@builtin(vertex_index) index: u32) -> BlendVertex {
  let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
  var v: BlendVertex;
  v.position = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0);
  v.uv = uv;
  v.offset = metrics.xyxy * vec4<f32>(1.0, 0.0, 0.0, 1.0) + uv.xyxy;
  return v;
}

fn color_at(uv: vec2<f32>) -> vec4<f32> {
  return textureSampleLevel(color_tex, linear_sampler, uv, 0.0);
}

@fragment fn fs_main(v: BlendVertex) -> @location(0) vec4<f32> {
  let here = textureSampleLevel(blend_tex, linear_sampler, v.uv, 0.0);
  let a = vec4<f32>(
    textureSampleLevel(blend_tex, linear_sampler, v.offset.xy, 0.0).a,
    textureSampleLevel(blend_tex, linear_sampler, v.offset.zw, 0.0).g,
    here.z,
    here.x);

  if (dot(a, vec4<f32>(1.0)) < 1e-5) {
    return color_at(v.uv);
  }
  let h = max(a.x, a.z) > max(a.y, a.w);
  let blending_offset = select(vec4<f32>(0.0, a.y, 0.0, a.w), vec4<f32>(a.x, 0.0, a.z, 0.0), h);
  var blending_weight = select(a.yw, a.xz, h);
  blending_weight /= dot(blending_weight, vec2<f32>(1.0));
  let blending_coord = blending_offset * vec4<f32>(metrics.xy, -metrics.xy) + v.uv.xyxy;
  return blending_weight.x * color_at(blending_coord.xy) + blending_weight.y * color_at(blending_coord.zw);
}
)wgsl";

}  // namespace gpu::smaa
