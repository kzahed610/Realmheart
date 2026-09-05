#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/ximgproc/segmentation.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kFrameMagic = 0x52485346u; // RHSF
constexpr std::uint32_t kFrameVersion = 1u;

struct SharedFrameHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::int32_t width;
    std::int32_t height;
    std::int32_t stride;
    std::uint32_t reserved;
    std::uint64_t byte_size;
};

struct PixelRegion {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

double area(const PixelRegion& rect) {
    return std::max(0.0, rect.width) * std::max(0.0, rect.height);
}

double intersection_area(const PixelRegion& a, const PixelRegion& b) {
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.x + a.width, b.x + b.width);
    const double bottom = std::min(a.y + a.height, b.y + b.height);
    return std::max(0.0, right - left) * std::max(0.0, bottom - top);
}

double iou(const PixelRegion& a, const PixelRegion& b) {
    const double inter = intersection_area(a, b);
    if (inter <= 0.0) return 0.0;
    const double union_area = area(a) + area(b) - inter;
    if (union_area <= std::numeric_limits<double>::epsilon()) return 0.0;
    return inter / union_area;
}

double contained_fraction(const PixelRegion& inner, const PixelRegion& outer) {
    const double inner_area = area(inner);
    if (inner_area <= std::numeric_limits<double>::epsilon()) return 0.0;
    return intersection_area(inner, outer) / inner_area;
}

struct TextEvidence {
    cv::Mat integral;
    std::vector<cv::Rect> lines;
};

struct RegionFeatures {
    double perimeter_edge_density = 0.0;
    double interior_edge_density = 0.0;
    double side_support = 0.0;
    double paired_side_support = 0.0;
    double boundary_contrast = 0.0;
    double contrast_side_support = 0.0;
    double luminance_variation = 0.0;
    double text_band_score = 0.0;
    double text_coverage = 0.0;
    double text_line_score = 0.0;
    double text_pressure = 0.0;
    double structure_confidence = 0.0;
    double contour_consensus = 0.0;
    double quality = 0.0;
    int structural_votes = 0;
    bool text_like = false;
};

struct ScoredRegion {
    PixelRegion rect;
    RegionFeatures features;
    bool rescued = false;
};

struct IntegralImages {
    cv::Mat edge;
    cv::Mat gray;
    cv::Mat gray_sq;
};

double integral_sum(const cv::Mat& integral, const cv::Rect& rect) {
    if (integral.empty() || rect.width <= 0 || rect.height <= 0) return 0.0;
    const int x1 = std::clamp(rect.x, 0, integral.cols - 1);
    const int y1 = std::clamp(rect.y, 0, integral.rows - 1);
    const int x2 = std::clamp(rect.x + rect.width, 0, integral.cols - 1);
    const int y2 = std::clamp(rect.y + rect.height, 0, integral.rows - 1);
    if (x2 <= x1 || y2 <= y1) return 0.0;

    return integral.at<double>(y2, x2)
        - integral.at<double>(y1, x2)
        - integral.at<double>(y2, x1)
        + integral.at<double>(y1, x1);
}

double rect_mean(const cv::Mat& integral, const cv::Rect& rect) {
    const double pixels = static_cast<double>(std::max(0, rect.width)) *
        static_cast<double>(std::max(0, rect.height));
    if (pixels <= 0.0) return 0.0;
    return integral_sum(integral, rect) / pixels;
}

cv::Rect clip_rect(const cv::Rect& rect, int width, int height) {
    const int left = std::clamp(rect.x, 0, width);
    const int top = std::clamp(rect.y, 0, height);
    const int right = std::clamp(rect.x + rect.width, 0, width);
    const int bottom = std::clamp(rect.y + rect.height, 0, height);
    return cv::Rect{
        left,
        top,
        std::max(0, right - left),
        std::max(0, bottom - top),
    };
}

cv::Rect contour_bounds(const std::vector<cv::Point>& contour) {
    if (contour.empty()) return {};

    int min_x = contour.front().x;
    int max_x = contour.front().x;
    int min_y = contour.front().y;
    int max_y = contour.front().y;
    for (const cv::Point& point : contour) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    return cv::Rect{
        min_x,
        min_y,
        max_x - min_x + 1,
        max_y - min_y + 1,
    };
}

