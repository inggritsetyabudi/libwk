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
#include "metrics.h"
#include "colorspace.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace wk::metrics {

namespace {

struct DerivedPlanes {
    std::array<std::vector<double>, 3> planes{};
};

uint32_t max_sample_value(BitDepth bit_depth) {
    switch (bit_depth) {
    case BitDepth::Bits8:
        return 255u;
    case BitDepth::Bits10:
        return 1023u;
    case BitDepth::Bits12:
        return 4095u;
    }
    return 255u;
}

uint8_t channel_count(const Image& image) {
    return image.has_alpha() ? 4u : 3u;
}

double psnr_from_mse(double mse, double max_value) {
    if (mse <= 1e-12) {
        return 100.0;
    }
    return 10.0 * std::log10((max_value * max_value) / mse);
}

DerivedPlanes build_ycbcr_planes(const Image& image, double max_value) {
    DerivedPlanes derived;
    const ColorMatrix matrix = get_rgb_to_ycbcr(1);
    const double chroma_offset = max_value * 0.5;
    const size_t pixel_count = static_cast<size_t>(image.width()) * image.height();

    for (auto& plane : derived.planes) {
        plane.resize(pixel_count);
    }

    for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
        const double r = sample_at(image, pixel_index, 0) / max_value;
        const double g = sample_at(image, pixel_index, 1) / max_value;
        const double b = sample_at(image, pixel_index, 2) / max_value;

        double y = 0.0;
        double cb = 0.0;
        double cr = 0.0;
        matrix.transform(r, g, b, y, cb, cr);

        derived.planes[0][pixel_index] = std::clamp(y * max_value, 0.0, max_value);
        derived.planes[1][pixel_index] = std::clamp(cb * max_value + chroma_offset, 0.0, max_value);
        derived.planes[2][pixel_index] = std::clamp(cr * max_value + chroma_offset, 0.0, max_value);
    }

    return derived;
}

double compute_channel_ssim(const Image& reference, const Image& candidate, uint8_t channel_index, double max_value) {
    constexpr uint32_t window_size = 8;
    const double c1 = std::pow(0.01 * max_value, 2.0);
    const double c2 = std::pow(0.03 * max_value, 2.0);
    double total_ssim = 0.0;
    uint32_t window_count = 0;

    for (uint32_t y = 0; y < reference.height(); y += window_size) {
        const uint32_t win_h = std::min(window_size, reference.height() - y);
        for (uint32_t x = 0; x < reference.width(); x += window_size) {
            const uint32_t win_w = std::min(window_size, reference.width() - x);
            double sum_x = 0.0;
            double sum_y = 0.0;
            double sum_x2 = 0.0;
            double sum_y2 = 0.0;
            double sum_xy = 0.0;

            for (uint32_t wy = 0; wy < win_h; ++wy) {
                for (uint32_t wx = 0; wx < win_w; ++wx) {
                    const size_t pixel_index = static_cast<size_t>(y + wy) * reference.width() + (x + wx);
                    const double ref_sample = sample_at(reference, pixel_index, channel_index);
                    const double cand_sample = sample_at(candidate, pixel_index, channel_index);
                    sum_x += ref_sample;
                    sum_y += cand_sample;
                    sum_x2 += ref_sample * ref_sample;
                    sum_y2 += cand_sample * cand_sample;
                    sum_xy += ref_sample * cand_sample;
                }
            }

            const double count = static_cast<double>(win_w) * win_h;
            const double mean_x = sum_x / count;
            const double mean_y = sum_y / count;
            const double var_x = std::max(0.0, (sum_x2 / count) - (mean_x * mean_x));
            const double var_y = std::max(0.0, (sum_y2 / count) - (mean_y * mean_y));
            const double cov_xy = (sum_xy / count) - (mean_x * mean_y);
            const double numerator = (2.0 * mean_x * mean_y + c1) * (2.0 * cov_xy + c2);
            const double denominator = (mean_x * mean_x + mean_y * mean_y + c1) * (var_x + var_y + c2);
            total_ssim += denominator > 0.0 ? numerator / denominator : 1.0;
            ++window_count;
        }
    }

    return window_count > 0 ? total_ssim / window_count : 1.0;
}

