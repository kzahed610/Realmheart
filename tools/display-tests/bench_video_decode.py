#!/usr/bin/env python3
"""Measure decoded-frame cost for the live wallpaper decoder.

The renderer plan asks for repeatable measurements rather than guessed budgets,
so this exists to answer questions with data. It decodes the same video with
several thread counts, and with hardware decode enabled, reporting wall time,
CPU time, and cost per frame.

What it established on the 2026-10-12 baseline (1920x1080 H.264, 150 frames,
i5-11300H / Iris Xe, Mesa 26.2):

    path    frames   cpu/frame   wall/frame
    sw:1       150      19.4ms        19.7ms
    sw:2       150      23.3ms        14.2ms
    hw:2       150      19.6ms        19.8ms
    sw:auto    150      33.9ms         6.5ms

Two conclusions, both counter-intuitive enough that they are worth writing down:

- Threading raises CPU cost monotonically. What it buys is wall time the
  wallpaper never needed, because the decoder only has to sustain the media's
  own frame rate into a three-frame queue. This is why kDecoderThreadCount is 2.
- Hardware decode through a CPU transfer is not a saving at all: 19.6ms against
  19.4ms for single-threaded software. The decode was never the dominant cost —
  the transfer back to the CPU and the RGB conversion are. A hardware path only
  pays off when the frame is sampled on the GPU, which is what the DMA-BUF /
  EGLImage importer has to do; until then the middle path is omitted on purpose,
  as the renderer plan allows.

It also measures the pixel path, because that is where the surprise was: decode is
not the dominant cost, and converting to RGBA on the CPU was. With the whole
decoder in the loop, so these numbers include decode (1920x1080 H.264, full
length, i5-11300H, --runs 2, 2026-10-13):

    path   cpu/frame   upload bytes/frame
    rgba      27.9ms                8294400
    nv12      23.5ms                3110400

The conversion and the copy are 4.4 ms/frame cheaper in NV12, and the upload is
2.7x smaller, because the colour conversion now runs in the fragment shader
instead of on the CPU. That is why the renderer converts to NV12 on the worker
and why DMA-BUF/EGLImage zero-copy matters less than the renderer plan assumed:
the bandwidth it would save is already a third of what it was (renderer plan
phase 12).

Usage:
  python3 tools/display-tests/bench_video_decode.py [video] [--runs N] [--pixels]
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
#include <libavutil/hwcontext.h>

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
    const bool want_hw = argc >= 5 && std::atoi(argv[4]) != 0;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, stream->codecpar);
    ctx->thread_count = threads;
    AVBufferRef* hw_device = nullptr;
    int hw_pix_fmt = -1;
    if (want_hw) {
        for (int i = 0; ; ++i) {
            const AVCodecHWConfig* c = avcodec_get_hw_config(codec, i);
            if (c == nullptr) break;
            if ((c->methods & 1) && c->device_type == AV_HWDEVICE_TYPE_VAAPI) {
                hw_pix_fmt = c->pix_fmt;
                break;
            }
        }
        if (hw_pix_fmt >= 0 &&
            av_hwdevice_ctx_create(&hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) == 0) {
            ctx->hw_device_ctx = av_buffer_ref(hw_device);
        } else {
            hw_pix_fmt = -1;
        }
    }
    AVFrame* hw_frame = av_frame_alloc();

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
                AVFrame* use = raw;
                if (hw_pix_fmt >= 0 && raw->format == hw_pix_fmt) {
                    if (av_hwframe_transfer_data(hw_frame, raw, 0) < 0) continue;
                    use = hw_frame;
                }
                if (scaler == nullptr || rgba->width != use->width ||
                    rgba->height != use->height) {
                    if (scaler != nullptr) sws_freeContext(scaler);
                    scaler = sws_getContext(use->width, use->height,
                        (AVPixelFormat)use->format, use->width, use->height,
                        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    av_frame_unref(rgba);
                    rgba->width = use->width;
                    rgba->height = use->height;
                    rgba->format = AV_PIX_FMT_RGBA;
                    av_frame_get_buffer(rgba, 0);
                }
                if (av_frame_make_writable(rgba) < 0) return 7;
                sws_scale(scaler, use->data, use->linesize, 0, use->height,
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
    av_frame_free(&hw_frame);
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



PIXELS_SOURCE = r"""
#include <sys/resource.h>
#include <sys/time.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cstdint>
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
    const std::string mode = argv[2];
    const bool nv12 = mode == "nv12";

    AVFormatContext* format = avformat_alloc_context();
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return 2;
    if (avformat_find_stream_info(format, nullptr) < 0) return 3;
    const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVCodecContext* codec = avcodec_alloc_context3(nullptr);
    avcodec_parameters_to_context(codec, format->streams[stream_index]->codecpar);
    codec->thread_count = 2;
    const AVCodec* decoder = avcodec_find_decoder(codec->codec_id);
    if (avcodec_open2(codec, decoder, nullptr) < 0) return 4;

    SwsContext* scaler = nullptr;
    AVFrame* converted = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    std::vector<unsigned char> destination;

    std::uint64_t frames = 0;
    std::uint64_t bytes_copied = 0;
    const auto start = std::chrono::steady_clock::now();
    const double cpu_start = cpu_seconds();

    AVFrame* decoded = av_frame_alloc();
    while (av_read_frame(format, packet) >= 0) {
        if (avcodec_send_packet(codec, packet) < 0) { av_packet_unref(packet); continue; }
        while (avcodec_receive_frame(codec, decoded) == 0) {
            if (scaler == nullptr) {
                const auto target = nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_RGBA;
                scaler = sws_getContext(decoded->width, decoded->height,
                                        (AVPixelFormat)decoded->format,
                                        decoded->width, decoded->height, target,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                converted->format = target;
                converted->width = decoded->width;
                converted->height = decoded->height;
                av_frame_get_buffer(converted, 0);
            }
            const int rows = sws_scale(scaler, decoded->data, decoded->linesize, 0,
                                       decoded->height, converted->data,
                                       converted->linesize);
            if (rows <= 0) continue;

            const int width = decoded->width;
            const int height = decoded->height;
            if (nv12) {
                const std::size_t total = (std::size_t)width * height * 3 / 2;
                destination.resize(total);
                for (int row = 0; row < height; ++row)
                    std::memcpy(destination.data() + (std::size_t)row * width,
                                converted->data[0] + (std::size_t)row * converted->linesize[0],
                                width);
                for (int row = 0; row < height / 2; ++row)
                    std::memcpy(destination.data() + (std::size_t)width * height +
                                    (std::size_t)row * width,
                                converted->data[1] + (std::size_t)row * converted->linesize[1],
                                width);
                bytes_copied += total;
            } else {
                const std::size_t total = (std::size_t)width * height * 4;
                destination.resize(total);
                for (int row = 0; row < height; ++row)
                    std::memcpy(destination.data() + (std::size_t)row * width * 4,
                                converted->data[0] + (std::size_t)row * converted->linesize[0],
                                (std::size_t)width * 4);
                bytes_copied += total;
            }
            ++frames;
        }
        av_packet_unref(packet);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double cpu = cpu_seconds() - cpu_start;
    std::printf("%llu %.4f %.4f %llu\n", (unsigned long long)frames, wall, cpu,
                (unsigned long long)(frames ? bytes_copied / frames : 0));
    return 0;
}
"""


def build_probe(source_text: str) -> Path:
    out = Path("/tmp/realmheart-bench")
    out.mkdir(parents=True, exist_ok=True)
    source = out / ("probe.cpp" if source_text is SOURCE else "pixels.cpp")
    source.write_text(source_text, encoding="utf-8")
    binary = out / ("probe" if source_text is SOURCE else "pixels")
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


def measure_pixels(video: Path, probe: Path, runs: int) -> int:
    """What one decoded frame costs to turn into pixels, and to upload them."""
    print(f"{'path':>12} {'cpu/frame ms':>13} {'bytes/frame':>12} {'MB/s':>9}")
    for mode in ("rgba", "nv12"):
        best = None
        for _ in range(runs):
            out = subprocess.run(
                [str(probe), str(video), mode],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
            frames, wall, cpu, per_frame_bytes = out.split()
            candidate = (int(frames), float(cpu), int(per_frame_bytes), float(wall))
            if best is None or candidate[1] < best[1]:
                best = candidate
        frames, cpu, per_frame_bytes, wall = best
        megabytes_per_second = per_frame_bytes / (cpu / max(frames, 1)) / 1e6
        print(f"{mode:>12} {cpu / max(frames, 1) * 1000:>13.3f} "
              f"{per_frame_bytes:>12} {megabytes_per_second:>9.1f}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("video", nargs="?",
                        default="/home/zahed/Pictures/Wallpapers/Test.mp4")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--pixels", action="store_true",
                        help="measure the pixel conversion and upload path "
                             "instead of decode only")
    args = parser.parse_args()

    video = Path(args.video)
    if not video.is_file():
        print(f"[bench] video not found: {video}", file=sys.stderr)
        return 1

    probe = build_probe(SOURCE)
    pixels_probe = build_probe(PIXELS_SOURCE)
    print(f"[bench] video={video.name}")

    if args.pixels:
        return measure_pixels(video, pixels_probe, args.runs)

    print(f"{'path':>10} {'frames':>6} {'wall s':>8} {'cpu s':>8} "
          f"{'cpu/frame ms':>13} {'wall/frame ms':>14}")

    for threads, hardware in ((1, False), (2, False), (2, True), (0, False)):
        results = []
        for _ in range(args.runs):
            out = subprocess.run(
                [str(probe), str(video), str(threads), "0",
                 "1" if hardware else "0"],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
            _, frames, wall, cpu = out.split()
            results.append((int(frames), float(wall), float(cpu)))
        frames = max(r[0] for r in results)
        wall = min(r[1] for r in results)
        cpu = min(r[2] for r in results)
        label = ("hw:" if hardware else "sw:") + ("auto" if threads == 0 else str(threads))
        print(f"{label:>10} {frames:>6} {wall:>8.4f} {cpu:>8.4f} "
              f"{cpu / max(frames, 1) * 1000:>13.3f} "
              f"{wall / max(frames, 1) * 1000:>14.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
