#ifndef LOGGER_LOGGER_H
#define LOGGER_LOGGER_H

#include "logger/current_thread.hpp"
#include "logger/noncopyable.hpp"
#include "logger/time_stamp.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string_view>
#include <type_traits>
namespace logger
{

enum LogLevel
{
	TRACE = 0,
	DEBUG,
	INFO,
	WARN,
	ERROR,
	FATAL
};

constexpr std::string_view logLevel2String(LogLevel level)
{
	switch (level)
	{
	case TRACE:
		return "TRACE";
	case DEBUG:
		return "DEBUG";
	case INFO:
		return "INFO";
	case WARN:
		return "WARN";
	case ERROR:
		return "ERROR";
	case FATAL:
		return "FATAL";
	default:
		return "UNKNOWN";
	}
}

constexpr const char* logLevel2Color(LogLevel level)
{
	switch (level)
	{
	case TRACE:
		return "\033[90m";
	case DEBUG:
		return "\033[36m";
	case INFO:
		return "\033[32m";
	case WARN:
		return "\033[33m";
	case ERROR:
		return "\033[31m";
	case FATAL:
		return "\033[35m";
	default:
		return "\033[0m";
	}
}
} // namespace logger

template <>
struct std::formatter<logger::LogLevel> : std::formatter<std::string_view>
{
	auto format(logger::LogLevel level, std::format_context& ctx) const
	{
		return std::formatter<std::string_view>::format(
			logger::logLevel2String(level), ctx);
	}
};

namespace logger
{

class NullLogger : public noncopyable
{
  public:
	NullLogger(LogLevel, const char*, const char*, int)
	{
	}

	template <typename T> NullLogger& operator<<(const T&)
	{
		return *this;
	}

	NullLogger& operator<<(std::ostream& (*)(std::ostream&))
	{
		return *this;
	}
};

// inline NullLogger null_logger;

class AsyncLogging;
class Logger : public noncopyable
{

  private:
	static std::unique_ptr<AsyncLogging> async_logging_;
	static std::atomic<bool> async_enabled_;
	// static std::mutex mtx_;

  public:
	/**
	 * 初始化异步日志系统
	 * @param log_file_base 日志文件路径以及前缀
	 * @param roll_size 日志文件滚动大小（字节），默认100MB
	 * @param flush_interval 缓冲区刷新间隔（秒），默认3秒
	 */
	static void initAsyncLogging(const std::string& log_file_base,
								 const std::string& exe_name,
								 int roll_size = 100 * 1024 * 1024,
								 int flush_interval = 3);

	static void shutdownAsyncLogging();

	static bool isAsyncEnabled();

  private:
	static void appendAsyncLog(LogLevel level, const std::string& message,
							   const char* file, const char* func, int line);

  public:
	class LogStream : noncopyable
	{
	  private:
		LogLevel level_;
		std::ostringstream stream_;

		const char* file_;
		const char* func_;
		int line_;

	  public:
		LogStream(LogLevel level, const char* file, const char* func, int line)
			: level_(level), file_(file), func_(func), line_(line)
		{
		}

		~LogStream()
		{
			if (!stream_.str().empty())
			{
				if (Logger::isAsyncEnabled())
				{
					Logger::appendAsyncLog(level_, stream_.str(), file_, func_,
										   line_);
				}
				else
				{
					std::string formatted_log = std::format(
						"{}{}[{}]{} {}:{} {}()-> {}\033[0m\n",
						logLevel2Color(level_),
						TimeStamp::now().toFormattedString(),
						CurrentThread::tid(), level_, getShortName(file_),
						line_, func_, stream_.str());

					std::cout << formatted_log;
				}
			}
		}

		template <typename T> LogStream& operator<<(const T& val)
		{
			stream_ << val;
			return *this;
		}

		LogStream& operator<<(std::ostream& (*manip)(std::ostream))
		{
			stream_ << manip;
			return *this;
		}

		static const char* getShortName(const char* file)
		{
			assert(file);
			const char* short_name = std::strrchr(file, '/');
			if (short_name)
			{
				short_name++;
			}
			else
			{
				short_name = file;
			}

			return short_name;
		}
	};
};

#ifndef LOGGER_LEVEL_SETTING
#define LOGGER_LEVEL_SETTING logger::INFO
#endif

constexpr LogLevel GLOBAL_MIN_LEVEL =
	static_cast<LogLevel>(LOGGER_LEVEL_SETTING);

template <LogLevel level>
using SelectedLogStream = std::conditional_t<level >= GLOBAL_MIN_LEVEL,
											 Logger::LogStream, NullLogger>;

} // namespace logger

#define LOG_TRACE                                                              \
	logger::SelectedLogStream<logger::TRACE>(logger::TRACE, __FILE__,          \
											 __func__, __LINE__)
#define LOG_DEBUG                                                              \
	logger::SelectedLogStream<logger::DEBUG>(logger::DEBUG, __FILE__,          \
											 __func__, __LINE__)
#define LOG_INFO                                                               \
	logger::SelectedLogStream<logger::INFO>(logger::INFO, __FILE__, __func__,  \
											__LINE__)
#define LOG_WARN                                                               \
	logger::SelectedLogStream<logger::WARN>(logger::WARN, __FILE__, __func__,  \
											__LINE__)
#define LOG_ERROR                                                              \
	logger::SelectedLogStream<logger::ERROR>(logger::ERROR, __FILE__,          \
											 __func__, __LINE__)
#define LOG_FATAL                                                              \
	logger::SelectedLogStream<logger::FATAL>(logger::FATAL, __FILE__,          \
											 __func__, __LINE__)

#endif