double compute_plane_ssim(const std::vector<double>& reference,
                          const std::vector<double>& candidate,
                          uint32_t width,
                          uint32_t height,
                          double max_value) {
    constexpr uint32_t window_size = 8;
    const double c1 = std::pow(0.01 * max_value, 2.0);
    const double c2 = std::pow(0.03 * max_value, 2.0);
    double total_ssim = 0.0;
    uint32_t window_count = 0;

    for (uint32_t y = 0; y < height; y += window_size) {
        const uint32_t win_h = std::min(window_size, height - y);
        for (uint32_t x = 0; x < width; x += window_size) {
            const uint32_t win_w = std::min(window_size, width - x);
            double sum_x = 0.0;
            double sum_y = 0.0;
            double sum_x2 = 0.0;
            double sum_y2 = 0.0;
            double sum_xy = 0.0;

            for (uint32_t wy = 0; wy < win_h; ++wy) {
                for (uint32_t wx = 0; wx < win_w; ++wx) {
                    const size_t pixel_index = static_cast<size_t>(y + wy) * width + (x + wx);
                    const double ref_sample = reference[pixel_index];
                    const double cand_sample = candidate[pixel_index];
                    sum_x += ref_sample;
                    sum_y += cand_sample;
                    sum_x2 += ref_sample * ref_sample;
                    sum_y2 += cand_sample * cand_sample;
                    sum_xy += ref_sample * cand_sample;
                }
            }

            const double count = static_cast<double>(win_w) * win_h;
            const double mean_x = sum_x / count;
            const double mean_y = sum_y / count;
            const double var_x = std::max(0.0, (sum_x2 / count) - (mean_x * mean_x));
            const double var_y = std::max(0.0, (sum_y2 / count) - (mean_y * mean_y));
            const double cov_xy = (sum_xy / count) - (mean_x * mean_y);
            const double numerator = (2.0 * mean_x * mean_y + c1) * (2.0 * cov_xy + c2);
            const double denominator = (mean_x * mean_x + mean_y * mean_y + c1) * (var_x + var_y + c2);
            total_ssim += denominator > 0.0 ? numerator / denominator : 1.0;
            ++window_count;
        }
    }

    return window_count > 0 ? total_ssim / window_count : 1.0;
}

ChannelMetrics compute_channel_metrics(const Image& reference, const Image& candidate, uint8_t channel_index, double max_value) {
    const size_t pixel_count = static_cast<size_t>(reference.width()) * reference.height();
    double sum_abs = 0.0;
    double sum_sq = 0.0;

    for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
        const double ref_sample = sample_at(reference, pixel_index, channel_index);
        const double cand_sample = sample_at(candidate, pixel_index, channel_index);
        const double delta = ref_sample - cand_sample;
        sum_abs += std::abs(delta);
        sum_sq += delta * delta;
    }

    ChannelMetrics metrics;
    if (pixel_count > 0) {
        metrics.mae = sum_abs / static_cast<double>(pixel_count);
        metrics.mse = sum_sq / static_cast<double>(pixel_count);
    }
    metrics.psnr = psnr_from_mse(metrics.mse, max_value);
    metrics.ssim = compute_channel_ssim(reference, candidate, channel_index, max_value);
    return metrics;
}

ChannelMetrics compute_plane_metrics(const std::vector<double>& reference,
                                     const std::vector<double>& candidate,
                                     uint32_t width,
                                     uint32_t height,
                                     double max_value) {
    const size_t pixel_count = static_cast<size_t>(width) * height;
    double sum_abs = 0.0;
    double sum_sq = 0.0;

    for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
        const double delta = reference[pixel_index] - candidate[pixel_index];
        sum_abs += std::abs(delta);
        sum_sq += delta * delta;
    }

    ChannelMetrics metrics;
    if (pixel_count > 0) {
        metrics.mae = sum_abs / static_cast<double>(pixel_count);
        metrics.mse = sum_sq / static_cast<double>(pixel_count);
    }
    metrics.psnr = psnr_from_mse(metrics.mse, max_value);
    metrics.ssim = compute_plane_ssim(reference, candidate, width, height, max_value);
    return metrics;
}