double rect_iou(const cv::Rect& a, const cv::Rect& b) {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width);
    const int bottom = std::min(a.y + a.height, b.y + b.height);
    const double inter = static_cast<double>(std::max(0, right - left)) *
        static_cast<double>(std::max(0, bottom - top));
    if (inter <= 0.0) return 0.0;
    const double area_a = static_cast<double>(a.width) * a.height;
    const double area_b = static_cast<double>(b.width) * b.height;
    const double united = area_a + area_b - inter;
    return united > 0.0 ? inter / united : 0.0;
}

double horizontal_text_band_score(const cv::Mat& edge_binary, const cv::Rect& roi) {
    if (roi.width < 8 || roi.height < 8) return 0.0;

    const cv::Mat region = edge_binary(roi);
    int active_rows = 0;
    int runs = 0;
    bool in_run = false;
    for (int y = 0; y < region.rows; ++y) {
        const double density = static_cast<double>(cv::countNonZero(region.row(y))) /
            static_cast<double>(region.cols);
        const bool active = density >= 0.075;
        if (active) {
            ++active_rows;
            if (!in_run) {
                ++runs;
                in_run = true;
            }
        } else {
            in_run = false;
        }
    }

    const double active_fraction = static_cast<double>(active_rows) /
        static_cast<double>(region.rows);
    if (runs < 2 || active_fraction < 0.08 || active_fraction > 0.72) return 0.0;

    const double run_score = std::clamp(
        static_cast<double>(runs - 1) / 5.0,
        0.0,
        1.0
    );
    const double band_fraction_score = std::clamp(
        1.0 - std::abs(active_fraction - 0.34) / 0.34,
        0.0,
        1.0
    );
    return 0.62 * run_score + 0.38 * band_fraction_score;
}

double side_span_coverage(
    const cv::Mat& edge_binary,
    const cv::Rect& strip,
    bool horizontal
) {
    if (strip.width <= 0 || strip.height <= 0) return 0.0;
    cv::Mat projection;
    cv::reduce(
        edge_binary(strip),
        projection,
        horizontal ? 0 : 1,
        cv::REDUCE_MAX,
        CV_8U
    );
    const int span = horizontal ? strip.width : strip.height;
    if (span <= 0) return 0.0;
    return std::clamp(
        static_cast<double>(cv::countNonZero(projection)) /
            static_cast<double>(span),
        0.0,
        1.0
    );
}

