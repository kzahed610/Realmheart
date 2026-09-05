#pragma once

#include "screenshot/MonitorResolver.hpp"
#include "screenshot/SemanticRegions.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/ximgproc/segmentation.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::screenshot {

class ContentRegionDetector {
public:
    static void append_detected_regions(
        SemanticRegionSnapshot& snapshot,
        const MonitorTarget& monitor,
        const FrozenFrame& frame
    ) {
        if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 || frame.rgba.empty()) {
            return;
        }

        if (snapshot.monitor_width <= 0.0 || snapshot.monitor_height <= 0.0) {
            snapshot.monitor_width = monitor.logical_width > 0.0
                ? monitor.logical_width
                : static_cast<double>(frame.width);
            snapshot.monitor_height = monitor.logical_height > 0.0
                ? monitor.logical_height
                : static_cast<double>(frame.height);
        }

        auto detected = detect_frame_regions(frame);
        if (detected.empty()) {
            if (!snapshot.available) {
                snapshot.available = false;
            }
            return;
        }

        const double logical_scale_x = snapshot.monitor_width / static_cast<double>(frame.width);
        const double logical_scale_y = snapshot.monitor_height / static_cast<double>(frame.height);

        std::size_t added = 0;
        for (const auto& rect_px : detected) {
            const SelectionRect logical_rect{
                .x = rect_px.x * logical_scale_x,
                .y = rect_px.y * logical_scale_y,
                .width = rect_px.width * logical_scale_x,
                .height = rect_px.height * logical_scale_y,
            };
            if (reject_as_window_duplicate(snapshot, logical_rect)) continue;

            snapshot.regions.push_back(SemanticRegion{
                .rect = logical_rect,
                .source = SemanticRegionSource::Content,
                .label = "Region",
                .priority = 2,
                .focus_history_id = -1,
            });
            ++added;
        }

        if (added == 0) return;
        snapshot.available = true;
        sort_regions(snapshot.regions);

        if (std::getenv("REALMHEART_SCREENSHOT_DEBUG") != nullptr) {
            std::cout << "[Screenshot] Content detector added " << added << " region(s)\n";
        }
    }

