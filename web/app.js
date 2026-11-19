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

(function () {
    "use strict";

    var $ = function (id) { return document.getElementById(id); };

    var dropZone = $("drop-zone");
    var fileInput = $("file-input");
    var viewer = $("viewer");
    var canvas = $("canvas");
    var ctx = canvas.getContext("2d");
    var decodeInfo = $("decode-info");
    var encodeInfo = $("encode-info");
    var infoGrid = $("info-grid");
    var encodeStats = $("encode-stats");
    var encodeResult = $("encode-result");
    var overlay = $("overlay");
    var overlayText = $("overlay-text");
    var histCanvas = $("histogram");
    var histCtx = histCanvas.getContext("2d");
    var qualityField = $("quality-field");
    var qualitySlider = $("enc-quality");
    var qualityLabel = $("quality-val");
    var modeSelect = $("enc-mode");
    var versionLabel = $("version-label");

    var wasm = null;
    var currentPixels = null;
    var currentWidth = 0;
    var currentHeight = 0;
    var encodedBlob = null;

    function heap() {
        return new Uint8Array(wasm.wasmMemory.buffer);
    }

    function heapU32() {
        return new Uint32Array(wasm.wasmMemory.buffer);
    }

    function writeBytes(ptr, src) {
        heap().set(src, ptr);
    }

    // function readBytes(ptr, len) {
    //     return new Uint8Array(heap().buffer, ptr, len);
    // }

    function readU32(ptr) {
        return heapU32()[ptr >> 2];
    }

    // function writeU32(ptr, val) {
    //     heapU32()[ptr >> 2] = val;
    // }

    WkModule().then(function (instance) {
        wasm = instance;
        try {
            var verPtr = wasm._wk_wasm_version();
            var h = heap();
            var str = "";
            for (var i = verPtr; h[i] !== 0; i++) {
                str += String.fromCharCode(h[i]);
            }
            versionLabel.textContent = "WK " + str + " | WebAssembly";
        } catch (e) {
            versionLabel.textContent = "WebAssembly Image Codec";
        }
    });

    function showOverlay(text) {
        overlayText.textContent = text;
        overlay.classList.remove("hidden");
    }

    function hideOverlay() {
        overlay.classList.add("hidden");
    }

    function showViewer() {
        dropZone.classList.add("hidden");
        viewer.classList.remove("hidden");
    }

    function resetViewer() {
        viewer.classList.add("hidden");
        dropZone.classList.remove("hidden");
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        decodeInfo.classList.add("hidden");
        encodeInfo.classList.add("hidden");
        encodeResult.classList.add("hidden");
        currentPixels = null;
        encodedBlob = null;
    }

    function formatBytes(n) {
        if (n < 1024) return n + " B";
        if (n < 1048576) return (n / 1024).toFixed(2) + " KB";
        return (n / 1048576).toFixed(2) + " MB";
    }

    function infoRow(label, value) {
        return '<div class="info-row"><span class="label">' + label + '</span><span class="value">' + value + '</span></div>';
    }

    $("btn-browse").addEventListener("click", function (e) {
        e.stopPropagation();
        fileInput.click();
    });

    dropZone.addEventListener("click", function () { fileInput.click(); });

    dropZone.addEventListener("dragover", function (e) {
        e.preventDefault();
        dropZone.classList.add("active");
    });

    dropZone.addEventListener("dragleave", function () {
        dropZone.classList.remove("active");
    });

    dropZone.addEventListener("drop", function (e) {
        e.preventDefault();
        dropZone.classList.remove("active");
        if (e.dataTransfer.files.length) handleFile(e.dataTransfer.files[0]);
    });

    fileInput.addEventListener("change", function () {
        if (fileInput.files.length) handleFile(fileInput.files[0]);
        fileInput.value = "";
    });

    $("btn-close").addEventListener("click", resetViewer);

    qualitySlider.addEventListener("input", function () {
        qualityLabel.textContent = qualitySlider.value;
    });

    modeSelect.addEventListener("change", function () {
        qualityField.classList.toggle("hidden", modeSelect.value === "1");
    });

    function handleFile(file) {
        if (!wasm) {
            alert("WASM module is still loading.");
            return;
        }
        var name = file.name.toLowerCase();
        if (name.endsWith(".wk")) {
            decodeWk(file);
        } else if (name.endsWith(".png") || name.endsWith(".jpg") || name.endsWith(".jpeg")) {
            loadForEncode(file);
        } else {
            alert("Unsupported format. Use .wk, .png, or .jpg files.");
        }
    }

    function decodeWk(file) {
        showOverlay("Decoding...");
        file.arrayBuffer().then(function (arrayBuf) {
            try {
                var buf = new Uint8Array(arrayBuf);

                var dataPtr = wasm._wk_wasm_alloc(buf.length);
                if (!dataPtr) throw new Error("Memory allocation failed");
                writeBytes(dataPtr, buf);

                var outW = wasm._malloc(4);
                var outH = wasm._malloc(4);
                var outBpp = wasm._malloc(4);
                var outPx = wasm._malloc(4);

                var err = wasm._wk_wasm_decode(dataPtr, buf.length, outW, outH, outBpp, outPx);

                var w = readU32(outW);
                var h = readU32(outH);
                var bpp = readU32(outBpp);
                var px = readU32(outPx);

                wasm._free(outW);
                wasm._free(outH);
                wasm._free(outBpp);
                wasm._free(outPx);

                // FHDR facts for the display path: the real bit depth
                // and CICP transfer (PQ/HLG/SDR), plus MaxCLL/MaxFALL
                // for the info panel. wk_wasm_get_info writes 15
                // uint32s (see web/wk_wasm.cpp for the layout). The
                // decoder alone cannot tell us the transfer, and
                // guessing it from bpp would tone-map SDR as HDR.
                // get_info re-reads the same buffer, so it succeeds
                // whenever decode did; if it does not, fail loudly
                // instead of showing a wrong picture.
                var infoPtr = wasm._malloc(60);
                var infoErr = wasm._wk_wasm_get_info(dataPtr, buf.length, infoPtr);
                var info = null;
                if (infoErr === 0) {
                    var iv = heapU32();
                    var base = infoPtr >> 2;
                    info = {
                        width: iv[base],
                        height: iv[base + 1],
                        bitDepth: iv[base + 2],
                        primaries: iv[base + 3],
                        transfer: iv[base + 4],
                        matrix: iv[base + 5],
                        maxCll: iv[base + 8],
                        maxFall: iv[base + 9],
                        isHdr: iv[base + 13]
                    };
                }
                wasm._free(infoPtr);
                wasm._wk_wasm_free(dataPtr);

                if (err !== 0) throw new Error("Decode error (code " + err + ")");
                if (!info) throw new Error("Could not read file info (code " + infoErr + ")");

                canvas.width = w;
                canvas.height = h;

                var totalPixelBytes = w * h * bpp;
                var rawPixels = new Uint8Array(heap().buffer, px, totalPixelBytes);
                var pixelsCopy = new Uint8Array(rawPixels);

                wasm._wk_wasm_free(px);

                var imgData = ctx.createImageData(w, h);

                if (bpp === 4) {
                    imgData.data.set(pixelsCopy);
                } else if (bpp === 3) {
                    for (var s = 0, d = 0; s < pixelsCopy.length; s += 3, d += 4) {
                        imgData.data[d] = pixelsCopy[s];
                        imgData.data[d + 1] = pixelsCopy[s + 1];
                        imgData.data[d + 2] = pixelsCopy[s + 2];
                        imgData.data[d + 3] = 255;
                    }
                } else if (bpp === 6 || bpp === 8) {
                    // 16-bit samples, little-endian (the layout
                    // wk_decode writes). Same Reinhard geometric-mean
                    // tone-map as native wkview (tools/tonemap.h),
                    // so the web viewer shows the same picture;
                    // 16-bit SDR keeps the linear downscale.
                    var channels = bpp === 8 ? 4 : 3;
                    var rgba = WkDisplay.convert16ToRGBA(
                        pixelsCopy, w, h, channels, info.bitDepth, info.transfer
                    );
                    imgData.data.set(rgba);
                } else {
                    throw new Error("Unsupported pixel format (bpp " + bpp + ")");
                }

                ctx.putImageData(imgData, 0, 0);
                drawHistogram(imgData);

                var channelsLabel = bpp === 4 ? "RGBA" : bpp === 3 ? "RGB" :
                    bpp === 8 ? "RGBA 16-bit" : "RGB 16-bit";
                infoGrid.innerHTML =
                    infoRow("Filename", file.name) +
                    infoRow("Dimensions", w + " x " + h) +
                    infoRow("Channels", channelsLabel) +
                    infoRow("Bit Depth", info.bitDepth + " bit") +
                    infoRow("Transfer", WkDisplay.transferName(info.transfer)) +
                    infoRow("MaxCLL", info.maxCll ? info.maxCll + " nits" : "unspecified") +
                    infoRow("MaxFALL", info.maxFall ? info.maxFall + " nits" : "unspecified") +
                    infoRow("File Size", formatBytes(file.size));

                encodeInfo.classList.add("hidden");
                decodeInfo.classList.remove("hidden");
                showViewer();
            } catch (e) {
                alert(e.message);
            } finally {
                hideOverlay();
            }
        });
    }

    function loadForEncode(file) {
        showOverlay("Loading...");
        var url = URL.createObjectURL(file);
        var img = new Image();
        img.onload = function () {
            canvas.width = img.width;
            canvas.height = img.height;
            ctx.drawImage(img, 0, 0);
            URL.revokeObjectURL(url);

            var imgData = ctx.getImageData(0, 0, img.width, img.height);
            currentPixels = new Uint8Array(imgData.data);
            currentWidth = img.width;
            currentHeight = img.height;

            encodeResult.classList.add("hidden");
            decodeInfo.classList.add("hidden");
            encodeInfo.classList.remove("hidden");
            showViewer();
            hideOverlay();
        };
        img.onerror = function () {
            alert("Failed to load image.");
            hideOverlay();
        };
        img.src = url;
    }

    $("btn-encode").addEventListener("click", function () {
        if (!currentPixels || !wasm) return;
        showOverlay("Encoding...");
        setTimeout(doEncode, 30);
    });

    function doEncode() {
        try {
            var bpp = 4;

            var ptr = wasm._wk_wasm_alloc(currentPixels.length);
            if (!ptr) throw new Error("Memory allocation failed");
            writeBytes(ptr, currentPixels);

            var quality = parseFloat(qualitySlider.value);
            var lossless = parseInt(modeSelect.value, 10);
            var outSize = wasm._malloc(4);
            var outData = wasm._malloc(4);

            var err = wasm._wk_wasm_encode(
                ptr, currentWidth, currentHeight, bpp,
                quality, lossless, outSize, outData
            );

            var sz = readU32(outSize);
            var dataPtr = readU32(outData);

            wasm._free(outSize);
            wasm._free(outData);
            wasm._wk_wasm_free(ptr);

            if (err !== 0) throw new Error("Encode error (code " + err + ")");

            var encoded = new Uint8Array(heap().buffer, dataPtr, sz);
            encodedBlob = new Uint8Array(encoded);
            wasm._wk_wasm_free(dataPtr);

            var rawSize = currentWidth * currentHeight * bpp;
            var ratio = ((sz / rawSize) * 100).toFixed(1);

            encodeStats.innerHTML =
                infoRow("Output Size", formatBytes(sz)) +
                infoRow("Compression", ratio + "% of raw") +
                infoRow("Mode", lossless ? "Lossless" : "Lossy (Q" + quality + ")");

            encodeResult.classList.remove("hidden");
        } catch (e) {
            alert(e.message);
        } finally {
            hideOverlay();
        }
    }

    $("btn-download").addEventListener("click", function () {
        if (!encodedBlob) return;
        var a = document.createElement("a");
        a.href = URL.createObjectURL(new Blob([encodedBlob], { type: "application/octet-stream" }));
        a.download = "output_" + Date.now() + ".wk";
        a.click();
        URL.revokeObjectURL(a.href);
    });

    function drawHistogram(imgData) {
        var bins = new Uint32Array(256);
        var px = imgData.data;
        var peak = 0;

        for (var i = 0; i < px.length; i += 4) {
            var luma = (77 * px[i] + 150 * px[i + 1] + 29 * px[i + 2]) >> 8;
            bins[luma]++;
            if (bins[luma] > peak) peak = bins[luma];
        }

        var w = histCanvas.width;
        var h = histCanvas.height;
        histCtx.clearRect(0, 0, w, h);

        var grad = histCtx.createLinearGradient(0, h, 0, 0);
        grad.addColorStop(0, "rgba(59,130,246,0.2)");
        grad.addColorStop(1, "rgba(59,130,246,0.8)");
        histCtx.fillStyle = grad;

        var barW = w / 256;
        for (var i = 0; i < 256; i++) {
            var barH = (bins[i] / peak) * h;
            histCtx.fillRect(i * barW, h - barH, Math.ceil(barW), barH);
        }
    }
})();
