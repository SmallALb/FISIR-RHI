// ════════════════════════════════════════════════════════════════════════════
// RemoteCube —— 「线上渲染」最小示例（PC 渲染 → 手机浏览器观看）
//
//   PC 端：**不开任何窗口**，用 RHI 的 Headless 呈现设备把 TextureCube 场景渲染到离屏纹理；
//          读回像素 → JPEG → 内置 HTTP 服务以 MJPEG 推给局域网里的浏览器。
//   手机端：浏览器打开 http://<PC 的局域网 IP>:1919/ 就能看到实时画面，不用装 App。
//
//   为什么用 Headless：这条路上 RHI 完全不碰 surface/交换链（RHIGetSwapChain 返回 nullptr），
//   渲染目标由示例自备 —— 正是「渲染与呈现解耦」的用法（见 RHIDisplay.h）。
//   画面只经 CPU 读回一次（CopyImageToBuffer），编码用仓库里已有的 stb_image_write（v1.16）。
//
//   端点：
//     GET /             观看页（<img src="/stream.mjpg">，手机直接能看）
//     GET /stream.mjpg  MJPEG 流（multipart/x-mixed-replace，浏览器原生支持）
//     GET /frame.jpg    最新一帧（curl / 自动化验证用）
//     GET /stats        JSON：分辨率 / 帧率 / 码率 / 观看人数
//
//   命令行：
//     -Port 1919      监听端口
//     -W 960 -H 540   渲染分辨率（16:9，手机竖屏看也合适）
//     -Quality 75     JPEG 质量 1..100
//     -Fps 30         目标帧率上限（0 = 不限）
//     -Cubes 9        立方体个数（排成网格）
//     -Seconds 0      跑 N 秒后自动退出（0 = 一直跑；自动化用）
//     -Frames 0       渲染 N 帧后自动退出（0 = 不限）
//
//   局域网访问要点：程序监听 0.0.0.0，但 Windows 防火墙默认会拦入站 —— 首次运行请允许，
//   或以管理员执行（示例启动时会把命令打出来）：
//     netsh advfirewall firewall add rule name="RemoteCube 1919" dir=in action=allow protocol=TCP localport=1919
// ════════════════════════════════════════════════════════════════════════════

// 注意：winsock2.h 必须先于 windows.h；这里也**不要**定义 WIN32_LEAN_AND_MEAN ——
// 它会把 COM 头（IUnknown/IStream）挡掉，而 dxcapi.h（ShaderComplier 依赖）需要它们。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Log/Logger.h"
#include "RHIBuffer.h"
#include "RHICommandList.h"
#include "RHICreator.h"
#include "RHIFence.h"
#include "RHIFrameBuffer.h"
#include "RHIPipeline.h"
#include "RHISampler.h"
#include "RHIShader.h"
#include "RHISwapChain.h"
#include "RHITexture.h"
#include "RHITypes.h"
#include "ShaderComplier.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

namespace {

	// ── 配置 ────────────────────────────────────────────────────────────────
	struct Config {
		uint16_t port = 1919;
		uint32_t width = 960;
		uint32_t height = 540;
		int      quality = 100;
		double   targetFps = 0.0;
		uint32_t cubes = 9;
		double   seconds = 0.0;      // 0 = 一直跑
		uint64_t frames = 0;         // 0 = 不限
		bool     openFirewall = false;   // -OpenFirewall：弹 UAC 自动放行端口
		int      encoderThreads = 0;     // 0 = 自动（min(4, 硬件线程数-1)）
	} g_Cfg;

	std::atomic<bool> g_Stop{ false };