private:
    struct PixelRegion {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    static std::vector<PixelRegion> detect_frame_regions(const FrozenFrame& frame) {
        std::vector<PixelRegion> accepted;

        cv::Mat rgba(frame.height, frame.width, CV_8UC4, const_cast<std::uint8_t*>(frame.rgba.data()), frame.stride);
        if (rgba.empty()) return accepted;

        cv::Mat bgr;
        cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
        if (bgr.empty()) return accepted;

        constexpr double resize_factor = 0.10;
        const int analysis_width = std::max(1, static_cast<int>(std::round(bgr.cols * resize_factor)));
        const int analysis_height = std::max(1, static_cast<int>(std::round(bgr.rows * resize_factor)));

        cv::Mat analysis;
        cv::resize(bgr, analysis, cv::Size(analysis_width, analysis_height), 0.0, 0.0, cv::INTER_AREA);

        auto ss = cv::ximgproc::segmentation::createSelectiveSearchSegmentation();
        ss->setBaseImage(analysis);
        ss->switchToSelectiveSearchFast(3000, 50, 0.6f);

        std::vector<cv::Rect> proposals;
        ss->process(proposals);

        const double scale_x = static_cast<double>(frame.width) / static_cast<double>(analysis.cols);
        const double scale_y = static_cast<double>(frame.height) / static_cast<double>(analysis.rows);

        std::vector<PixelRegion> candidates;
        candidates.reserve(std::min<std::size_t>(proposals.size(), 512));

        const double min_w = std::max(160.0, frame.width * 0.07);
        const double min_h = std::max(90.0, frame.height * 0.07);
        const double max_w = frame.width * 0.92;
        const double max_h = frame.height * 0.92;
        const double max_area = frame.width * frame.height * 0.70;

        for (const auto& rect : proposals) {
            PixelRegion region{
                .x = rect.x * scale_x,
                .y = rect.y * scale_y,
                .width = rect.width * scale_x,
                .height = rect.height * scale_y,
            };

            region.x = std::clamp(region.x, 0.0, static_cast<double>(frame.width));
            region.y = std::clamp(region.y, 0.0, static_cast<double>(frame.height));
            region.width = std::clamp(region.width, 0.0, static_cast<double>(frame.width) - region.x);
            region.height = std::clamp(region.height, 0.0, static_cast<double>(frame.height) - region.y);

            const double area = region.width * region.height;
            if (region.width < min_w || region.height < min_h) continue;
            if (region.width > max_w || region.height > max_h) continue;
            if (area <= 0.0 || area > max_area) continue;

            const double aspect = region.width / std::max(1.0, region.height);
            if (aspect < 0.35 || aspect > 4.5) continue;

            candidates.push_back(region);
            if (candidates.size() >= 700) break;
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const PixelRegion& a, const PixelRegion& b) {
            const double area_a = a.width * a.height;
            const double area_b = b.width * b.height;
            if (std::abs(area_a - area_b) > 0.5) return area_a < area_b;
            if (std::abs(a.y - b.y) > 0.5) return a.y < b.y;
            return a.x < b.x;
        });

        for (const auto& candidate : candidates) {
            bool overlaps = false;
            for (const auto& keep : accepted) {
                if (iou(candidate, keep) >= 0.70 || contained_fraction(candidate, keep) >= 0.92) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) accepted.push_back(candidate);
            if (accepted.size() >= 80) break;
        }

        return accepted;
    }

    static double area(const PixelRegion& rect) {
        return std::max(0.0, rect.width) * std::max(0.0, rect.height);
    }

    static double intersection_area(const PixelRegion& a, const PixelRegion& b) {
        const double left = std::max(a.x, b.x);
        const double top = std::max(a.y, b.y);
        const double right = std::min(a.x + a.width, b.x + b.width);
        const double bottom = std::min(a.y + a.height, b.y + b.height);
        return std::max(0.0, right - left) * std::max(0.0, bottom - top);
    }

    static double iou(const PixelRegion& a, const PixelRegion& b) {
        const double inter = intersection_area(a, b);
        if (inter <= 0.0) return 0.0;
        const double union_area = area(a) + area(b) - inter;
        if (union_area <= std::numeric_limits<double>::epsilon()) return 0.0;
        return inter / union_area;
    }

    static double contained_fraction(const PixelRegion& inner, const PixelRegion& outer) {
        const double inter = intersection_area(inner, outer);
        const double inner_area = area(inner);
        if (inner_area <= std::numeric_limits<double>::epsilon()) return 0.0;
        return inter / inner_area;
    }

    static double intersection_area(const SelectionRect& a, const SelectionRect& b) {
        const double left = std::max(a.x, b.x);
        const double top = std::max(a.y, b.y);
        const double right = std::min(a.x + a.width, b.x + b.width);
        const double bottom = std::min(a.y + a.height, b.y + b.height);
        return std::max(0.0, right - left) * std::max(0.0, bottom - top);
    }

    static bool reject_as_window_duplicate(
        const SemanticRegionSnapshot& snapshot,
        const SelectionRect& candidate
    ) {
        const double candidate_area = candidate.width * candidate.height;
        if (candidate_area <= std::numeric_limits<double>::epsilon()) return true;

        for (const auto& region : snapshot.regions) {
            if (region.source != SemanticRegionSource::Window) continue;
            const double inter = intersection_area(candidate, region.rect);
            if (inter <= 0.0) continue;

            const double region_area = region.rect.width * region.rect.height;
            if (region_area <= std::numeric_limits<double>::epsilon()) continue;

            const double candidate_inside_window = inter / candidate_area;
            const double window_covered_by_candidate = inter / region_area;

            if (candidate_inside_window >= 0.96 && window_covered_by_candidate >= 0.82) {
                return true;
            }
        }
        return false;
    }

    static int source_rank(SemanticRegionSource source) {
        switch (source) {
            case SemanticRegionSource::Content:
                return 0;
            case SemanticRegionSource::Layer:
                return 1;
            case SemanticRegionSource::Window:
            default:
                return 2;
        }
    }

    static void sort_regions(std::vector<SemanticRegion>& regions) {
        std::stable_sort(regions.begin(), regions.end(), [](const SemanticRegion& left, const SemanticRegion& right) {
            const int source_left = source_rank(left.source);
            const int source_right = source_rank(right.source);
            if (source_left != source_right) return source_left < source_right;
            if (left.priority != right.priority) return left.priority < right.priority;
            if (left.source == SemanticRegionSource::Window && right.source == SemanticRegionSource::Window &&
                left.focus_history_id != right.focus_history_id) {
                return left.focus_history_id < right.focus_history_id;
            }
            const double left_area = left.rect.width * left.rect.height;
            const double right_area = right.rect.width * right.rect.height;
            if (std::abs(left_area - right_area) > 0.5) return left_area < right_area;
            return left.label < right.label;
        });
    }
};

} // namespace realmheart::screenshot
