#pragma once

// BunnyPBR 性能测试模块（header-only）
// 通过 main 参数 `-Test` 启用，测试结束后导出：
//   - PerfReport_C{count}.md      配置信息 + 常规性能统计（帧时间 / FPS / 百分位 / 分布）
//   - PerfFrameTimes_C{count}.csv 逐帧原始耗时，便于外部工具进一步分析
//
// 与 TextureCube 的差异：本样例的性能轴是「兔子数量」（实例数），由 `-C X` 指定，
// 不再使用 TextureCube 的 `-DC`（绘制调用数）/ `-INS`（实例数）两个独立维度。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace FISIR {

    // 测试配置（最终写入报告的配置信息部分）
    struct PerfConfig {
        uint32_t bunnyCount        = 0;   // 兔子数量（实例数）
        uint32_t trianglesPerBunny = 0;   // 每只兔子三角形数（含兜底球体）
        uint32_t dispatchGroupX    = 0;   // compute dispatch 组数 X
        uint32_t offscreenWidth    = 0;
        uint32_t offscreenHeight   = 0;
        uint32_t swapchainWidth    = 0;
        uint32_t swapchainHeight   = 0;
        std::string presentMode    = "Mailbox";
        uint32_t renderPassesPerFrame = 2;   // 离屏 + 呈现
        uint64_t totalFrames       = 0;
        double   totalSeconds      = 0.0;
        uint64_t warmupFrames      = 0;    // 已从统计中剔除的前 N 帧（预热）
    };

    // 统计结果
    struct PerfStats {
        double avgMs, minMs, maxMs;
        double medianMs;    // P50
        double p95Ms;
        double p99Ms;       // 1% low（帧时间的 99 分位）
        double p999Ms;      // 0.1% low（帧时间的 99.9 分位）
        double stddevMs;
        double avgFps;
        double fps1Low;     // 1000 / P99
        double fps01Low;    // 1000 / P99.9
    };

    // 线性插值百分位（sorted 必须已升序排列）
    inline double percentile(const std::vector<double>& sorted, double p) {
        if (sorted.empty()) return 0.0;
        double idx = p * static_cast<double>(sorted.size() - 1);
        size_t lo = static_cast<size_t>(std::floor(idx));
        size_t hi = static_cast<size_t>(std::ceil(idx));
        if (lo == hi) return sorted[lo];
        double frac = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }

    inline PerfStats computeStats(const std::vector<double>& frameMs) {
        PerfStats s{};
        if (frameMs.empty()) return s;
        size_t n = frameMs.size();

        double sum = std::accumulate(frameMs.begin(), frameMs.end(), 0.0);
        s.avgMs = sum / static_cast<double>(n);

        std::vector<double> sorted = frameMs;
        std::sort(sorted.begin(), sorted.end());
        s.minMs    = sorted.front();
        s.maxMs    = sorted.back();
        s.medianMs = percentile(sorted, 0.50);
        s.p95Ms    = percentile(sorted, 0.95);
        s.p99Ms    = percentile(sorted, 0.99);
        s.p999Ms   = percentile(sorted, 0.999);

        double var = 0.0;
        for (double v : frameMs) { double d = v - s.avgMs; var += d * d; }
        s.stddevMs = std::sqrt(var / static_cast<double>(n));

        s.avgFps   = 1000.0 / s.avgMs;
        s.fps1Low  = 1000.0 / s.p99Ms;
        s.fps01Low = 1000.0 / s.p999Ms;
        return s;
    }

    // 帧时间分布（按毫秒区间统计，用于观察抖动/卡顿来源）
    inline std::vector<std::pair<std::string, size_t>> histogram(const std::vector<double>& frameMs) {
        static const double bounds[] = { 0.0, 1.0, 2.0, 4.0, 8.0, 16.0, 33.33, 66.67, 1e9 };
        static const char*  labels[] = { "<1ms", "1-2ms", "2-4ms", "4-8ms", "8-16ms", "16-33ms", "33-66ms", ">66ms" };
        std::vector<size_t> cnt(8, 0);
        for (double v : frameMs) {
            for (int i = 0; i < 8; ++i) {
                if (v >= bounds[i] && v < bounds[i + 1]) { cnt[i]++; break; }
            }
        }
        std::vector<std::pair<std::string, size_t>> out;
        for (int i = 0; i < 8; ++i) out.emplace_back(labels[i], cnt[i]);
        return out;
    }

    inline void writePerformanceReport(const PerfConfig& cfg,
                                       const std::vector<double>& frameMs,
                                       const std::string& mdPath,
                                       const std::string& csvPath) {
        PerfStats s = computeStats(frameMs);
        size_t n = frameMs.size();
        uint64_t totalTriangles = static_cast<uint64_t>(cfg.bunnyCount) * cfg.trianglesPerBunny;

        // ── Markdown 报告 ─────────────────────────────────────
        std::ofstream md(mdPath);
        if (md.is_open()) {
            md << std::fixed << std::setprecision(3);
            md << "# FISIR-RHI BunnyPBR 性能测试报告\n\n";

            md << "## 1. 测试配置\n\n";
            md << "| 参数 | 值 |\n|---|---|\n";
            md << "| RHI 后端 | Vulkan (RHIVK.dll) |\n";
            md << "| 兔子数量（实例数） | " << cfg.bunnyCount << " |\n";
            md << "| 每只兔子三角形数 | " << cfg.trianglesPerBunny << " |\n";
            md << "| 总三角形数 | " << totalTriangles << " |\n";
            md << "| compute dispatch 组数 | " << cfg.dispatchGroupX << " |\n";
            md << "| 离屏渲染分辨率 | " << cfg.offscreenWidth << " x " << cfg.offscreenHeight << " |\n";
            md << "| 交换链分辨率 | " << cfg.swapchainWidth << " x " << cfg.swapchainHeight << " |\n";
            md << "| 呈现模式 | " << cfg.presentMode << " |\n";
            md << "| 每帧渲染通道 | " << cfg.renderPassesPerFrame << " (离屏 + 呈现) |\n";
            md << "| 测试帧数 | " << cfg.totalFrames << " |\n";
            md << "| 预热剔除帧数 | " << cfg.warmupFrames << " |\n";
            md << "| 总耗时 | " << std::setprecision(3) << cfg.totalSeconds << " s |\n\n";

            md << "## 2. 帧时间统计（CPU 侧，含提交等待）\n\n";
            md << "| 指标 | 值 |\n|---|---|\n";
            md << "| 平均帧时间 | " << s.avgMs << " ms |\n";
            md << "| 最小帧时间 | " << s.minMs << " ms |\n";
            md << "| 最大帧时间 | " << s.maxMs << " ms |\n";
            md << "| 中位数 (P50) | " << s.medianMs << " ms |\n";
            md << "| P95 | " << s.p95Ms << " ms |\n";
            md << "| P99 (1% low) | " << s.p99Ms << " ms |\n";
            md << "| P99.9 (0.1% low) | " << s.p999Ms << " ms |\n";
            md << "| 标准差 | " << s.stddevMs << " ms |\n\n";

            md << "## 3. FPS 统计\n\n";
            md << "| 指标 | 值 |\n|---|---|\n";
            md << "| 平均 FPS | " << std::setprecision(1) << s.avgFps << " |\n";
            md << "| 1% low FPS | " << s.fps1Low << " |\n";
            md << "| 0.1% low FPS | " << s.fps01Low << " |\n\n";

            md << "## 4. 帧时间分布\n\n";
            md << "| 区间 | 帧数 | 占比 |\n|---|---|---|\n";
            for (auto& [label, cnt] : histogram(frameMs)) {
                double ratio = n ? 100.0 * static_cast<double>(cnt) / static_cast<double>(n) : 0.0;
                md << "| " << label << " | " << cnt << " | " << std::setprecision(2) << ratio << "% |\n";
            }
            md << "\n";
            md << "> 注：MAILBOX 呈现模式无垂直同步，测得 FPS 为原始吞吐而非屏幕刷新率锁定值。\n";
            md << "> 当前 RHI 尚未暴露 GPU 时间戳查询，故仅统计 CPU 侧帧时间（含 acquire 等待与提交确认）。\n";
            md << "> 仿真在每帧由 computeFence->wait() 串行化，故帧时间包含 CPU 等待 compute 完成的开销。\n";
            md.close();
        }

        // ── CSV 逐帧数据 ─────────────────────────────────────
        std::ofstream csv(csvPath);
        if (csv.is_open()) {
            csv << "frame_index,frame_time_ms\n";
            csv << std::fixed << std::setprecision(4);
            for (size_t i = 0; i < frameMs.size(); ++i) {
                csv << i << "," << frameMs[i] << "\n";
            }
            csv.close();
        }
    }

} // namespace FISIR