ImageStatistics compute_image_statistics(const DerivedPlanes& planes, double max_value) {
    ImageStatistics stats;
    if (max_value <= 0.0) {
        return stats;
    }

    const size_t pixel_count = planes.planes[0].size();
    if (pixel_count == 0) {
        return stats;
    }

    const double chroma_mid = max_value * 0.5;
    const double max_chroma_radius = max_value * std::sqrt(0.5);
    double sum_y = 0.0;
    double sum_y2 = 0.0;
    double sum_chroma = 0.0;
    size_t dark_count = 0;
    size_t bright_count = 0;

    for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
        const double y_norm = planes.planes[0][pixel_index] / max_value;
        const double cb = planes.planes[1][pixel_index] - chroma_mid;
        const double cr = planes.planes[2][pixel_index] - chroma_mid;
        const double chroma_norm = max_chroma_radius > 0.0
            ? std::clamp(std::hypot(cb, cr) / max_chroma_radius, 0.0, 1.0)
            : 0.0;

        sum_y += y_norm;
        sum_y2 += y_norm * y_norm;
        sum_chroma += chroma_norm;
        if (y_norm <= 0.25) {
            ++dark_count;
        }
        if (y_norm >= 0.75) {
            ++bright_count;
        }
    }

    stats.mean_luma = sum_y / static_cast<double>(pixel_count);
    stats.luma_stddev = std::sqrt(std::max(0.0, sum_y2 / static_cast<double>(pixel_count) - stats.mean_luma * stats.mean_luma));
    stats.mean_chroma = sum_chroma / static_cast<double>(pixel_count);
    stats.dark_fraction = static_cast<double>(dark_count) / static_cast<double>(pixel_count);
    stats.bright_fraction = static_cast<double>(bright_count) / static_cast<double>(pixel_count);
    return stats;
}
ArtifactMetrics compute_artifact_metrics(const Image& reference,
                                         const Image& candidate,
                                         uint8_t compared_channels,
                                         const DerivedPlanes& reference_ycbcr,
                                         const DerivedPlanes& candidate_ycbcr,
                                         double max_value) {
    ArtifactMetrics artifacts;
    const uint32_t width = reference.width();
    const uint32_t height = reference.height();
    const size_t pixel_count = static_cast<size_t>(width) * height;
    const double chroma_mid = max_value * 0.5;
    double weighted_luma_sum = 0.0;
    double luma_weight_sum = 0.0;
    double weighted_chroma_sum = 0.0;
    double chroma_weight_sum = 0.0;

    for (size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
        const uint32_t x = static_cast<uint32_t>(pixel_index % width);
        const uint32_t y = static_cast<uint32_t>(pixel_index / width);
        const double ref_y = reference_ycbcr.planes[0][pixel_index];
        const double cand_y = candidate_ycbcr.planes[0][pixel_index];
        const double ref_cb = reference_ycbcr.planes[1][pixel_index];
        const double cand_cb = candidate_ycbcr.planes[1][pixel_index];
        const double ref_cr = reference_ycbcr.planes[2][pixel_index];
        const double cand_cr = candidate_ycbcr.planes[2][pixel_index];

        double edge_strength = 0.0;
        if (x + 1 < width) {
            edge_strength = std::max(edge_strength, std::abs(ref_y - reference_ycbcr.planes[0][pixel_index + 1]));
        }
        if (x > 0) {
            edge_strength = std::max(edge_strength, std::abs(ref_y - reference_ycbcr.planes[0][pixel_index - 1]));
        }
        if (y + 1 < height) {
            edge_strength = std::max(edge_strength, std::abs(ref_y - reference_ycbcr.planes[0][pixel_index + width]));
        }
        if (y > 0) {
            edge_strength = std::max(edge_strength, std::abs(ref_y - reference_ycbcr.planes[0][pixel_index - width]));
        }

        const double edge_weight = 1.0 + 3.0 * std::clamp(edge_strength / max_value, 0.0, 1.0);
        const double chroma_radius = std::hypot(ref_cb - chroma_mid, ref_cr - chroma_mid) / max_value;
        const double chroma_weight = edge_weight * (1.0 + 1.75 * std::clamp(chroma_radius, 0.0, 1.0));
        const double luma_abs = std::abs(ref_y - cand_y);
        const double chroma_abs = 0.5 * (std::abs(ref_cb - cand_cb) + std::abs(ref_cr - cand_cr));

        weighted_luma_sum += luma_abs * edge_weight;
        luma_weight_sum += edge_weight;
        weighted_chroma_sum += chroma_abs * chroma_weight;
        chroma_weight_sum += chroma_weight;

        for (uint8_t channel_index = 0; channel_index < compared_channels; ++channel_index) {
            artifacts.max_abs_error = std::max(
                artifacts.max_abs_error,
                std::abs(sample_at(reference, pixel_index, channel_index) - sample_at(candidate, pixel_index, channel_index)));
        }
    }

    if (luma_weight_sum > 0.0) {
        artifacts.weighted_luma_mae = weighted_luma_sum / luma_weight_sum;
    }
    if (chroma_weight_sum > 0.0) {
        artifacts.weighted_chroma_mae = weighted_chroma_sum / chroma_weight_sum;
    }

    return artifacts;
}

}

