#include "Logger.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include <chrono>
#include <ctime>
#ifdef __ANDROID__
#include <android/log.h>   // __android_log_print：把日志发到 logcat（tag=FISIR）
#endif

namespace FISIR {
	static std::atomic_bool stopTag = false;
	static std::mutex*              Tlock     = nullptr;
	static std::condition_variable* Cv        = nullptr;
	static std::thread*             MsgThread = nullptr;
	static std::atomic_bool initTag = false;
	static const char* colors[] = {
		"\033[32m", "\033[33m", "\033[31m", "\033[4;36m"
	};


	struct MsgData {
		MsgData():level(LogLevel::INFO_){Msg[0] = '\0'; file[0] = '\0'; function[0] = '\0';}

		MsgData(LogLevel l, std::string_view f, std::string_view fn, std::string_view s): level(l) {
			size_t len = std::min(s.size(), sizeof(Msg) - 1);
			std::copy_n(s.data(), len, Msg);
			Msg[len] = '\0';

			len = std::min(f.size(), sizeof(file) - 1);
			std::copy_n(f.data(), len, file);
			file[len] = '\0';

			len = std::min(fn.size(), sizeof(function) - 1);
			std::copy_n(fn.data(), len, function);
			function[len] = '\0';
		}
		LogLevel level;
		char file[256];
		char function[256];
		char Msg[2*1024];
	};

	class MsgQue {
	public:
		MsgQue() {
			Datas = static_cast<MsgData*>(malloc(sizeof(MsgData) * Capacity));
			if (!Datas) Datas = Fallback_Datas;
		}


		~MsgQue() {
			{
				std::lock_guard<std::mutex> lock(Qlock);
			}
			cv_empty.notify_all();
			cv_full.notify_all();
			if (Datas != Fallback_Datas) free(Datas);

		}

		void push(const MsgData& Data) {
			std::unique_lock<std::mutex> lock(Qlock);
			if (full() && !stopTag.load()) {
				// Queue full: drop oldest message instead of blocking
				// (consumer may be stuck on console I/O).
				++head;
			}
			Datas[tail % Capacity] = Data;
			++tail;
			cv_empty.notify_one();
		}

		bool pop(MsgData& Data) {
			std::unique_lock<std::mutex> lock(Qlock);
			cv_empty.wait(lock, [this]{return !empty() || stopTag.load();});
			if (stopTag.load() || empty()) {
				return false;
			}
			Data = Datas[head % Capacity];
			++head;
			cv_full.notify_one();
			return true;
		}

		bool empty() const {
			return head.load() == tail.load();
		}

		bool full() const {
			return (tail.load() - head.load()) >= Capacity;
		}

	private:
		static constexpr size_t Capacity = 128;
		MsgData*  Datas {nullptr};
		MsgData Fallback_Datas[16];
		std::atomic_size_t head{0}, tail{0};
		std::condition_variable cv_empty;
		std::condition_variable cv_full;
		std::mutex Qlock;
	};

	static MsgQue* MessageQue = nullptr;