TextEvidence build_text_evidence(const cv::Mat& edge_binary) {
    TextEvidence evidence;
    if (edge_binary.empty()) return evidence;

    cv::Mat edge_u8;
    edge_binary.convertTo(edge_u8, CV_8U, 255.0);

    // Characters are many tiny edge islands. A short horizontal close joins
    // neighbouring glyphs into word/line-shaped components without merging
    // normal card/panel borders across large distances.
    cv::Mat joined;
    const cv::Mat kernel = cv::getStructuringElement(
        cv::MORPH_RECT,
        cv::Size(5, 1)
    );
    cv::morphologyEx(edge_u8, joined, cv::MORPH_CLOSE, kernel);

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int count = cv::connectedComponentsWithStats(
        joined,
        labels,
        stats,
        centroids,
        8,
        CV_32S
    );

    cv::Mat mask = cv::Mat::zeros(edge_binary.rows, edge_binary.cols, CV_8U);
    const int max_line_height = std::max(
        5,
        static_cast<int>(std::round(edge_binary.rows * 0.055))
    );

    for (int label = 1; label < count; ++label) {
        const int x = stats.at<int>(label, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(label, cv::CC_STAT_TOP);
        const int width = stats.at<int>(label, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(label, cv::CC_STAT_HEIGHT);
        const int pixels = stats.at<int>(label, cv::CC_STAT_AREA);
        if (width < 9 || height < 2 || height > max_line_height) continue;

        const double aspect = static_cast<double>(width) /
            static_cast<double>(std::max(1, height));
        const double fill = static_cast<double>(pixels) /
            static_cast<double>(std::max(1, width * height));
        if (aspect < 1.85 || fill < 0.10 || fill > 0.92) continue;

        const cv::Rect line = clip_rect(
            cv::Rect{x, y, width, height},
            edge_binary.cols,
            edge_binary.rows
        );
        if (line.width <= 0 || line.height <= 0) continue;
        evidence.lines.push_back(line);
        cv::rectangle(mask, line, cv::Scalar{1}, cv::FILLED);
    }

    cv::integral(mask, evidence.integral, CV_64F);
    return evidence;
}

std::vector<cv::Rect> build_structural_rectangles(const cv::Mat& edge_binary) {
    std::vector<cv::Rect> structural;
    if (edge_binary.empty()) return structural;

    cv::Mat edge_u8;
    edge_binary.convertTo(edge_u8, CV_8U, 255.0);
    cv::Mat closed;
    const cv::Mat kernel = cv::getStructuringElement(
        cv::MORPH_RECT,
        cv::Size(3, 3)
    );
    cv::morphologyEx(edge_u8, closed, cv::MORPH_CLOSE, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(
        closed,
        contours,
        cv::RETR_LIST,
        cv::CHAIN_APPROX_SIMPLE
    );

    structural.reserve(std::min<std::size_t>(contours.size(), 256));
    for (const auto& contour : contours) {
        cv::Rect rect = clip_rect(
            contour_bounds(contour),
            edge_binary.cols,
            edge_binary.rows
        );
        if (rect.width < 22 || rect.height < 12) continue;
        if (rect.width > edge_binary.cols * 0.94 ||
            rect.height > edge_binary.rows * 0.94) {
            continue;
        }

        const double aspect = static_cast<double>(rect.width) /
            static_cast<double>(std::max(1, rect.height));
        if (aspect < 0.32 || aspect > 5.0) continue;

        const int thickness = std::clamp(
            static_cast<int>(std::round(std::min(rect.width, rect.height) * 0.055)),
            1,
            3
        );
        const cv::Rect top{rect.x, rect.y, rect.width, thickness};
        const cv::Rect bottom{
            rect.x,
            rect.y + std::max(0, rect.height - thickness),
            rect.width,
            thickness,
        };
        const cv::Rect left{rect.x, rect.y, thickness, rect.height};
        const cv::Rect right{
            rect.x + std::max(0, rect.width - thickness),
            rect.y,
            thickness,
            rect.height,
        };

        const double top_support = side_span_coverage(edge_binary, top, true);
        const double bottom_support = side_span_coverage(edge_binary, bottom, true);
        const double left_support = side_span_coverage(edge_binary, left, false);
        const double right_support = side_span_coverage(edge_binary, right, false);
        const double average =
            (top_support + bottom_support + left_support + right_support) / 4.0;
        const double paired = 0.5 * (
            std::min(top_support, bottom_support) +
            std::min(left_support, right_support)
        );

        // Text contours usually have lots of local edge pixels but almost no
        // long continuous vertical + horizontal perimeter agreement.
        if (average < 0.30 || paired < 0.16) continue;
        structural.push_back(rect);
        if (structural.size() >= 240) break;
    }
    return structural;
}

double contour_consensus(
    const cv::Rect& roi,
    const std::vector<cv::Rect>& structural_rectangles
) {
    double best = 0.0;
    for (const auto& structural : structural_rectangles) {
        best = std::max(best, rect_iou(roi, structural));
        if (best >= 0.94) break;
    }
    return best;
}

double directional_boundary_contrast(
    const cv::Mat& gray_integral,
    const cv::Rect& roi,
    int frame_width,
    int frame_height,
    int thickness,
    double& side_support
) {
    double sum = 0.0;
    int available = 0;
    int convincing = 0;

    const int band = std::max(1, thickness);
    const int pad = std::max(2, band * 2);
    const auto add_side = [&](const cv::Rect& inside, const cv::Rect& outside) {
        const cv::Rect clipped_inside = clip_rect(inside, frame_width, frame_height);
        const cv::Rect clipped_outside = clip_rect(outside, frame_width, frame_height);
        if (clipped_inside.width <= 0 || clipped_inside.height <= 0 ||
            clipped_outside.width <= 0 || clipped_outside.height <= 0) {
            return;
        }
        const double delta = std::abs(
            rect_mean(gray_integral, clipped_inside) -
            rect_mean(gray_integral, clipped_outside)
        );
        const double normalized = std::clamp(delta / 34.0, 0.0, 1.0);
        sum += normalized;
        ++available;
        if (delta >= 8.0) ++convincing;
    };

    add_side(
        cv::Rect{roi.x, roi.y, roi.width, band},
        cv::Rect{roi.x, roi.y - pad, roi.width, pad}
    );
    add_side(
        cv::Rect{roi.x, roi.y + std::max(0, roi.height - band), roi.width, band},
        cv::Rect{roi.x, roi.y + roi.height, roi.width, pad}
    );
    add_side(
        cv::Rect{roi.x, roi.y, band, roi.height},
        cv::Rect{roi.x - pad, roi.y, pad, roi.height}
    );
    add_side(
        cv::Rect{roi.x + std::max(0, roi.width - band), roi.y, band, roi.height},
        cv::Rect{roi.x + roi.width, roi.y, pad, roi.height}
    );

    side_support = available > 0
        ? static_cast<double>(convincing) / static_cast<double>(available)
        : 0.0;
    return available > 0 ? sum / static_cast<double>(available) : 0.0;
}

RegionFeatures score_region(
    const PixelRegion& region,
    int frame_width,
    int frame_height,
    const cv::Mat& score_edges,
    const IntegralImages& integrals,
    const TextEvidence& text_evidence,
    const std::vector<cv::Rect>& structural_rectangles,
    double score_scale_x,
    double score_scale_y
) {
    RegionFeatures features;

    cv::Rect roi{
        static_cast<int>(std::floor(region.x * score_scale_x)),
        static_cast<int>(std::floor(region.y * score_scale_y)),
        std::max(1, static_cast<int>(std::ceil(region.width * score_scale_x))),
        std::max(1, static_cast<int>(std::ceil(region.height * score_scale_y))),
    };
    roi = clip_rect(roi, score_edges.cols, score_edges.rows);
    if (roi.width < 8 || roi.height < 7) {
        features.text_like = true;
        return features;
    }

    const int thickness = std::clamp(
        static_cast<int>(std::round(std::min(roi.width, roi.height) * 0.060)),
        1,
        4
    );
    const int inner_inset = std::min(
        std::max(2, thickness * 2),
        std::max(2, std::min(roi.width, roi.height) / 4)
    );
    const cv::Rect inner = clip_rect(
        cv::Rect{
            roi.x + inner_inset,
            roi.y + inner_inset,
            roi.width - inner_inset * 2,
            roi.height - inner_inset * 2,
        },
        score_edges.cols,
        score_edges.rows
    );

    const double total_edges = integral_sum(integrals.edge, roi);
    const double inner_edges = integral_sum(integrals.edge, inner);
    const double total_pixels = static_cast<double>(roi.width) * roi.height;
    const double inner_pixels = static_cast<double>(inner.width) * inner.height;
    const double perimeter_pixels = std::max(1.0, total_pixels - inner_pixels);

    features.perimeter_edge_density = std::clamp(
        (total_edges - inner_edges) / perimeter_pixels,
        0.0,
        1.0
    );
    features.interior_edge_density = inner_pixels > 0.0
        ? std::clamp(inner_edges / inner_pixels, 0.0, 1.0)
        : 0.0;

    const cv::Rect top{roi.x, roi.y, roi.width, thickness};
    const cv::Rect bottom{
        roi.x,
        roi.y + std::max(0, roi.height - thickness),
        roi.width,
        thickness,
    };
    const cv::Rect left{roi.x, roi.y, thickness, roi.height};
    const cv::Rect right{
        roi.x + std::max(0, roi.width - thickness),
        roi.y,
        thickness,
        roi.height,
    };
    const double top_support = side_span_coverage(score_edges, top, true);
    const double bottom_support = side_span_coverage(score_edges, bottom, true);
    const double left_support = side_span_coverage(score_edges, left, false);
    const double right_support = side_span_coverage(score_edges, right, false);
    features.side_support =
        (top_support + bottom_support + left_support + right_support) / 4.0;
    features.paired_side_support = 0.5 * (
        std::min(top_support, bottom_support) +
        std::min(left_support, right_support)
    );

    features.boundary_contrast = directional_boundary_contrast(
        integrals.gray,
        roi,
        score_edges.cols,
        score_edges.rows,
        thickness,
        features.contrast_side_support
    );

    const double mean_gray = rect_mean(integrals.gray, roi);
    const double mean_gray_sq = rect_mean(integrals.gray_sq, roi);
    const double variance = std::max(0.0, mean_gray_sq - mean_gray * mean_gray);
    features.luminance_variation = std::clamp(std::sqrt(variance) / 68.0, 0.0, 1.0);
    features.text_band_score = horizontal_text_band_score(
        score_edges,
        inner.width > 0 ? inner : roi
    );

    if (!text_evidence.integral.empty()) {
        features.text_coverage = std::clamp(
            rect_mean(text_evidence.integral, inner.width > 0 ? inner : roi),
            0.0,
            1.0
        );
    }
    int contained_text_lines = 0;
    const cv::Rect text_roi = inner.width > 0 ? inner : roi;
    for (const auto& line : text_evidence.lines) {
        const int left_edge = std::max(text_roi.x, line.x);
        const int top_edge = std::max(text_roi.y, line.y);
        const int right_edge = std::min(text_roi.x + text_roi.width, line.x + line.width);
        const int bottom_edge = std::min(text_roi.y + text_roi.height, line.y + line.height);
        const double overlap = static_cast<double>(
            std::max(0, right_edge - left_edge) *
            std::max(0, bottom_edge - top_edge)
        );
        const double line_area = static_cast<double>(line.width) * line.height;
        if (line_area > 0.0 && overlap / line_area >= 0.72) {
            ++contained_text_lines;
        }
    }
    features.text_line_score = std::clamp(
        static_cast<double>(contained_text_lines) / 5.0,
        0.0,
        1.0
    );
    features.contour_consensus = contour_consensus(roi, structural_rectangles);

    const double frame_area = static_cast<double>(frame_width) * frame_height;
    const double area_fraction = frame_area > 0.0 ? area(region) / frame_area : 0.0;
    const double size_signal = std::clamp(
        (std::sqrt(std::max(0.0, area_fraction)) - 0.08) / 0.30,
        0.0,
        1.0
    );
    const double aspect = region.width / std::max(1.0, region.height);
    // Merely containing several lines of text does not make a panel a text
    // fragment. Weight line count by how much of the candidate those line
    // boxes actually occupy; real cards often contain lots of text with
    // padding, while paragraph-only proposals are dominated by it.
    const double coverage_pressure = std::clamp(
        features.text_coverage / 0.42,
        0.0,
        1.0
    );
    const double line_pressure = features.text_line_score * std::clamp(
        features.text_coverage / 0.20,
        0.0,
        1.0
    );
    features.text_pressure = std::max({
        features.text_band_score,
        coverage_pressure,
        line_pressure,
    });

    const bool strong_border =
        features.side_support >= 0.40 &&
        features.paired_side_support >= 0.25;
    const bool strong_contrast =
        features.boundary_contrast >= 0.25 &&
        features.contrast_side_support >= 0.50;
    const bool contour_agreement = features.contour_consensus >= 0.52;
    const bool media_like =
        area_fraction >= 0.035 &&
        features.luminance_variation >= 0.58 &&
        features.text_pressure < 0.62 &&
        (features.boundary_contrast >= 0.14 || features.contour_consensus >= 0.30);

    features.structural_votes =
        static_cast<int>(strong_border) +
        static_cast<int>(strong_contrast) +
        static_cast<int>(contour_agreement) +
        static_cast<int>(media_like);

    // A real container is allowed to contain text. Convert the independent
    // boundary cues into one confidence value and use that to *attenuate* the
    // text penalty instead of subtracting the full text score unconditionally.
    features.structure_confidence = std::clamp(
        0.24 * std::clamp(features.side_support / 0.50, 0.0, 1.0) +
        0.24 * std::clamp(features.paired_side_support / 0.34, 0.0, 1.0) +
        0.18 * std::clamp(features.boundary_contrast / 0.34, 0.0, 1.0) +
        0.24 * std::clamp(features.contour_consensus / 0.66, 0.0, 1.0) +
        0.10 * std::clamp(
            static_cast<double>(features.structural_votes) / 3.0,
            0.0,
            1.0
        ),
        0.0,
        1.0
    );

    const bool weak_structure = features.structure_confidence < 0.46;
    features.text_like =
        (features.text_pressure >= 0.58 && weak_structure) ||
        (features.text_pressure >= 0.82 &&
            features.structure_confidence < 0.60);

    // Consensus matters more than any single noisy metric. Selective Search is
    // only a proposal source; the candidate must independently look like a
    // bounded UI container before it reaches the overlay. Text is expensive
    // only when the boundary evidence is weak.
    const double text_penalty = features.text_pressure *
        (1.08 - 0.74 * features.structure_confidence);
    features.quality =
        1.15 * features.side_support +
        1.20 * features.paired_side_support +
        1.00 * features.boundary_contrast +
        0.42 * features.contrast_side_support +
        1.05 * features.contour_consensus +
        0.32 * features.luminance_variation +
        0.20 * size_signal +
        0.28 * features.structure_confidence +
        0.18 * static_cast<double>(features.structural_votes) -
        text_penalty;

    if (features.text_like) features.quality -= 1.05;
    if (aspect > 3.0 && features.text_pressure >= 0.42 &&
        features.structure_confidence < 0.58) {
        features.quality -= 0.45;
    }
    return features;
}

std::vector<PixelRegion> detect_regions(
    const std::uint8_t* rgba_data,
    int width,
    int height,
    int stride
) {
    std::vector<PixelRegion> accepted;
    if (rgba_data == nullptr || width <= 0 || height <= 0 || stride < width * 4) {
        return accepted;
    }

    cv::Mat rgba(height, width, CV_8UC4, const_cast<std::uint8_t*>(rgba_data), stride);
    cv::Mat bgr;
    cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
    if (bgr.empty()) return accepted;

    constexpr double proposal_resize_factor = 0.10;
    const int analysis_width = std::max(
        1,
        static_cast<int>(std::round(width * proposal_resize_factor))
    );
    const int analysis_height = std::max(
        1,
        static_cast<int>(std::round(height * proposal_resize_factor))
    );

    cv::Mat analysis;
    cv::resize(
        bgr,
        analysis,
        cv::Size(analysis_width, analysis_height),
        0.0,
        0.0,
        cv::INTER_AREA
    );

    auto ss = cv::ximgproc::segmentation::createSelectiveSearchSegmentation();
    ss->setBaseImage(analysis);
    ss->switchToSelectiveSearchFast(3000, 50, 0.6f);

    std::vector<cv::Rect> proposals;
    ss->process(proposals);

    const double scale_x = static_cast<double>(width) / analysis.cols;
    const double scale_y = static_cast<double>(height) / analysis.rows;

    // Structural validation runs at 25% instead of 20%. The extra pixels make
    // long-side continuity and text-line evidence substantially more stable,
    // while remaining tiny compared with the real framebuffer.
    constexpr double score_resize_factor = 0.25;
    const int score_width = std::max(
        1,
        static_cast<int>(std::round(width * score_resize_factor))
    );
    const int score_height = std::max(
        1,
        static_cast<int>(std::round(height * score_resize_factor))
    );
    cv::Mat score_bgr;
    cv::resize(
        bgr,
        score_bgr,
        cv::Size(score_width, score_height),
        0.0,
        0.0,
        cv::INTER_AREA
    );

    cv::Mat score_gray;
    cv::cvtColor(score_bgr, score_gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred;
    cv::GaussianBlur(score_gray, blurred, cv::Size(3, 3), 0.0);
    cv::Mat score_edges;
    cv::Canny(blurred, score_edges, 42.0, 118.0, 3, true);
    cv::threshold(score_edges, score_edges, 0.0, 1.0, cv::THRESH_BINARY);

    IntegralImages integrals;
    cv::integral(score_edges, integrals.edge, CV_64F);
    cv::integral(
        score_gray,
        integrals.gray,
        integrals.gray_sq,
        CV_64F,
        CV_64F
    );

    const TextEvidence text_evidence = build_text_evidence(score_edges);
    const std::vector<cv::Rect> structural_rectangles =
        build_structural_rectangles(score_edges);

    const double score_scale_x = static_cast<double>(score_width) / width;
    const double score_scale_y = static_cast<double>(score_height) / height;

    std::vector<ScoredRegion> candidates;
    candidates.reserve(std::min<std::size_t>(proposals.size(), 512));
    std::vector<ScoredRegion> rescue_candidates;
    rescue_candidates.reserve(96);

    const double min_w = std::max(180.0, width * 0.080);
    const double min_h = std::max(100.0, height * 0.082);
    const double max_w = width * 0.92;
    const double max_h = height * 0.92;
    const double max_area = static_cast<double>(width) * height * 0.70;

    std::size_t rejected_text = 0;
    std::size_t rejected_structure = 0;
    std::size_t rejected_quality = 0;

    for (const auto& rect : proposals) {
        PixelRegion region{
            .x = rect.x * scale_x,
            .y = rect.y * scale_y,
            .width = rect.width * scale_x,
            .height = rect.height * scale_y,
        };

        region.x = std::clamp(region.x, 0.0, static_cast<double>(width));
        region.y = std::clamp(region.y, 0.0, static_cast<double>(height));
        region.width = std::clamp(region.width, 0.0, static_cast<double>(width) - region.x);
        region.height = std::clamp(region.height, 0.0, static_cast<double>(height) - region.y);

        const double region_area = area(region);
        if (region.width < min_w || region.height < min_h) continue;
        if (region.width > max_w || region.height > max_h) continue;
        if (region_area <= 0.0 || region_area > max_area) continue;

        const double aspect = region.width / std::max(1.0, region.height);
        if (aspect < 0.38 || aspect > 4.0) continue;

        RegionFeatures features = score_region(
            region,
            width,
            height,
            score_edges,
            integrals,
            text_evidence,
            structural_rectangles,
            score_scale_x,
            score_scale_y
        );
        if (features.text_like) {
            ++rejected_text;
            continue;
        }

        const bool exceptional_single_cue =
            features.paired_side_support >= 0.46 ||
            (features.contour_consensus >= 0.68 && features.side_support >= 0.30) ||
            (features.boundary_contrast >= 0.42 &&
                features.contrast_side_support >= 0.60) ||
            (features.structure_confidence >= 0.72 &&
                features.text_pressure < 0.76);

        const bool rescue_eligible =
            !features.text_like &&
            features.structure_confidence >= 0.46 &&
            features.quality >= 0.56 &&
            (features.text_pressure < 0.64 ||
                features.structure_confidence >= 0.70);

        if (features.structural_votes < 2 && !exceptional_single_cue) {
            ++rejected_structure;
            if (rescue_eligible) {
                rescue_candidates.push_back(ScoredRegion{
                    .rect = region,
                    .features = features,
                });
            }
            continue;
        }
        if (features.quality < 0.84) {
            ++rejected_quality;
            if (rescue_eligible) {
                rescue_candidates.push_back(ScoredRegion{
                    .rect = region,
                    .features = features,
                });
            }
            continue;
        }

        candidates.push_back(ScoredRegion{
            .rect = region,
            .features = features,
        });
        if (candidates.size() >= 280) break;
    }

    // Precision-first adaptive fallback: if strict consensus starves the
    // recognizer, promote only a handful of low-text, medium-confidence
    // candidates. This recovers useful cards on borderless modern UIs without
    // reopening the floodgates for paragraph-shaped Selective Search noise.
    if (candidates.size() < 2 && !rescue_candidates.empty()) {
        std::stable_sort(
            rescue_candidates.begin(),
            rescue_candidates.end(),
            [](const ScoredRegion& a, const ScoredRegion& b) {
                if (std::abs(
                        a.features.structure_confidence -
                        b.features.structure_confidence
                    ) > 0.025) {
                    return a.features.structure_confidence >
                        b.features.structure_confidence;
                }
                return a.features.quality > b.features.quality;
            }
        );

        for (const auto& rescue : rescue_candidates) {
            bool near_existing = false;
            for (const auto& existing : candidates) {
                if (iou(rescue.rect, existing.rect) >= 0.68) {
                    near_existing = true;
                    break;
                }
            }
            if (!near_existing) {
                ScoredRegion promoted = rescue;
                promoted.rescued = true;
                candidates.push_back(std::move(promoted));
            }
            if (candidates.size() >= 4) break;
        }
    }

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const ScoredRegion& a, const ScoredRegion& b) {
            if (a.features.structural_votes != b.features.structural_votes) {
                return a.features.structural_votes > b.features.structural_votes;
            }
            if (std::abs(a.features.quality - b.features.quality) > 0.025) {
                return a.features.quality > b.features.quality;
            }
            const double area_a = area(a.rect);
            const double area_b = area(b.rect);
            if (std::abs(area_a - area_b) > 0.5) return area_a > area_b;
            if (std::abs(a.rect.y - b.rect.y) > 0.5) return a.rect.y < b.rect.y;
            return a.rect.x < b.rect.x;
        }
    );

    std::vector<ScoredRegion> kept;
    kept.reserve(24);
    for (const auto& candidate : candidates) {
        bool duplicate = false;
        for (const auto& keep : kept) {
            const double overlap = iou(candidate.rect, keep.rect);
            const double candidate_area = area(candidate.rect);
            const double keep_area = area(keep.rect);
            const double smaller = std::min(candidate_area, keep_area);
            const double larger = std::max(candidate_area, keep_area);
            const double size_similarity = larger > 0.0 ? smaller / larger : 0.0;
            const double containment = std::max(
                contained_fraction(candidate.rect, keep.rect),
                contained_fraction(keep.rect, candidate.rect)
            );

            // Keep genuine nesting only when the child is meaningfully smaller.
            // This removes the clouds of almost-the-same rectangles Selective
            // Search tends to emit around text blocks and panels.
            if (overlap >= 0.70 ||
                (containment >= 0.94 && size_similarity >= 0.60)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) kept.push_back(candidate);
        if (kept.size() >= 24) break;
    }

    accepted.reserve(kept.size());
    for (const auto& keep : kept) accepted.push_back(keep.rect);

    std::stable_sort(
        accepted.begin(),
        accepted.end(),
        [](const PixelRegion& a, const PixelRegion& b) {
            const double area_a = area(a);
            const double area_b = area(b);
            if (std::abs(area_a - area_b) > 0.5) return area_a < area_b;
            if (std::abs(a.y - b.y) > 0.5) return a.y < b.y;
            return a.x < b.x;
        }
    );

    if (std::getenv("REALMHEART_SCREENSHOT_REGION_DEBUG") != nullptr) {
        std::cerr << "[Screenshot regions] proposals=" << proposals.size()
                  << " structural=" << structural_rectangles.size()
                  << " text-lines=" << text_evidence.lines.size()
                  << " rejected-text=" << rejected_text
                  << " rejected-structure=" << rejected_structure
                  << " rejected-quality=" << rejected_quality
                  << " accepted=" << kept.size() << '\n';
        for (const auto& keep : kept) {
            const auto& f = keep.features;
            std::cerr << "  q=" << f.quality
                      << " votes=" << f.structural_votes
                      << " side=" << f.side_support
                      << " paired=" << f.paired_side_support
                      << " contrast=" << f.boundary_contrast
                      << " contour=" << f.contour_consensus
                      << " structure=" << f.structure_confidence
                      << " text=" << f.text_pressure
                      << " source=" << (keep.rescued ? "rescue" : "strict")
                      << " rect=" << keep.rect.x << ',' << keep.rect.y
                      << ' ' << keep.rect.width << 'x' << keep.rect.height
                      << '\n';
        }
    }

    return accepted;
}

int parse_frame_fd(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view{argv[index]} != "--frame-fd") continue;
        try {
            return std::stoi(argv[index + 1]);
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

} // namespace

int main(int argc, char** argv) {
    const auto started = Clock::now();
    const int frame_fd = parse_frame_fd(argc, argv);
    if (frame_fd < 0) {
        std::cerr << "region worker: missing --frame-fd\n";
        return 2;
    }

    struct stat stat_buffer {};
    if (::fstat(frame_fd, &stat_buffer) != 0 ||
        stat_buffer.st_size < static_cast<off_t>(sizeof(SharedFrameHeader))) {
        std::cerr << "region worker: invalid frame fd\n";
        return 3;
    }

    const std::size_t mapped_size = static_cast<std::size_t>(stat_buffer.st_size);
    void* mapped = ::mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE, frame_fd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "region worker: mmap failed\n";
        return 4;
    }

    const auto* header = static_cast<const SharedFrameHeader*>(mapped);
    const std::size_t payload_capacity = mapped_size - sizeof(SharedFrameHeader);
    const bool valid =
        header->magic == kFrameMagic &&
        header->version == kFrameVersion &&
        header->width > 0 &&
        header->height > 0 &&
        header->stride >= header->width * 4 &&
        header->byte_size > 0 &&
        header->byte_size <= payload_capacity &&
        header->byte_size >= static_cast<std::uint64_t>(header->stride) *
            static_cast<std::uint64_t>(header->height);

    if (!valid) {
        ::munmap(mapped, mapped_size);
        std::cerr << "region worker: malformed shared frame\n";
        return 5;
    }

    const auto* rgba = reinterpret_cast<const std::uint8_t*>(header + 1);
    const auto regions = detect_regions(
        rgba,
        header->width,
        header->height,
        header->stride
    );

    for (const auto& region : regions) {
        std::cout << region.x << '\t'
                  << region.y << '\t'
                  << region.width << '\t'
                  << region.height << '\n';
    }

    ::munmap(mapped, mapped_size);

    if (std::getenv("REALMHEART_SCREENSHOT_TIMING") != nullptr) {
        const double elapsed = std::chrono::duration<double, std::milli>(
            Clock::now() - started
        ).count();
        std::cerr << "[Screenshot timing] region worker total "
                  << elapsed << " ms\n";
    }
    return 0;
}
