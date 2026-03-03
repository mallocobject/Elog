#include "elog/logger.h"
#include "elog/timestamp.h"
#include <iostream>
#include <string>
#include <unistd.h>

using namespace elog;

int main(int argc, char* argv[])
{
	// 日志文件名
	std::string basename = "async_log_bench";

	// 创建 AsyncLogging（flush 间隔 3 秒）
	Logger::initAsyncLogging(LYNX_WEB_SRC_DIR
							 "/log/", // 日志文件存放目录（需提前创建）
							 "basename", // 日志文件名前缀
							 100 * 1024 * 1024, // 单个日志文件滚动大小 (100MB)
							 3 // 后端定期刷盘间隔 (3秒)
	);

	const int totalMessages = 5'000'000; // 总日志条数
	const std::string logLine =
		"Test log message for performance benchmarking. 1234567890 "
		"abcdefghijklmnopqrstuvwxyz\n";
	const size_t logLen = logLine.size();

	Timestamp start = Timestamp::now();

	for (int i = 0; i < totalMessages; ++i)
	{
		LOG_INFO << logLine; // 写日志
	}

	Timestamp end = Timestamp::now();

	double seconds = (end.microseconds() - start.microseconds()) / 1e6;
	double qps = totalMessages / seconds;
	double mbps = (totalMessages * logLen) / (1024.0 * 1024.0) / seconds;

	std::cout << "Total messages: " << totalMessages << "\n";
	std::cout << "Total time: " << seconds << " s\n";
	std::cout << "Throughput: " << qps << " logs/s\n";
	std::cout << "Bandwidth: " << mbps << " MB/s\n";
	// 等待后端写完
	sleep(3);
	Logger::shutdownAsyncLogging();

	return 0;
}