	Logger::Logger() {
		MessageQue = static_cast<MsgQue*>(malloc(sizeof(MsgQue)));
		if (MessageQue) {
			new (MessageQue) MsgQue();
		}
		Tlock  = static_cast<std::mutex*>(malloc(sizeof(std::mutex)));
		Cv     = static_cast<std::condition_variable*>(malloc(sizeof(std::condition_variable)));
		MsgThread = static_cast<std::thread*>(malloc(sizeof(std::thread)));
		if (Tlock)  new (Tlock)  std::mutex();
		if (Cv)     new (Cv)     std::condition_variable();
		if (MsgThread) {
			new (MsgThread) std::thread([]() {
			while (true) {
				if (!MessageQue || !Tlock || !Cv) break;
				std::unique_lock<std::mutex> lock(*Tlock);
				Cv->wait(lock, [] { return !MessageQue->empty() || stopTag.load(); });
				if (stopTag.load()) break;
				MsgData Data;
				if (!MessageQue->pop(Data)) continue;
				if (stopTag.load()) break;

#ifndef __ANDROID__
				printf("%s", colors[(int)Data.level]);
#endif
				const char* levelName = "????";				switch (Data.level) {
					case LogLevel::INFO_:  levelName = "INFO";  break;
					case LogLevel::WARN_:  levelName = "WARN";  break;
					case LogLevel::ERROR_: levelName = "ERROR"; break;
					case LogLevel::DEBUG_: levelName = "DEBUG"; break;
				}

				auto now = std::chrono::system_clock::now();
				std::time_t now_time = std::chrono::system_clock::to_time_t(now);

#ifdef _WIN32
				std::tm local_tm{};
				if (::localtime_s(&local_tm, &now_time) != 0) {
					// fallback to UTC
					local_tm = *std::gmtime(&now_time);
				}
#else
				std::tm local_tm{};
				if (!::localtime_r(&now_time, &local_tm)) {
					local_tm = *std::gmtime(&now_time);
				}
#endif

				std::array<char, 5*1024> buffer;
#ifdef __ANDROID__
				// Android：走 logcat。原生的 printf 在 logcat 里只会显示成 tag=stdout 的裸行、
				// 丢掉等级，所以这里按等级发到 "FISIR" tag（同时去掉 ANSI 颜色码）。
				snprintf(buffer.data(), buffer.size(),
					"[%04d-%02d-%02d %02d:%02d:%02d][%s][%s][%s]: %s",
					local_tm.tm_year + 1900,
					local_tm.tm_mon + 1,
					local_tm.tm_mday,
					local_tm.tm_hour,
					local_tm.tm_min,
					local_tm.tm_sec,
					Data.file, Data.function, levelName, Data.Msg);
				static const int androidPrio[] = {
					ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_DEBUG
				};
				__android_log_print(androidPrio[(int)Data.level], "FISIR", "%s", buffer.data());
#else
				snprintf(buffer.data(), buffer.size(),
					"\033[0m[%04d-%02d-%02d %02d:%02d:%02d][%s][%s][%s%s\033[0m]: %s",
					local_tm.tm_year + 1900,
					local_tm.tm_mon + 1,
					local_tm.tm_mday,
					local_tm.tm_hour,
					local_tm.tm_min,
					local_tm.tm_sec,
					Data.file, Data.function,
					colors[(int)Data.level], levelName, Data.Msg);
				printf("%s\n", buffer.data());
				fflush(stdout);
#endif

				// ── 旁路落文件（崩溃安全）────────────────────────────────────────
				// 日志线程是异步的：进程一旦原生崩溃，队列里还没写出去的消息就永远丢了 ——
				// 手机上调这种「启动一会儿就 SIGSEGV」的问题时，丢的恰好是最关键的尾巴。
				// 设了 FISIR_LOG_FILE 就把同样的内容**同步**追加到文件（每行 fflush），
				// Android 的平台层会自动把它指到应用私有目录。
				{
					static FILE* fileSink = nullptr;
					static bool sinkInit = false;
					if (!sinkInit) {
						sinkInit = true;
						if (const char* path = getenv("FISIR_LOG_FILE")) {
							fileSink = fopen(path, "w");
						}
					}
					if (fileSink) {
						// 去掉 ANSI 颜色码再落盘：文件里带转义序列没法读
						std::string plain;
						plain.reserve(256);
						for (const char* p = buffer.data(); *p; ++p) {
							if (*p == '\033') { while (*p && *p != 'm') ++p; continue; }
							plain.push_back(*p);
						}
						fprintf(fileSink, "%s\n", plain.c_str());
						fflush(fileSink);   // 关键：崩溃前必须已落盘
					}
				}
			}
			});
		}

		// atexit: stop the log thread.  Do NOT destroy mutex/cv
		// (their destructors call DeleteCriticalSection which crashes
		// during DLL unload on MinGW).  The OS reclaims everything.
		std::atexit([]() {
			stopTag.store(true);
			if (Cv)        Cv->notify_all();
			if (MsgThread && MsgThread->joinable()) MsgThread->join();
		});
	}

	Logger::~Logger() {
		stopTag.store(true);
		if (Cv)        Cv->notify_all();
		if (MsgThread && MsgThread->joinable()) MsgThread->join();
		// Intentionally leak MessageQue/Tlock/Cv/MsgThread —
		// calling their destructors → DeleteCriticalSection → crash during DLL unload
	}

	Logger& Logger::instance() {
		static Logger logger;
		if (initTag.load()) {
			return logger;
		}
		initTag.store(true);
		return logger;
	}

	void Logger::stop() {
		stopTag.store(true);
		if (Cv)        Cv->notify_all();
		if (MsgThread && MsgThread->joinable()) MsgThread->join();
	}

	void Logger::PushMessageFormat(LogLevel level, std::string_view fmt, std::string_view file, std::string_view function, std::format_args args) {
		if (stopTag.load() || !MessageQue) {
			return;
		}
		std::array<char, 4*1024> buf{};
		char* end = std::vformat_to(buf.data(), fmt, args);
		size_t len = static_cast<size_t>(end - buf.data());
		if (len >= buf.size()) len = buf.size() - 1;
		buf[len] = '\0';
		if (MessageQue) {
			MessageQue->push(MsgData(level, file, function, std::string_view(buf.data())));
			Cv->notify_one();
		}
	}

}