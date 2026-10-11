#!/usr/bin/env python3
"""Measure decoded-frame cost for the live wallpaper decoder.

The renderer plan asks for repeatable measurements rather than guessed budgets,
so this exists to answer one question with data: how many decode threads does a
wallpaper actually need? It decodes the same video with several thread counts
and reports wall time, CPU time, and CPU cost per frame.

Usage:
  python3 tools/display-tests/bench_video_decode.py [video] [--runs N]
"""
from __future__ import annotations

import argparse
import os
import resource
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]

SOURCE = r"""
#include <sys/resource.h>
#include <sys/time.h>

#include <chrono>

// FFmpeg's public headers have no extern "C" guards; without this block every
// symbol mangles and the link fails.
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double cpu_seconds() {
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

int main(int argc, char** argv) {
    const std::string path = argv[1];
    const int threads = std::atoi(argv[2]);
    const int expected_frames = std::atoi(argv[3]);

    AVFormatContext* format = avformat_alloc_context();
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return 2;
    if (avformat_find_stream_info(format, nullptr) < 0) return 3;
    const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) return 4;
    AVStream* stream = format->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) return 5;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, stream->codecpar);
    ctx->thread_count = threads;

    const double cpu_before = cpu_seconds();
    const auto wall_before = std::chrono::steady_clock::now();
    if (avcodec_open2(ctx, codec, nullptr) < 0) return 6;

    SwsContext* scaler = nullptr;
    AVFrame* raw = av_frame_alloc();
    AVFrame* rgba = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    int frames = 0;

    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream_index &&
            avcodec_send_packet(ctx, packet) >= 0) {
            while (avcodec_receive_frame(ctx, raw) >= 0) {
                if (raw->width <= 0 || raw->height <= 0) continue;
                if (scaler == nullptr || rgba->width != raw->width ||
                    rgba->height != raw->height) {
                    if (scaler != nullptr) sws_freeContext(scaler);
                    scaler = sws_getContext(raw->width, raw->height,
                        (AVPixelFormat)raw->format, raw->width, raw->height,
                        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    av_frame_unref(rgba);
                    rgba->width = raw->width;
                    rgba->height = raw->height;
                    rgba->format = AV_PIX_FMT_RGBA;
                    av_frame_get_buffer(rgba, 0);
                }
                if (av_frame_make_writable(rgba) < 0) return 7;
                sws_scale(scaler, raw->data, raw->linesize, 0, raw->height,
                          rgba->data, rgba->linesize);
                ++frames;
            }
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, raw) >= 0) ++frames;

    const double wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_before).count();
    const double cpu = cpu_seconds() - cpu_before;

    std::printf("%d %d %.4f %.4f\n", threads, frames, wall, cpu);
    av_packet_free(&packet);
    av_frame_free(&raw);
    av_frame_free(&rgba);
    if (scaler != nullptr) sws_freeContext(scaler);
    avcodec_free_context(&ctx);
    avformat_close_input(&format);
    (void)expected_frames;
    return 0;
}
"""


def build_probe() -> Path:
    out = Path("/tmp/realmheart-bench")
    out.mkdir(parents=True, exist_ok=True)
    source = out / "probe.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    binary = out / "probe"
    command = [
        "c++", "-std=c++20", "-O2", str(source), "-o", str(binary),
        *subprocess.run(
            ["pkg-config", "--cflags", "--libs",
             "libavformat", "libavcodec", "libavutil", "libswscale"],
            capture_output=True, text=True, check=True,
        ).stdout.split(),
    ]
    subprocess.run(command, check=True, capture_output=True)
    return binary


def main() -> int:
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("video", nargs="?",
                        default="/home/zahed/Pictures/Wallpapers/Test.mp4")
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()

    video = Path(args.video)
    if not video.is_file():
        print(f"[bench] video not found: {video}", file=sys.stderr)
        return 1

    probe = build_probe()
    print(f"[bench] video={video.name}")
    print(f"{'threads':>7} {'frames':>6} {'wall s':>8} {'cpu s':>8} "
          f"{'cpu/frame ms':>13} {'wall/frame ms':>14}")

    for threads in (1, 2, 4, 0):
        results = []
        for _ in range(args.runs):
            out = subprocess.run(
                [str(probe), str(video), str(threads), "0"],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
            _, frames, wall, cpu = out.split()
            results.append((int(frames), float(wall), float(cpu)))
        frames = max(r[0] for r in results)
        wall = min(r[1] for r in results)
        cpu = min(r[2] for r in results)
        label = "auto" if threads == 0 else str(threads)
        print(f"{label:>7} {frames:>6} {wall:>8.4f} {cpu:>8.4f} "
              f"{cpu / max(frames, 1) * 1000:>13.3f} "
              f"{wall / max(frames, 1) * 1000:>14.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