	BOOL WINAPI ConsoleHandler(DWORD type) {
		if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
			g_Stop.store(true);
			return TRUE;
		}
		return FALSE;
	}

	// ── 帧发布：渲染线程写、每个 HTTP 客户端线程读 ───────────────────────────
	// 用 shared_ptr<const vector> 发布，客户端只拿共享所有权，不做逐字节拷贝。
	class FrameHub {
	public:
		void Publish(std::vector<uint8_t>&& jpeg) {
			auto frame = std::make_shared<const std::vector<uint8_t>>(std::move(jpeg));
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Frame = std::move(frame);
				++m_Seq;
			}
			m_Cv.notify_all();
		}

		// 阻塞到出现比 seenSeq 更新的一帧；返回 nullptr 表示超时 / 正在退出。
		std::shared_ptr<const std::vector<uint8_t>> WaitNext(uint64_t& seenSeq, int timeoutMs) {
			std::unique_lock<std::mutex> lock(m_Mutex);
			m_Cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[&] { return m_Seq != seenSeq || g_Stop.load(); });
			if (m_Seq == seenSeq) return nullptr;
			seenSeq = m_Seq;
			return m_Frame;
		}

		std::shared_ptr<const std::vector<uint8_t>> Snapshot() {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_Frame;
		}

	private:
		std::mutex m_Mutex;
		std::condition_variable m_Cv;
		std::shared_ptr<const std::vector<uint8_t>> m_Frame;
		uint64_t m_Seq{ 0 };
	};

	FrameHub g_Hub;

	// ── 统计（/stats 用）─────────────────────────────────────────────────────
	// 放在编码线程池之前：EncoderWorker 要更新这些计数器。
	struct Stats {
		std::atomic<int>      viewers{ 0 };
		std::atomic<double>   fps{ 0.0 };
		std::atomic<double>   mbps{ 0.0 };
		std::atomic<uint64_t> framesSent{ 0 };
		std::atomic<uint64_t> jpegBytes{ 0 };
		std::atomic<uint32_t> jpegSize{ 0 };
	} g_Stats;

	// ════════════════════════════════════════════════════════════════════════
	// 编码线程池（多线程写帧）
	//
	// 为什么：整条链路里 **JPEG 编码是唯一瓶颈**（stb 单线程，960x540 约 120ms/帧，
	// 而渲染 + 读回只有几毫秒）。stb 本身不支持把一张图切给多线程编，所以这里做的是
	// **多帧并行**：渲染线程只负责"渲染 → 读回 → 投递"，N 个编码线程各自完整编一帧。
	//
	// 丢帧策略：显示端永远只关心**最新**一帧，所以队列满了丢最旧的、编码线程也总是取最新的，
	// 编码积压没有任何意义。缓冲用对象池复用，避免每帧 2MB 的分配抖动。
	// ════════════════════════════════════════════════════════════════════════
	struct RawFrame {
		std::vector<uint8_t> rgba;   // 读回缓冲的副本（w*h*4）
		uint64_t seq = 0;
		uint32_t w = 0, h = 0;
	};

	// RGBA 缓冲对象池：Acquire/Release 配对使用。
	class FrameBufferPool {
	public:
		std::vector<uint8_t> Acquire(size_t bytes) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_Free.empty()) {
				auto buf = std::move(m_Free.back());
				m_Free.pop_back();
				if (buf.size() < bytes) buf.resize(bytes);
				return buf;
			}
			return std::vector<uint8_t>(bytes);
		}
		void Release(std::vector<uint8_t>&& buf) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_Free.size() < 8) m_Free.push_back(std::move(buf));
		}
	private:
		std::mutex m_Mutex;
		std::vector<std::vector<uint8_t>> m_Free;
	};

	// 渲染线程 → 编码线程的待编帧队列（有界、丢最旧）—— 用本仓库的 LockFreeQue
	// （Vyukov 有界 MPMC 环：push/pop **非阻塞**、满/空返回 false；只有 pop_wait 会阻塞，
	//   且**只在 stopQue() 之后才返回 false**）。两条铁律：
	//   · 消费者只能用 pop_wait 取帧：绝不能"先 empty() 判断再返回 null"（TOCTOU）——
	//     多个编码线程互抢时，先看到"空"的那个会 break 退出，几轮后编码线程全死光，画面就不动了。
	//   · 生产者的"丢最旧"必须用非阻塞的 pop：用 pop_wait 会在环被消费者清空的瞬间永久阻塞。
	class RawFrameQueue {
	public:
		explicit RawFrameQueue(size_t cap, FrameBufferPool* pool) : m_Cap(cap), m_Pool(pool) {}

		void Push(std::shared_ptr<RawFrame> frame) {
			// 丢最旧（非阻塞）：显示端只要最新一帧，编码积压没有意义。
			while (m_Queue.size() >= m_Cap) {
				std::shared_ptr<RawFrame> dropped;
				if (!m_Queue.pop(dropped)) break;            // 已被消费者取走
				m_Pool->Release(std::move(dropped->rgba));
				m_Dropped.fetch_add(1);
			}
			if (!m_Queue.push(std::move(frame)))             // 环满（cap 远小于 1024，正常轮不到）
				m_Dropped.fetch_add(1);
		}

		// 阻塞取一帧；返回 false = 队列已 stop（收尾由 StopAll() 触发）。
		bool PopWait(std::shared_ptr<RawFrame>& out) { return m_Queue.pop_wait(out); }

		size_t Pending() { return m_Queue.size(); }
		uint64_t Dropped() const { return m_Dropped.load(); }

		// 收尾：置 stop 并唤醒所有等在这条队列上的编码线程。
		void StopAll() { m_Queue.stopQue(); }

	private:
		FISIR::LockFreeQue<std::shared_ptr<RawFrame>,1024> m_Queue;
		size_t m_Cap;
		FrameBufferPool* m_Pool;
		std::atomic<uint64_t> m_Dropped{ 0 };
	};

	FrameBufferPool g_BufferPool;
	RawFrameQueue* g_RawQueue = nullptr;
	std::atomic<uint64_t> g_FrameSeq{ 0 };

	// 一个编码线程：不断从队列取帧 → RGBA→RGB → JPEG → 发布给 HTTP 客户端。
	// 退出条件只有两个：g_Stop（收尾）或队列被 stopQue（PopWait 返回 false）。**不要**因为
	// "这一瞬间队列是空的"就退出 —— 那会让线程提前死掉、画面停更。
	void EncoderWorker() {
		std::vector<uint8_t> rgb;
		std::shared_ptr<RawFrame> frame;
		while (!g_Stop.load() && g_RawQueue->PopWait(frame)) {
			if (!frame) continue;

			const size_t pixels = static_cast<size_t>(frame->w) * frame->h;
			rgb.resize(pixels * 3);
			// stb 的 JPEG 写 comp=3 需要紧凑 RGB；直接传 RGBA 会写出浏览器不认的 4 通道 JPEG。
			const uint8_t* src = frame->rgba.data();
			for (size_t i = 0; i < pixels; ++i) {
				rgb[i * 3 + 0] = src[i * 4 + 0];
				rgb[i * 3 + 1] = src[i * 4 + 1];
				rgb[i * 3 + 2] = src[i * 4 + 2];
			}

			std::vector<uint8_t> jpeg;
			jpeg.reserve(pixels / 4);
			stbi_write_jpg_to_func(
				[](void* ctx, void* data, int size) {
					auto* out = static_cast<std::vector<uint8_t>*>(ctx);
					const uint8_t* p = static_cast<const uint8_t*>(data);
					out->insert(out->end(), p, p + size);
				},
				&jpeg, static_cast<int>(frame->w), static_cast<int>(frame->h), 3, rgb.data(), g_Cfg.quality);

			const size_t jpegBytes = jpeg.size();
			g_Stats.jpegSize.store(static_cast<uint32_t>(jpegBytes));
			g_Stats.framesSent.fetch_add(1);
			g_Stats.jpegBytes.fetch_add(jpegBytes);
			g_Hub.Publish(std::move(jpeg));      // 发布后客户端取到的是最新帧

			g_BufferPool.Release(std::move(frame->rgba));
		}
	}

	// ── 统计（已提前声明到编码线程池之前）───────────────────────────────────
	std::chrono::steady_clock::time_point g_StartTime;

	// ── 极简 HTTP ───────────────────────────────────────────────────────────
	bool SendAll(SOCKET sock, const void* data, size_t len) {
		const char* p = static_cast<const char*>(data);
		while (len > 0) {
			const int chunk = static_cast<int>(len > 1u << 20 ? (1u << 20) : len);
			const int sent = send(sock, p, chunk, 0);
			if (sent <= 0) return false;
			p += sent;
			len -= static_cast<size_t>(sent);
		}
		return true;
	}

	void SendSimple(SOCKET sock, const char* status, const char* contentType, const std::string& body) {
		char head[256];
		const int n = snprintf(head, sizeof(head),
			"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
			"Cache-Control: no-store\r\nConnection: close\r\n\r\n",
			status, contentType, body.size());
		if (SendAll(sock, head, static_cast<size_t>(n)))
			SendAll(sock, body.data(), body.size());
	}

	std::string BuildIndexHtml() {
		return std::string(
			"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
			"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">"
			"<title>FISIR RemoteCube</title><style>"
			"html,body{margin:0;height:100%;background:#0b0b0f;color:#ddd;"
			"font:13px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;overflow:hidden}"
			"#v{width:100vw;height:100vh;object-fit:contain;display:block;background:#000}"
			"#hud{position:fixed;left:10px;top:10px;padding:6px 10px;border-radius:8px;"
			"background:#000a;backdrop-filter:blur(4px);white-space:pre;font-variant-numeric:tabular-nums}"
			"#err{position:fixed;left:0;right:0;bottom:0;padding:10px;background:#811;display:none}"
			"</style></head><body>"
			"<img id=\"v\" src=\"/stream.mjpg\" alt=\"stream\">"
			"<div id=\"hud\">connecting...</div>"
			"<div id=\"err\"></div>"
			"<script>"
			"const hud=document.getElementById('hud');"
			"const img=document.getElementById('v');"
			"let last=performance.now(),frames=0,fps=0;"
			// MJPEG 每来一帧会触发一次 load（浏览器实现一致）
			"img.addEventListener('load',()=>{frames++;const t=performance.now();"
			"if(t-last>=1000){fps=frames*1000/(t-last);frames=0;last=t;}});"
			"img.addEventListener('error',()=>{document.getElementById('err').style.display='block';"
			"document.getElementById('err').textContent='stream error - reconnecting';"
			"setTimeout(()=>{img.src='/stream.mjpg?t='+Date.now();"
			"document.getElementById('err').style.display='none';},1500);});"
			"setInterval(async()=>{try{const r=await fetch('/stats',{cache:'no-store'});"
			"const s=await r.json();"
			// 注意：这里要的是 JS 源码里的 \n 转义（两个字符），所以 C++ 字符串里必须写 \\n；
			// 直接写 \n 会在 JS 字符串字面量中间插入真换行 → 整个 <script> 语法错误（HUD 永远显示 connecting）。
			"hud.textContent=s.width+'x'+s.height+'  server '+s.fps.toFixed(1)+'fps  \\n"
			"+s.mbps.toFixed(2)+' Mbps  jpeg '+s.jpegKB.toFixed(1)+'KB  viewers '+s.viewers+'\\n"
			"+'browser '+fps.toFixed(1)+'fps';}catch(e){hud.textContent='stats unavailable';}},1000);"
			"</script></body></html>");
	}

	void SendStats(SOCKET sock) {
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_StartTime).count();
		char json[512];
		snprintf(json, sizeof(json),
			"{\"width\":%u,\"height\":%u,\"fps\":%.2f,\"mbps\":%.3f,\"viewers\":%d,"
			"\"frames\":%llu,\"jpegKB\":%.1f,\"quality\":%d,\"uptime\":%.1f}",
			g_Cfg.width, g_Cfg.height, g_Stats.fps.load(), g_Stats.mbps.load(), g_Stats.viewers.load(),
			(unsigned long long)g_Stats.framesSent.load(), g_Stats.jpegSize.load() / 1024.0,
			g_Cfg.quality, seconds);
		SendSimple(sock, "200 OK", "application/json", json);
	}

	// 一个连接一个线程：示例规模（手机 + 少数几台机器）足够了。
	void HandleClient(SOCKET sock, std::string peer) {
		// 只读请求行 + 头，够解析 path 就行
		char buf[4096];
		const int n = recv(sock, buf, sizeof(buf) - 1, 0);
		if (n <= 0) { closesocket(sock); return; }
		buf[n] = '\0';

		std::string path = "/";
		if (strncmp(buf, "GET ", 4) == 0) {
			const char* sp = strchr(buf + 4, ' ');
			if (sp) path.assign(buf + 4, sp - (buf + 4));
		}
		// 去掉 query（观看页重连时会带 ?t=...）
		const size_t q = path.find('?');
		if (q != std::string::npos) path.resize(q);

		if (path == "/" || path == "/index.html") {
			SendSimple(sock, "200 OK", "text/html; charset=utf-8", BuildIndexHtml());
		} else if (path == "/stats") {
			SendStats(sock);
		} else if (path == "/frame.jpg") {
			auto frame = g_Hub.Snapshot();
			if (!frame) {
				SendSimple(sock, "503 Service Unavailable", "text/plain", "no frame yet");
			} else {
				char head[256];
				const int hn = snprintf(head, sizeof(head),
					"HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
					"Cache-Control: no-store\r\nConnection: close\r\n\r\n", frame->size());
				if (SendAll(sock, head, static_cast<size_t>(hn)))
					SendAll(sock, frame->data(), frame->size());
			}
		} else if (path == "/stream.mjpg") {
			static const char kHead[] =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
				"Cache-Control: no-store, no-cache, must-revalidate\r\n"
				"Pragma: no-cache\r\n"
				"Connection: close\r\n\r\n";
			if (!SendAll(sock, kHead, sizeof(kHead) - 1)) { closesocket(sock); return; }

			g_Stats.viewers.fetch_add(1);
			const int viewerId = g_Stats.viewers.load();
			Info("[http] viewer #{} connected from {} (MJPEG stream)", viewerId, peer);

			uint64_t seen = 0;
			while (!g_Stop.load()) {
				auto frame = g_Hub.WaitNext(seen, 1000);
				if (!frame) continue;
				char part[160];
				const int pn = snprintf(part, sizeof(part),
					"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", frame->size());
				if (!SendAll(sock, part, static_cast<size_t>(pn)) ||
				    !SendAll(sock, frame->data(), frame->size()) ||
				    !SendAll(sock, "\r\n", 2))
					break;   // 客户端断开 / 发送超时
			}
			g_Stats.viewers.fetch_sub(1);
			Info("[http] viewer #{} disconnected ({})", viewerId, peer);
		} else if (path == "/favicon.ico") {
			SendSimple(sock, "204 No Content", "text/plain", "");
		} else {
			SendSimple(sock, "404 Not Found", "text/plain", "not found");
		}
		closesocket(sock);
	}

	std::vector<std::thread> g_ClientThreads;
	std::mutex g_ClientThreadsMutex;

	void HttpServerLoop() {
		WSADATA wsa{};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			Error("[http] WSAStartup failed ({})", WSAGetLastError());
			g_Stop.store(true);
			return;
		}
		// ── 双栈监听 ────────────────────────────────────────────────────────
		// 用 AF_INET6 + IPV6_V6ONLY=0：一个端口同时接受 IPv6 客户端和 IPv4 客户端
		//（后者在 socket 层表现为 ::ffff:a.b.c.d 的映射地址）。于是局域网走 IPv4、
		// 跨网走公网 IPv6，都不需要额外端口映射。
		int family = AF_INET6;
		SOCKET listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
		if (listener != INVALID_SOCKET) {
			DWORD v6only = 0;
			if (setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) != 0)
				Warn("[http] IPV6_V6ONLY=0 failed ({}) -- IPv4 clients may be refused", WSAGetLastError());
		} else {
			Warn("[http] IPv6 socket() failed ({}), falling back to IPv4-only", WSAGetLastError());
			family = AF_INET;
			listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		}
		if (listener == INVALID_SOCKET) {
			Error("[http] socket() failed ({})", WSAGetLastError());
			WSACleanup();
			g_Stop.store(true);
			return;
		}
		BOOL reuse = TRUE;
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

		if (family == AF_INET6) {
			sockaddr_in6 addr{};
			addr.sin6_family = AF_INET6;
			addr.sin6_addr = in6addr_any;                  // 所有网卡上的所有 IPv6/IPv4 地址
			addr.sin6_port = htons(g_Cfg.port);
			if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
				Error("[http] bind [::]:{} failed ({} -- port already in use?)", g_Cfg.port, WSAGetLastError());
				closesocket(listener);
				WSACleanup();
				g_Stop.store(true);
				return;
			}
		} else {
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_ANY);
			addr.sin_port = htons(g_Cfg.port);
			if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
				Error("[http] bind 0.0.0.0:{} failed ({} -- port already in use?)", g_Cfg.port, WSAGetLastError());
				closesocket(listener);
				WSACleanup();
				g_Stop.store(true);
				return;
			}
		}
		listen(listener, SOMAXCONN);
		Info("[http] listening on {}:{} (dual-stack)", family == AF_INET6 ? "[::]" : "0.0.0.0", g_Cfg.port);

		while (!g_Stop.load()) {
			// 用带超时的 select 等连接：直接阻塞在 accept() 上的话，退出时没人能唤醒它，
			// 主线程会在 join() 处永远等下去（实测挂死；之前几次"干净退出"只是恰好有客户端连了一下）。
			fd_set rfds;
			FD_ZERO(&rfds);
			FD_SET(listener, &rfds);
			timeval tv{ 0, 200 * 1000 };            // 200ms
			const int sel = select(0, &rfds, nullptr, nullptr, &tv);
			if (sel <= 0) continue;                 // 超时/出错 → 回循环顶部检查 g_Stop

			sockaddr_storage peerAddr{};
			int peerLen = sizeof(peerAddr);
			SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen);
			if (client == INVALID_SOCKET) {
				if (g_Stop.load()) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}
			char peerIp[64] = "?";
			if (peerAddr.ss_family == AF_INET) {
				inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peerAddr)->sin_addr, peerIp, sizeof(peerIp));
			} else {
				auto* sin6 = reinterpret_cast<sockaddr_in6*>(&peerAddr);
				if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))   // 双栈 socket 上的 IPv4 客户端
					inet_ntop(AF_INET, &sin6->sin6_addr.u.Byte[12], peerIp, sizeof(peerIp));
				else
					inet_ntop(AF_INET6, &sin6->sin6_addr, peerIp, sizeof(peerIp));
			}
			// 低延迟：禁用 Nagle；并给发送设超时，避免卡死的客户端占住线程。
			BOOL nodelay = TRUE;
			setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
			DWORD timeoutMs = 5000;
			setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

			std::lock_guard<std::mutex> lock(g_ClientThreadsMutex);
			g_ClientThreads.emplace_back(HandleClient, client, std::string(peerIp));
		}

		closesocket(listener);
		{
			std::lock_guard<std::mutex> lock(g_ClientThreadsMutex);
			for (auto& t : g_ClientThreads) if (t.joinable()) t.join();
			g_ClientThreads.clear();
		}
		WSACleanup();
		Info("[http] server stopped");
	}

	// 按**网卡**列出可访问地址（IPv4 局域网 + IPv6 跨网）：
	// 只列 up 的物理/无线网卡，虚拟网卡（WSL / Hyper-V / VMware…）排最后并标注；
	// IPv6 只列**全球单播**（2000::/3）—— 链路本地 fe80:: 和 ULA fc00::/7 出了这个网段就没用。
	void PrintAccessUrls() {
		struct Entry { std::string ip; std::string adapter; int prio; bool v6; bool global; };
		std::vector<Entry> entries;
		ULONG size = 16 * 1024;
		std::vector<uint8_t> buf(size);
		auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
		const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
		ULONG ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size);   // 两个地址族都要
		if (ret == ERROR_BUFFER_OVERFLOW) {
			buf.resize(size);
			addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
			ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &size);
		}
		if (ret == NO_ERROR) {
			for (auto* a = addrs; a; a = a->Next) {
				if (a->OperStatus != IfOperStatusUp) continue;
				if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
				char nameUtf8[256] = "?";
				if (a->FriendlyName) {
					WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);
				}
				const std::string name = nameUtf8;
				int prio = 3;
				if (a->IfType == IF_TYPE_IEEE80211) prio = 0;                                   // Wi-Fi 最优先
				else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) prio = 1;                        // 有线次之
				if (name.find("vEthernet") != std::string::npos || name.find("WSL") != std::string::npos ||
				    name.find("Virtual") != std::string::npos || name.find("VMware") != std::string::npos ||
				    name.find("VirtualBox") != std::string::npos || name.find("Loopback") != std::string::npos ||
				    name.find("Bluetooth") != std::string::npos)
					prio = 9;                                                                   // 虚拟/次要网卡排最后
				for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
					if (!ua->Address.lpSockaddr) continue;
					char ip[64]{};
					if (ua->Address.lpSockaddr->sa_family == AF_INET) {
						auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
						inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
						entries.push_back({ ip, name, prio, false, false });
					} else if (ua->Address.lpSockaddr->sa_family == AF_INET6) {
						auto* sin6 = reinterpret_cast<sockaddr_in6*>(ua->Address.lpSockaddr);
						const auto* b = reinterpret_cast<const uint8_t*>(&sin6->sin6_addr);
						if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) continue;
						if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;              // fe80::/10
						if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) continue;
						const bool global = ((b[0] & 0xE0) == 0x20);                        // 2000::/3 全球单播
						const bool ula = ((b[0] & 0xFE) == 0xFC);                           // fc00::/7 仅站内
						if (!global && !ula) continue;
						inet_ntop(AF_INET6, &sin6->sin6_addr, ip, sizeof(ip));
						// IPv6 排在 IPv4 之后（局域网最常用），但全球地址优先于 ULA
						entries.push_back({ ip, name, prio * 10 + (global ? 0 : 1) + 1, true, global });
					}
				}
			}
		}
		std::sort(entries.begin(), entries.end(),
			[](const Entry& a, const Entry& b) { return a.prio < b.prio; });

		Info("--------------------------------------------------------------");
		Info("  RemoteCube is up on port {} (dual-stack: IPv4 + IPv6).", g_Cfg.port);
		auto printEntry = [](const Entry& e, uint16_t port) {
			const char* tag = e.prio >= 90 ? "  <- virtual/secondary, phone cannot use this" : "";
			if (e.v6) Info("      http://[{}]:{}/   [{}]{}{}", e.ip, port, e.adapter,
				e.global ? "  <- cross-network (IPv6)" : "  <- IPv6 ULA, LAN only", tag);
			else       Info("      http://{}:{}/   [{}]{}", e.ip, port, e.adapter, tag);
		};
		bool anyV4 = false, anyGlobalV6 = false;
		for (const auto& e : entries) { if (!e.v6) anyV4 = true; if (e.v6 && e.global) anyGlobalV6 = true; }
		Info("  LAN (same Wi-Fi / Ethernet):");
		if (!anyV4) Info("      (no IPv4 address found)");
		for (const auto& e : entries) if (!e.v6) printEntry(e, g_Cfg.port);
		Info("  Cross-network (phone on mobile data, needs a global IPv6 address):");
		if (!anyGlobalV6) Info("      (this machine has no global IPv6 address -- cross-network needs IPv6 or a tunnel)");
		for (const auto& e : entries) if (e.v6) printEntry(e, g_Cfg.port);
		Info("      http://127.0.0.1:{}/  or  http://[::1]:{}/   [local test]", g_Cfg.port, g_Cfg.port);
		Info("  If the phone cannot connect:");
		Info("    1) Windows Firewall -- run once as admin (or start with -OpenFirewall). The rule below");
		Info("       covers both IPv4 and IPv6:");
		Info("       netsh advfirewall firewall add rule name=\"RemoteCube {}\" dir=in action=allow protocol=TCP localport={}",
			g_Cfg.port, g_Cfg.port);
		Info("    2) Same-Wi-Fi only: use the LAN address; the PC's network profile should be Private.");
		Info("    3) Cross-network over IPv6: the HOME ROUTER must also allow inbound IPv6 to this host");
		Info("       (China ISP CPEs block it by default -- that is the usual blocker), and the phone's");
		Info("       network must have IPv6 (mobile data usually does; many hotel/office Wi-Fi does not).");
		Info("--------------------------------------------------------------");
	}

	// -OpenFirewall：弹 UAC 执行 netsh 放行端口（默认关，只有显式加参数才会动系统设置）。
	void OpenFirewallRule() {
		char params[320];
		snprintf(params, sizeof(params),
			"/c netsh advfirewall firewall add rule name=\"RemoteCube %u\" dir=in action=allow protocol=TCP localport=%u",
			g_Cfg.port, g_Cfg.port);
		SHELLEXECUTEINFOA sei{};
		sei.cbSize = sizeof(sei);
		sei.lpVerb = "runas";              // 触发 UAC
		sei.lpFile = "cmd.exe";
		sei.lpParameters = params;
		sei.nShow = SW_HIDE;
		sei.fMask = SEE_MASK_NOCLOSEPROCESS;
		if (ShellExecuteExA(&sei)) {
			WaitForSingleObject(sei.hProcess, 20000);
			CloseHandle(sei.hProcess);
			Info("[firewall] rule for TCP {} added (or already existed)", g_Cfg.port);
		} else {
			Warn("[firewall] could not add rule (UAC declined?) -- add it manually with the netsh command above");
		}
	}

	// ── 程序化纹理：棋盘 + 彩色边框，省掉外部贴图文件依赖 ─────────────────────
	std::vector<uint8_t> MakeCheckerTexture(uint32_t size) {
		std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
		for (uint32_t y = 0; y < size; ++y) {
			for (uint32_t x = 0; x < size; ++x) {
				const uint32_t cell = (x / (size / 8) + y / (size / 8)) & 1u;
				uint8_t r = cell ? 235 : 40, g = cell ? 225 : 60, b = cell ? 200 : 90;
				if (x < size / 16 || y < size / 16 || x >= size - size / 16 || y >= size - size / 16) {
					r = 255; g = 140; b = 40;                  // 边框：橙色，方便看朝向
				}
				const size_t i = (static_cast<size_t>(y) * size + x) * 4;
				pixels[i + 0] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = 255;
			}
		}
		return pixels;
	}

	bool ParseArgs(int argc, char** argv) {
		for (int i = 1; i < argc; ++i) {
			const std::string a = argv[i];
			auto next = [&](double& out) { if (i + 1 < argc) out = std::atof(argv[++i]); };
			if (a == "-Port" || a == "-port") { if (i + 1 < argc) g_Cfg.port = static_cast<uint16_t>(std::atoi(argv[++i])); }
			else if (a == "-W" || a == "-w") { if (i + 1 < argc) g_Cfg.width = static_cast<uint32_t>(std::atoi(argv[++i])); }
			else if (a == "-H" || a == "-h") { if (i + 1 < argc) g_Cfg.height = static_cast<uint32_t>(std::atoi(argv[++i])); }
			else if (a == "-Quality" || a == "-quality") { if (i + 1 < argc) g_Cfg.quality = std::atoi(argv[++i]); }
			else if (a == "-Fps" || a == "-fps") { next(g_Cfg.targetFps); }
			else if (a == "-Cubes" || a == "-cubes") { if (i + 1 < argc) g_Cfg.cubes = static_cast<uint32_t>(std::atoi(argv[++i])); }
			else if (a == "-Seconds" || a == "-seconds") { next(g_Cfg.seconds); }
			else if (a == "-Frames" || a == "-frames") { if (i + 1 < argc) g_Cfg.frames = std::strtoull(argv[++i], nullptr, 10); }
			else if (a == "-OpenFirewall" || a == "-openfirewall") { g_Cfg.openFirewall = true; }
			else if (a == "-Threads" || a == "-threads") { if (i + 1 < argc) g_Cfg.encoderThreads = std::atoi(argv[++i]); }
			else Warn("unknown argument, ignored: {}", a);
		}
		if (g_Cfg.width < 64 || g_Cfg.height < 64 || g_Cfg.width > 4096 || g_Cfg.height > 4096) {
			Error("invalid resolution: {}x{}", g_Cfg.width, g_Cfg.height);
			return false;
		}
		if (g_Cfg.quality < 1) g_Cfg.quality = 1;
		if (g_Cfg.quality > 100) g_Cfg.quality = 100;
		if (g_Cfg.cubes < 1) g_Cfg.cubes = 1;
		return true;
	}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
