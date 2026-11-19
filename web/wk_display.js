// Copyright 2026 Inggrit Setya Budi
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Web display conversion for 16-bit .wk frames (web/app.js).
// Viewer-only: mirrors the native tone-map in tools/tonemap.h
// (slice 3, Reinhard with a geometric-mean key value) so the web
// viewer shows the same picture as wkview. The formulas below are
// transcribed 1:1 from src/core/colorspace.h (pq/hlg/bt709/srgb) and
// tools/tonemap.h — same constants, same operation order, same
// 1e-6 floors — so JS and native agree to the last code value.
//
// Samples arrive as little-endian uint16 bytes, the layout wk_decode
// writes (native wkview reads them with read_u16_le too). SDR
// transfers keep the historical linear downscale, exactly like
// native, so 16-bit SDR pixels never change. 8-bit frames (bpp 3/4)
// never reach this module: app.js keeps its existing direct path.
//
// No DOM, no wasm dependency: Node loads it via require (see
// tools/webhdr_test.js), the browser via <script> (window.WkDisplay).
(function (root, factory) {
    "use strict";
    if (typeof module !== "undefined" && module.exports) {
        module.exports = factory();
    } else {
        root.WkDisplay = factory();
    }
})(typeof self !== "undefined" ? self : this, function () {
    "use strict";

    // PQ (ST 2084), src/core/colorspace.h wk::pq.
    var PQ_M1 = 0.1593017578125;
    var PQ_M2 = 78.84375;
    var PQ_C1 = 0.8359375;
    var PQ_C2 = 18.8515625;
    var PQ_C3 = 18.6875;

    // HLG (BT.2100), src/core/colorspace.h wk::hlg.
    var HLG_A = 0.17883277;
    var HLG_B = 0.28466892;
    var HLG_C = 0.55991073;

    function pqEotf(ep) {
        if (ep <= 0) return 0;
        var epPow = Math.pow(ep, 1 / PQ_M2);
        var num = Math.max(epPow - PQ_C1, 0);
        var den = PQ_C2 - PQ_C3 * epPow;
        if (den <= 0) return 0;
        return 10000 * Math.pow(num / den, 1 / PQ_M1);
    }

    function hlgInvOetf(ep) {
        if (ep <= 0) return 0;
        if (ep <= 0.5) return (ep * ep) / 3;
        return (Math.exp((ep - HLG_C) / HLG_A) + HLG_B) / 12;
    }

    function bt709Eotf(v) {
        if (v < 0.081) return v / 4.5;
        return Math.pow((v + 0.099) / 1.099, 1 / 0.45);
    }

    function srgbEotf(v) {
        if (v <= 0.04045) return v / 12.92;
        return Math.pow((v + 0.055) / 1.055, 2.4);
    }

    // Same switch + default as wk::apply_eotf in colorspace.h.
    function applyEotf(ep, transfer) {
        switch (transfer) {
            case 1: return bt709Eotf(ep);
            case 13: return srgbEotf(ep);
            case 16: return pqEotf(ep);
            case 18: return hlgInvOetf(ep);
            default: return srgbEotf(ep);
        }
    }

    function srgbOetf(l) {
        if (l <= 0.0031308) return 12.92 * l;
        return 1.055 * Math.pow(l, 1 / 2.4) - 0.055;
    }

    // Linear light, 0..1, from one code value. PQ EOTF returns nits,
    // so it is divided by 10000; HLG inverse OETF already returns
    // scene-linear 0..1. Same as wkview::tonemap_linear_rel.
    function linearRel(code, maxValue, transfer) {
        var ep = code / maxValue;
        var linear = applyEotf(ep, transfer);
        if (transfer === 16) linear /= 10000;
        return linear;
    }

    // BT.709 luminance weights, for the key-value pre-pass.
    function luminance(r, g, b) {
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
    }

    // Geometric mean of per-pixel linear luminance, with the same
    // 1e-6 floor the native code and the measurement harness use.
    function geometricMean(lums) {
        if (!lums || lums.length === 0) return 0;
        var sum = 0;
        for (var i = 0; i < lums.length; i++) sum += Math.log(lums[i] + 1e-6);
        return Math.exp(sum / lums.length);
    }

    // Textbook Reinhard key value: the geometric mean maps to 0.18.
    function keyScale(geo) {
        return 0.18 / Math.max(geo, 1e-6);
    }

    // Reinhard on linear light with the key value already baked into
    // keyScale: x = linear * keyScale, out = x / (1 + x), then sRGB
    // OETF. Monotonic, 0 -> 0, finite for any finite input.
    function tonemapChannel(lin, key) {
        var l = Math.max(lin, 0);
        var x = l * key;
        var tm = x / (1 + x);
        var srgb = Math.min(Math.max(srgbOetf(tm), 0), 1);
        return Math.floor(srgb * 255 + 0.5);
    }

    // Historical linear downscale, exact integer math like native:
    // (sample * 255 + maxValue / 2) / maxValue, integer division.
    function linearDownscale(sample, maxValue) {
        return Math.floor((sample * 255 + Math.floor(maxValue / 2)) / maxValue);
    }

    function isHdrTransfer(transfer) {
        return transfer === 16 || transfer === 18;
    }

    // One 16-bit sample to an 8-bit display value. HDR transfers go
    // through the tone-map; SDR transfers keep the linear downscale.
    function tonemapToU8(sample, maxValue, transfer, key) {
        if (!isHdrTransfer(transfer)) return linearDownscale(sample, maxValue);
        return tonemapChannel(linearRel(sample, maxValue, transfer), key);
    }

    function transferName(transfer) {
        switch (transfer) {
            case 1: return "BT.709";
            case 13: return "sRGB";
            case 16: return "PQ (ST 2084)";
            case 18: return "HLG";
            default: return "Unknown (" + transfer + ")";
        }
    }

    // Full 16-bit frame to 8-bit RGBA for ImageData.
    // bytes: Uint8Array of little-endian uint16 samples, R,G,B(,A)
    // order, length width*height*channels*2. channels is 3 (bpp 6)
    // or 4 (bpp 8). Alpha is never tone-mapped: it stays linear,
    // exactly like native rasterize; bpp 6 frames get alpha 255.
    function convert16ToRGBA(bytes, width, height, channels, bitDepth, transfer) {
        var count = width * height;
        var maxValue = (1 << bitDepth) - 1;
        var view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        var out = new Uint8ClampedArray(count * 4);
        var key = 0;
        var lums = null;
        var i, c;
        if (isHdrTransfer(transfer)) {
            lums = new Float64Array(count);
            for (i = 0; i < count; i++) {
                var r = linearRel(view.getUint16((i * channels) * 2, true), maxValue, transfer);
                var g = linearRel(view.getUint16((i * channels + 1) * 2, true), maxValue, transfer);
                var b = linearRel(view.getUint16((i * channels + 2) * 2, true), maxValue, transfer);
                lums[i] = luminance(r, g, b);
            }
            key = keyScale(geometricMean(lums));
        }
        for (i = 0; i < count; i++) {
            var o = i * 4;
            for (c = 0; c < 3; c++) {
                var sample = view.getUint16((i * channels + c) * 2, true);
                out[o + c] = tonemapToU8(sample, maxValue, transfer, key);
            }
            if (channels === 4) {
                out[o + 3] = linearDownscale(view.getUint16((i * channels + 3) * 2, true), maxValue);
            } else {
                out[o + 3] = 255;
            }
        }
        return out;
    }

    return {
        pqEotf: pqEotf,
        hlgInvOetf: hlgInvOetf,
        srgbOetf: srgbOetf,
        applyEotf: applyEotf,
        linearRel: linearRel,
        luminance: luminance,
        geometricMean: geometricMean,
        keyScale: keyScale,
        tonemapChannel: tonemapChannel,
        linearDownscale: linearDownscale,
        isHdrTransfer: isHdrTransfer,
        tonemapToU8: tonemapToU8,
        transferName: transferName,
        convert16ToRGBA: convert16ToRGBA
    };
});