double sample_at(const Image& image, size_t pixel_index, uint8_t channel_index) {
    const auto pixels = image.pixels();
    const size_t channels = channel_count(image);
    const size_t bytes_per_sample = image.bit_depth() == BitDepth::Bits8 ? 1u : 2u;
    // Out-of-range reads used to walk off the end of the pixel buffer
    // whenever a caller indexed past the image (e.g. comparing images
    // of different sizes). compare_images() rejects mismatched sizes
    // first, so this only fires on a caller bug -- return 0 rather
    // than reading out of bounds.
    if (pixel_index >= pixels.size() / (channels * bytes_per_sample) || channel_index >= channels) {
        return 0.0;
    }
    if (image.bit_depth() == BitDepth::Bits8) {
        const size_t offset = pixel_index * channels + channel_index;
        return static_cast<double>(pixels[offset]);
    }

    const size_t offset = (pixel_index * channels + channel_index) * 2;
    return static_cast<double>(read_le16(pixels.data() + offset));
}

Result<ImageMetrics> compare_images(const Image& reference, const Image& candidate, bool include_alpha) {
    if (reference.width() == 0 || reference.height() == 0 || candidate.width() == 0 || candidate.height() == 0) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "cannot compare empty images"});
    }
    if (reference.width() != candidate.width() || reference.height() != candidate.height()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "image dimensions do not match"});
    }
    if (reference.bit_depth() != candidate.bit_depth()) {
        return std::unexpected(Error{ErrorCode::InvalidParameter, "image bit depths do not match"});
    }

    ImageMetrics result;
    result.width = reference.width();
    result.height = reference.height();
    result.compared_channels = static_cast<uint8_t>((include_alpha && reference.has_alpha() && candidate.has_alpha()) ? 4 : 3);
    result.compared_alpha = result.compared_channels == 4;

    const double max_value = static_cast<double>(max_sample_value(reference.bit_depth()));
    const size_t pixel_count = static_cast<size_t>(reference.width()) * reference.height();
    double total_abs = 0.0;
    double total_sq = 0.0;
    double total_ssim = 0.0;

    for (uint8_t channel_index = 0; channel_index < result.compared_channels; ++channel_index) {
        result.channels[channel_index] = compute_channel_metrics(reference, candidate, channel_index, max_value);
        total_abs += result.channels[channel_index].mae * static_cast<double>(pixel_count);
        total_sq += result.channels[channel_index].mse * static_cast<double>(pixel_count);
        total_ssim += result.channels[channel_index].ssim;
    }

    const double sample_count = static_cast<double>(pixel_count) * result.compared_channels;
    result.mae = total_abs / sample_count;
    result.mse = total_sq / sample_count;
    result.psnr = psnr_from_mse(result.mse, max_value);
    result.ssim = total_ssim / result.compared_channels;

    const DerivedPlanes reference_ycbcr = build_ycbcr_planes(reference, max_value);
    const DerivedPlanes candidate_ycbcr = build_ycbcr_planes(candidate, max_value);
    result.reference_stats = compute_image_statistics(reference_ycbcr, max_value);
    result.candidate_stats = compute_image_statistics(candidate_ycbcr, max_value);
    for (size_t plane_index = 0; plane_index < result.ycbcr.size(); ++plane_index) {
        result.ycbcr[plane_index] = compute_plane_metrics(
            reference_ycbcr.planes[plane_index],
            candidate_ycbcr.planes[plane_index],
            result.width,
            result.height,
            max_value);
    }

    result.artifacts = compute_artifact_metrics(
        reference,
        candidate,
        result.compared_channels,
        reference_ycbcr,
        candidate_ycbcr,
        max_value);
    result.artifacts.chroma_mae = 0.5 * (result.ycbcr[1].mae + result.ycbcr[2].mae);
    result.artifacts.chroma_mse = 0.5 * (result.ycbcr[1].mse + result.ycbcr[2].mse);
    result.artifacts.chroma_psnr = psnr_from_mse(result.artifacts.chroma_mse, max_value);
    return result;
}

}
