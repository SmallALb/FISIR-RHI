#include "Logger.h"
#include <cstdio>
#include <thread>
#include <string>
#include <mutex>
#include <condition_variable>
#include <array>
#include <chrono>

namespace FISIR {


	struct MsgData {
		MsgData():level(LogLevel::INFO_){Msg[0] = '\0';}

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
		char Msg[4*1024];
	};

	class MsgQue {
	public:
		MsgQue() {}

		void push(const MsgData& Data) {
			std::unique_lock<std::mutex> lock(Qlock);
			cv_full.wait(lock, [this]{return !full();});
			Datas[tail % Capacity] = Data;
			++tail;
			cv_empty.notify_one();
		}

		bool pop(MsgData& Data) {
			std::unique_lock<std::mutex> lock(Qlock);
			cv_empty.wait(lock, [this]{return !empty();});
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
		const size_t Capacity = 64;
		std::array<MsgData, 64> Datas;
		std::atomic_size_t head, tail;
		std::condition_variable cv_empty;
		std::condition_variable cv_full;
		std::mutex Qlock;
	}MessageQue;

	static std::mutex Tlock;
	static std::condition_variable cv;
	static std::thread MsgThread;
	static std::atomic_bool stopTag = 0;
	static std::atomic_bool initTag = 0;
	const char* colors[] = {
		"\033[32m", "\033[33m", "\033[31m", "\033[4;36m"
	};


	Logger::Logger() {
		MsgThread = std::move(std::thread([&]() {
			while (1) {
				std::array<char, 5*1024> buffer;
				std::unique_lock<std::mutex> lock(Tlock);
				cv.wait(lock, [] {return !MessageQue.empty() || stopTag; });
				if (stopTag) break;
				MsgData Data;
				if (!MessageQue.pop(Data)) continue;
				printf(colors[(int)Data.level]);
				const char* levelName = [&]()->const char*{
					switch (Data.level) {
					case LogLevel::INFO_: return "INFO"; 
					case LogLevel::WARN_: return "WARN"; 
					case LogLevel::ERROR_: return "ERROR"; 
					case LogLevel::DEBUG_: return "DEBUG"; 
					}
				}();

				auto now = std::chrono::system_clock::now();

				// 转换为 time_t
				std::time_t now_time = std::chrono::system_clock::to_time_t(now);

				// 转换为本地时间（获取年月日时分秒）
				std::tm* local_time = std::localtime(&now_time);

				// 分别获取各个部分
				int year = local_time->tm_year + 1900;   
				int month = local_time->tm_mon + 1;      
				int day = local_time->tm_mday;           
				int hour = local_time->tm_hour;          
				int minute = local_time->tm_min;         
				int second = local_time->tm_sec;         

				snprintf(buffer.data(), buffer.size(), "\033[0mFISIRLOG[%04d-%02d-%02d %02d:%02d:%02d][%s][%s][%s%s\033[0m]: %s",
					year, month, day, hour, minute, second, Data.file, Data.function, colors[(int)Data.level], levelName, Data.Msg);
				printf("%s\n", buffer.data());
				fflush(stdout);
			}
		}));
	}

	Logger::~Logger() {
		stop();
	}

	Logger& Logger::instance() {
		static Logger logger;
		if (initTag) {
			return logger;
		}
		
		initTag = 1;
		return logger;
	}

	void Logger::stop() {
		stopTag = 1;
		cv.notify_one();
		MsgThread.join();
	}

	void Logger::PushMessageFormat(LogLevel level, std::string_view fmt, std::string_view file, std::string_view function, std::format_args args) {
		std::array<char, 4*1024> buffer;
		auto it =  std::vformat_to(buffer.begin(), fmt, args);
		*it = '\0';
		MessageQue.push(MsgData(level, file, function, std::vformat(fmt, args)));
		cv.notify_one();
	}

}