#endif
	Info("============================================");
	Info("|  RemoteCube -- headless render + MJPEG   |");
	Info("============================================");
	if (!ParseArgs(argc, argv)) return 1;
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	g_StartTime = std::chrono::steady_clock::now();

	const uint32_t W = g_Cfg.width, H = g_Cfg.height;

	// ── 1. RHI + Headless 呈现设备（无窗口、无 surface、无交换链）─────────────
	FISIR::RHICreator::setRenderInterfaceApi(FISIR::RHIAPI::Vulkan);
	FISIR::DynamicRHI* rhi = FISIR::RHICreator::getCurrentRenderInterface();
	if (!rhi) { Error("[RemoteCube] failed to create RHI"); return 1; }
	auto viewport = rhi->RHICreateViewport(W, H, FISIR::TextureCOLORType::RGBA_8,
		FISIR::DisplayDeviceType::Headless, nullptr, 3);
	rhi->Init();
	auto swapchain = rhi->RHIGetSwapChain(viewport);        // Headless ⇒ nullptr
	if (swapchain) Warn("[RemoteCube] expected no swapchain for Headless, but got 0x{:x}", (size_t)swapchain);

	// ── 2. 着色器（与 TextureCube 同款：纹理立方体 + 网格实例摆放）───────────
	const wchar_t* vsCode = LR"(
		[[vk::binding(0, 0)]] cbuffer MVPBuffer  : register(b0) {
			float4x4 ViewProj;
			float4x4 Model;
			float4 GridParams;   // x=cols y=rows z=spacingX w=spacingY
		};
		struct VSInput {
			float3 pos   : POSITION;
			float3 color : COLOR;
			float2 uv    : TEXCOORD;
		};
		struct PSInput {
			float4 pos   : SV_POSITION;
			float3 color : COLOR;
			float2 uv    : TEXCOORD;
		};
		PSInput mainVs(VSInput input, uint instanceID : SV_InstanceID) {
			PSInput output;
			float cols = GridParams.x, rows = GridParams.y;
			float col = fmod((float)instanceID, cols);
			float row = floor((float)instanceID / cols);
			float3 offset = float3((col - (cols - 1.0) * 0.5) * GridParams.z,
			                       (row - (rows - 1.0) * 0.5) * GridParams.w,
			                       0.0);
			// 公转 / 自转的唯一区别就在这一行的结合顺序：
			//   mul(Model, local + offset) —— offset 也被 Model 转了，整个网格绕世界原点公转；
			//   mul(Model, local) + offset —— 先绕自身中心旋转，再平移到网格位置 ⇒ 原地自转。
			float4 world = mul(Model, float4(input.pos, 1.0)) + float4(offset, 0.0);
			output.pos = mul(ViewProj, world);
			output.color = input.color;
			output.uv = input.uv;
			return output;
		}
	)";
	auto* vsCompiler = new FISIR::ShaderComplier();
	vsCompiler->compileShader(vsCode, wcslen(vsCode) * sizeof(wchar_t), L"mainVs", L"vs_5_0");
	auto* vs = rhi->RHICreateShader(FISIR::ShaderTYP::__VERTEXSHADER__, "mainVs",
		vsCompiler->getShaderData(), vsCompiler->getShaderDataSize());
	delete vsCompiler;

	const wchar_t* psCode = LR"(
		[[vk::binding(1, 0)]] Texture2D    myTexture  : register(t1);
		[[vk::binding(2, 0)]] SamplerState mySampler  : register(s2);
		struct PSInput {
			float4 pos   : SV_POSITION;
			float3 color : COLOR;
			float2 uv    : TEXCOORD;
		};
		float4 mainPs(PSInput input) : SV_TARGET {
			return myTexture.Sample(mySampler, input.uv) * float4(input.color, 1.0);
		}
	)";
	auto* psCompiler = new FISIR::ShaderComplier();
	psCompiler->compileShader(psCode, wcslen(psCode) * sizeof(wchar_t), L"mainPs", L"ps_5_0");
	auto* ps = rhi->RHICreateShader(FISIR::ShaderTYP::__FRAGMENTSHADER__, "mainPs",
		psCompiler->getShaderData(), psCompiler->getShaderDataSize());
	delete psCompiler;

	// ── 3. 离屏渲染目标（本示例自备 —— Headless 路径没有交换链）──────────────
	FISIR::TextureInfo colorTexInfo{
		.size = { H, W, 1 },
		.colorType = FISIR::TextureCOLORType::RGBA_8,
		.type = FISIR::TextureType::TEXTURE2D,
		.useFor = FISIR::TextureUseForColorAttachment | FISIR::TextureUseForTransferSrc |
		          FISIR::TextureUseForTransferDst | FISIR::TextureUseForShaderReadOnly,
		.mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,
	};
	auto* colorTexture = rhi->RHICreateTexture(colorTexInfo);
	if (!colorTexture) { Error("[RemoteCube] offscreen color texture creation failed"); return 1; }

	FISIR::TextureInfo depthTexInfo{
		.size = { H, W, 1 },
		.colorType = FISIR::TextureCOLORType::Depth24_Stencil8,
		.type = FISIR::TextureType::TEXTURE2D,
		.useFor = FISIR::TextureUseForDepthStencilAttachment,
		.mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,
	};
	auto* depthTexture = rhi->RHICreateTexture(depthTexInfo);

	FISIR::ColorEntry colorEntry{ {.loadOp = FISIR::RenderTargetLoadAction::Clear,
		.storeOp = FISIR::RenderTargetStoreAction::Store,
		.dstLayout = FISIR::TextureLayout::ShaderReadOnlyOptimal,
		.colorType = FISIR::TextureCOLORType::RGBA_8, .sampleCount = 0} };
	FISIR::DepthStencilEntry depthEntry{ .sampleCount = 0,
		.dstLayout = FISIR::TextureLayout::DepthStencilAttachmentOptimal, .exeit = true };
	depthEntry.depthAction.setDWAndSW(true, true);
	FISIR::SubPassInfo subPass{ .ColorEntryMask = 1, .UseDepthStencil = true, .ReadDepthAsInput = false };
	FISIR::RHIRenderPassInfo renderPassInfo({ {0, colorEntry} }, depthEntry, { subPass });
	auto* framebuffer = rhi->RHICreateFrameBuffer(W, H, { colorTexture, depthTexture }, renderPassInfo);
	if (!framebuffer) { Error("[RemoteCube] framebuffer creation failed"); return 1; }

	// ── 4. 管线 ─────────────────────────────────────────────────────────────
	FISIR::RHIPipelineDescribeInfo describeInfo{
		{0, 1, FISIR::RHIDescriptorTyp::UniformBuffer, FISIR::RHIUsingStage::VertexShaderStage},
		{1, 1, FISIR::RHIDescriptorTyp::SamplerImage, FISIR::RHIUsingStage::FragmentShaderStage},
	{2, 1, FISIR::RHIDescriptorTyp::Sampler, FISIR::RHIUsingStage::FragmentShaderStage},
	};
	FISIR::RHIVertexInputInfo vertexInputInfo{
		FISIR::RHIBaseDataTYPE::_Fvec3, FISIR::RHIBaseDataTYPE::_Fvec3, FISIR::RHIBaseDataTYPE::_Fvec2,
	};
	FISIR::RHIPipelineState pipelineState{
		.describeInfo = describeInfo,
		.vertexInfo = vertexInputInfo,
		.topologyType = FISIR::TopologyType::Triangle,
		.rasterizationState = { false, false, false, FISIR::PolygonMode::Fill, FISIR::FrontFace::CW, FISIR::CullMode::None },
		.depthStencilState = {1, 1, 0, 0.0, 1.0, FISIR::_Equal_Less_},
		.colorblendState = {.UsingColorBit = (FISIR::ColorBit)(FISIR::_R_PASS_ | FISIR::_G_PASS_ | FISIR::_B_PASS_) },
		.renderpass = framebuffer->getFrameRenderPass(),
	};
	pipelineState.Shaders[FISIR::__VERTEXSHADER__] = vs;
	pipelineState.Shaders[FISIR::__FRAGMENTSHADER__] = ps;
	auto* pipeline = rhi->RHICreatePipeline(pipelineState);
	if (!pipeline) { Error("[RemoteCube] pipeline creation failed"); return 1; }

	// ── 5. 常量缓冲 / 纹理 / 采样器 / 资源包 ────────────────────────────────
	struct FrameUniforms {
		glm::mat4 ViewProj;
		glm::mat4 Model;
		glm::vec4 GridParams;
	};
	FISIR::BufferInfo uniformInfo{
		.data_CPU = nullptr,
		.size = sizeof(FrameUniforms),
		.stride = sizeof(FrameUniforms),
		.bufferlayout = FISIR::UniformBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	auto* uniformBuffer = rhi->RHICreateBuffer(uniformInfo);

	const uint32_t kTexSize = 256;
	std::vector<uint8_t> texPixels = MakeCheckerTexture(kTexSize);
	FISIR::TextureInfo texInfo{
		.size = { kTexSize, kTexSize, 1 },
		.colorType = FISIR::TextureCOLORType::RGBA_8,
		.type = FISIR::TextureType::TEXTURE2D,
		.useFor = FISIR::TextureUseForShaderReadOnly | FISIR::TextureUseForTransferDst,
		.mipLevels = 1, .arrayLayers = 1, .sampleCount = 0,
	};
	auto* texture = rhi->RHICreateTexture(texInfo);
	FISIR::BufferInfo uploadInfo{
		.data_CPU = texPixels.data(),
		.size = texPixels.size(),
		.bufferlayout = FISIR::TransferSrcBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	auto* uploadBuffer = rhi->RHICreateBuffer(uploadInfo);
	{
		FISIR::RHIRenderCommandList upload(rhi);
		FISIR::RHITexture* texArray[] = { texture };
		upload.TransitionTextures(texArray, 1,
			FISIR::ResourceAccess::Undefined, FISIR::ResourceAccess::TransferDst,
			FISIR::TextureLayout::Undefined, FISIR::TextureLayout::TransferDstOptimal,
			FISIR::RHIUsingStage::NoneStage, FISIR::RHIUsingStage::PipelineTransferStage);
		upload.CopyToTexture(uploadBuffer, texture, 0, 0, 1, 0, { 0, 0, 0 }, texInfo.size);
		upload.TransitionTextures(texArray, 1,
			FISIR::ResourceAccess::TransferDst, FISIR::ResourceAccess::ShaderReadOnly,
			FISIR::TextureLayout::TransferDstOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
			FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);
		auto* fence = rhi->RHICreateFence();
		upload.End(fence);
		fence->wait();
		rhi->RHIDestroyFence(fence);
	}

	FISIR::SamplerInfo samplerInfo;
	auto* sampler = rhi->RHICreateSampler(samplerInfo);
	auto resourcePack = rhi->RHICreateResourcePack({ uniformBuffer, texture, sampler });

	// ── 6. 几何：36 顶点立方体 + 36 索引 ────────────────────────────────────
	struct Vertex { float pos[3]; float color[3]; float uv[2]; };
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	{
		const float s = 0.5f;
		const float faceNormal[6][3] = { {0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0} };
		for (int f = 0; f < 6; ++f) {
			const float n[3] = { faceNormal[f][0], faceNormal[f][1], faceNormal[f][2] };
			// 面内两个切向
			float t[3] = { n[1], n[2], n[0] };
			float b[3] = { n[2], n[0], n[1] };
			const uint32_t base = static_cast<uint32_t>(vertices.size());
			const int quad[4][2] = { {-1,-1},{1,-1},{1,1},{-1,1} };
			for (int c = 0; c < 4; ++c) {
				const float u = quad[c][0], v = quad[c][1];
				Vertex vert{};
				for (int k = 0; k < 3; ++k) vert.pos[k] = s * (n[k] + t[k] * u + b[k] * v);
				vert.color[0] = 1.0f; vert.color[1] = 1.0f; vert.color[2] = 1.0f;
				vert.uv[0] = (u + 1.0f) * 0.5f;
				vert.uv[1] = (1.0f - v) * 0.5f;
				vertices.push_back(vert);
			}
			indices.insert(indices.end(), { base + 0, base + 1, base + 2, base + 0, base + 2, base + 3 });
		}
	}
	FISIR::BufferInfo vertexInfo{
		.data_CPU = vertices.data(), .size = vertices.size() * sizeof(Vertex),
		.stride = sizeof(Vertex), .bufferlayout = FISIR::VertexBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	auto* vertexBuffer = rhi->RHICreateBuffer(vertexInfo);
	FISIR::BufferInfo indexInfo{
		.data_CPU = indices.data(), .size = indices.size() * sizeof(uint32_t),
		.stride = 0, .bufferlayout = FISIR::IndexBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	auto* indexBuffer = rhi->RHICreateBuffer(indexInfo);

	// ── 7. 读回缓冲（host visible）+ 围栏：每帧渲完 → 拷回 → JPEG ────────────
	FISIR::BufferInfo readbackInfo{
		.data_CPU = nullptr,
		.size = static_cast<uint64_t>(W) * H * 4,
		.stride = 0,
		.bufferlayout = FISIR::TransferDstBuffer,
		.memoryType = (FISIR::MemType)(FISIR::MemTypHostVisable | FISIR::MemTypHostCoherent),
	};
	auto* readback = rhi->RHICreateBuffer(readbackInfo);
	auto* frameFence = rhi->RHICreateFence(false, "RemoteCubeFrameFence");

	// ── 8. HTTP 服务线程 + 编码线程池 ───────────────────────────────────────
	if (g_Cfg.openFirewall) OpenFirewallRule();
	std::thread httpThread(HttpServerLoop);
	std::this_thread::sleep_for(std::chrono::milliseconds(150));   // 让 bind/listen 先就绪
	PrintAccessUrls();

	// 编码线程数：默认 min(4, 硬件线程数-1) —— 留出核给渲染/RHI 线程与 HTTP。
	const int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
	int encThreads = g_Cfg.encoderThreads > 0 ? g_Cfg.encoderThreads : std::min(4, std::max(1, hwThreads - 1));
	encThreads = 16;
	g_RawQueue = new RawFrameQueue(static_cast<size_t>(encThreads) + 1, &g_BufferPool);
	std::vector<std::thread> encoders;
	encoders.reserve(encThreads);
	for (int i = 0; i < encThreads; ++i) encoders.emplace_back(EncoderWorker);
	Info("[RemoteCube] encoder threads = {} (hardware threads {})", encThreads, hwThreads);

	// ── 9. 渲染 + 编码循环 ─────────────────────────────────────────────────
	const uint32_t cubes = g_Cfg.cubes;
	const float aspect = static_cast<float>(W) / static_cast<float>(H);
	const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(cubes) * aspect)));
	const uint32_t rows = (cubes + cols - 1) / cols;
	const float halfH = 3.0f * std::tan(glm::radians(30.0f));
	const float halfW = halfH * aspect;
	glm::vec4 gridParams(static_cast<float>(cols), static_cast<float>(rows),
	                     2.0f * halfW / static_cast<float>(cols),
	                     2.0f * halfH / static_cast<float>(rows));
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 50.0f);
	const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.2f, 3.6f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	const glm::mat4 viewProj = proj * view;

	FISIR::ClearValue clear{ .ColorClear = 1, .colorinfo = {0.05f, 0.06f, 0.09f, 1.0f}, .depthclearval = 1.0f };
	FISIR::RHITexture* colorArray[] = { colorTexture };

	Info("[RemoteCube] rendering {}x{}, {} cubes, JPEG quality {}, target {} fps",
		W, H, cubes, g_Cfg.quality, g_Cfg.targetFps);
	float angle = 0.0f;
	uint64_t frameCount = 0;
	bool gpuStalled = false;
	uint64_t lastRenderFrames = 0, lastEncodedFrames = 0, lastEncodedBytes = 0;
	auto windowStart = std::chrono::steady_clock::now();

	while (!g_Stop.load()) {
		const auto frameStart = std::chrono::steady_clock::now();

		// 上一帧的提交已经等过了（见循环末尾），这里可以安全改写 uniform。
		FrameUniforms uniforms{};
		uniforms.ViewProj = viewProj;
		uniforms.Model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f));
		uniforms.GridParams = gridParams;
		uniformBuffer->updateBufferData(&uniforms, sizeof(FrameUniforms));

		// 录制：渲染到离屏纹理 → 转出到 TransferSrc → 拷回 host 可见缓冲 → 转回 ShaderReadOnly
		FISIR::RHIRenderCommandList cmdList(rhi);
		cmdList.BeginRenderPass(framebuffer, 0, clear);
		cmdList.SetPipelineState(pipeline);
		cmdList.SetVertexBuffer(vertexBuffer, 0, 0);
		cmdList.SetIndexBuffer(indexBuffer, 0);
		cmdList.SetResourcePack(resourcePack);
		cmdList.SetViewPort(0, 0, static_cast<float>(W), static_cast<float>(H), 1.0f, 0.0f);
		cmdList.SetScissor(W, H);
		cmdList.DrawIndex(0, 36, 0, cubes);
		cmdList.EndRenderPass();

		cmdList.TransitionTextures(colorArray, 1,
			FISIR::ResourceAccess::ShaderReadOnly, FISIR::ResourceAccess::TransferSrc,
			FISIR::TextureLayout::ShaderReadOnlyOptimal, FISIR::TextureLayout::TransferSrcOptimal,
			FISIR::RHIUsingStage::FragmentShaderStage, FISIR::RHIUsingStage::PipelineTransferStage);
		// 注意字段序：CopyImageToBuffer 的 extent 与 TextureInfo.size 一样是 **{height, width, depth}**
		//（不是 {width, height}）。按宽度优先传会越界 —— 校验层报
		// VUID-vkCmdCopyImageToBuffer-imageSubresource-07972，Release 下直接设备丢失。
		// TextureCube 是 1024x1024 正方形所以一直没暴露这个坑，非正方形渲染目标才会踩到。
		cmdList.CopyImageToBuffer(colorTexture, readback, 0, 0, 1, { 0, 0, 0 }, 0, { H, W, 1 });
		cmdList.TransitionTextures(colorArray, 1,
			FISIR::ResourceAccess::TransferSrc, FISIR::ResourceAccess::ShaderReadOnly,
			FISIR::TextureLayout::TransferSrcOptimal, FISIR::TextureLayout::ShaderReadOnlyOptimal,
			FISIR::RHIUsingStage::PipelineTransferStage, FISIR::RHIUsingStage::FragmentShaderStage);

		// 围栏复用前必须 reset()：否则 VulkanFence 里「已置位」的缓存会让下面的 wait() 立刻返回。
		frameFence->reset();
		cmdList.End(frameFence, {}, {});
		// **有界**等待：设备丢失 / 提交失败时围栏永远不会置位，无界 wait() 会把示例永久挂住
		//（前面实测过：设备丢失后进程一直卡在这里，-Seconds 都到不了）。超时就当作 GPU 掉线退出。
		if (!frameFence->waitFor(5'000'000'000ull)) {
			Error("[RemoteCube] GPU did not finish this frame within 5s (device lost?), exiting");
			gpuStalled = true;
			break;
		}

		// ── 把读回的原始帧交给编码线程池 ─────────────────────────────────────
		// 单线程 stb 编码 960x540 约 120ms，是整条链路唯一瓶颈；而 stb 本身是单线程实现，
		// 所以加速手段是**多帧并行编码**（每帧仍由一个线程完整编码，保证 JPEG 合法可解码）。
		{
			auto raw = std::make_shared<RawFrame>();
			raw->rgba = g_BufferPool.Acquire(static_cast<size_t>(W) * H * 4);
			memcpy(raw->rgba.data(), readback->getBufferData(), raw->rgba.size());
			raw->w = W;
			raw->h = H;
			raw->seq = ++g_FrameSeq;
			g_RawQueue->Push(std::move(raw));
		}
		++frameCount;
		angle += 0.02f;                     // 之前漏了这句 —— 画面一直是静止的第一帧

		const auto now = std::chrono::steady_clock::now();
		const double windowSec = std::chrono::duration<double>(now - windowStart).count();
		if (windowSec >= 1.0) {
			// 统计口径分开看：render = 投递给编码池的帧数；encoded = 真正编出并推给客户端的帧数。
			// 两者差距大就说明瓶颈在编码（render 被丢帧策略截断），而不是渲染或网络。
			const uint64_t renderNow = frameCount;
			const uint64_t encodedNow = g_Stats.framesSent.load();
			const uint64_t bytesNow = g_Stats.jpegBytes.load();
			const double renderFps = (renderNow - lastRenderFrames) / windowSec;
			const double encodedFps = (encodedNow - lastEncodedFrames) / windowSec;
			const double mbps = ((bytesNow - lastEncodedBytes) * 8.0) / windowSec / 1e6;
			lastRenderFrames = renderNow;
			lastEncodedFrames = encodedNow;
			lastEncodedBytes = bytesNow;
			g_Stats.fps.store(encodedFps);
			g_Stats.mbps.store(mbps);
			Info("[RemoteCube] render {:.1f} fps | encoded {:.1f} fps | {:.2f} Mbps | jpeg {:.1f} KB | viewers {} | queued {} | dropped {}",
				renderFps, encodedFps, mbps, g_Stats.jpegSize.load() / 1024.0,
				g_Stats.viewers.load(), g_RawQueue->Pending(), g_RawQueue->Dropped());
			windowStart = now;
		}

		// 帧率上限（无 vsync 可依赖，headless 必须自己节流）
		if (g_Cfg.targetFps > 0.0) {
			const double targetMs = 1000.0 / g_Cfg.targetFps;
			const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
			if (elapsedMs < targetMs)
				std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(targetMs - elapsedMs));
		}

		if (g_Cfg.frames && frameCount >= g_Cfg.frames) break;
		if (g_Cfg.seconds > 0.0 &&
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - g_StartTime).count() >= g_Cfg.seconds) break;
	}

	Info("[RemoteCube] render loop ended after {} frames ({} encoded), shutting down...",
		frameCount, g_Stats.framesSent.load());
	g_Stop.store(true);
	if (g_RawQueue) g_RawQueue->StopAll();
	for (auto& t : encoders) if (t.joinable()) t.join();
	if (httpThread.joinable()) httpThread.join();

	// ── 10. 清理（先等 GPU 落地再销毁资源，与 TextureCube 的顺序一致）────────
	if (!gpuStalled && frameFence->isSubmited()) frameFence->waitFor(2'000'000'000ull);
	rhi->RHIDestroyFence(frameFence);
	rhi->RHIDestroyResourcePack(resourcePack);
	if (sampler) rhi->RHIDestroySampler(sampler);
	if (vertexBuffer) rhi->RHIDestroyBuffer(vertexBuffer); 
	if (indexBuffer) rhi->RHIDestroyBuffer(indexBuffer);
	if (uniformBuffer) rhi->RHIDestroyBuffer(uniformBuffer);
	if (uploadBuffer) rhi->RHIDestroyBuffer(uploadBuffer);
	if (readback) rhi->RHIDestroyBuffer(readback);
	if (framebuffer) rhi->RHIDestroyFrameBuffer(framebuffer);
	if (colorTexture) rhi->RHIDestroyTexture(colorTexture);
	if (depthTexture) rhi->RHIDestroyTexture(depthTexture);
	if (texture) rhi->RHIDestroyTexture(texture);
	FISIR::RHICreator::destroyRenderInterface();
	FISIR::RHICreator::freeCurrentRenderInterfaceApi();
	Info("=== RemoteCube -- Done ===");
	return 0;
}
