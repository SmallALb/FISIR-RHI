#pragma once

#include <format>

#include "../DLLheader.h"



namespace FISIR {

	enum class LogLevelColor {
		Green,
		Yellow,
		Red,
	};


	enum class LogLevel {
		INFO_,
		WARN_,
		ERROR_,
		DEBUG_,
	};

	class EXPORTDLL Logger {
		Logger();
	public:
		~Logger();

		static Logger& instance();

		void stop();

		template<typename... Args>
		void PushMessage(LogLevel level, std::string_view fmt, std::string_view file, std::string_view func, Args&&... args) {
			PushMessageFormat(level, fmt, file, func, std::make_format_args(args...));
		}

		void PushMessageFormat(LogLevel level, std::string_view fmt, std::string_view file, std::string_view function, std::format_args args);
	};

}

#define Info(fmt, ...) FISIR::Logger::instance().PushMessage(FISIR::LogLevel::INFO_, fmt, __FILE__, __FUNCTION__, __VA_ARGS__)
#define Error(fmt, ...) FISIR::Logger::instance().PushMessage(FISIR::LogLevel::ERROR_, fmt, __FILE__, __FUNCTION__, __VA_ARGS__)
#define Warn(fmt, ...) FISIR::Logger::instance().PushMessage(FISIR::LogLevel::WARN_, fmt, __FILE__, __FUNCTION__, __VA_ARGS__)

#ifdef _DEBUG
#define Debug(fmt, ...) FISIR::Logger::instance().PushMessage(FISIR::LogLevel::DEBUG_, fmt, __FILE__, __FUNCTION__, __VA_ARGS__)
#else
#define Debug(fmt, ...)
#endif // _DEBUG

