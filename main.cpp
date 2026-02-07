#define LOGGER_LEVEL_SETTING logger::TRACE

// #include "logger/logger.h"

// int main(int argc, char* argv[])
// {
// 	logger::Logger::initAsyncLogging(LYNX_WEB_SRC_DIR "/log", argv[0]);

// 	LOG_TRACE << "Hello world!";
// 	LOG_DEBUG << "Hello world!";
// 	LOG_INFO << "Hello world!";
// 	LOG_WARN << "Hello world!";
// 	LOG_ERROR << "Hello world!";
// 	LOG_FATAL << "Hello world!";

// 	logger::Logger::shutdownAsyncLogging();

// 	return 0;
// }

// logger_stress_test.cpp
#include "logger/logger.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

// 硬核压力测试配置
constexpr int THREAD_COUNT = 16;		// 线程数
constexpr int LOGS_PER_THREAD = 500000; // 每个线程写入的日志数量
constexpr int MESSAGE_SIZE = 128;		// 每条消息的大小（字符数）
constexpr bool USE_ASYNC = true;		// 使用异步日志

// 全局统计
std::atomic<int64_t> total_logs_written{0};
std::atomic<int64_t> total_bytes_written{0};
std::atomic<int> completed_threads{0};

// 测试消息生成
std::string generate_message(int thread_id, int log_id)
{
	thread_local static const char charset[] = "0123456789"
											   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
											   "abcdefghijklmnopqrstuvwxyz";

	char buffer[MESSAGE_SIZE];
	int len = MESSAGE_SIZE - 1;

	// 固定前缀
	int offset = snprintf(buffer, 20, "[T%d-L%d] ", thread_id, log_id);

	// 随机填充
	for (int i = offset; i < len; ++i)
	{
		buffer[i] = charset[rand() % (sizeof(charset) - 1)];
	}
	buffer[len] = '\0';

	return buffer;
}

// 工作线程
void worker_thread(int thread_id)
{
	// 设置随机种子
	srand(time(nullptr) + thread_id);

	// 预热一下
	for (int i = 0; i < 100; ++i)
	{
		LOG_INFO << "Thread " << thread_id << " warming up";
	}

	// 开始压力测试
	auto start_time = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < LOGS_PER_THREAD; ++i)
	{
		// 生成随机日志级别
		int level_rand = rand() % 100;
		logger::LogLevel level;

		if (level_rand < 5)
			level = logger::FATAL;
		else if (level_rand < 15)
			level = logger::ERROR;
		else if (level_rand < 30)
			level = logger::WARN;
		else if (level_rand < 60)
			level = logger::INFO;
		else if (level_rand < 85)
			level = logger::DEBUG;
		else
			level = logger::TRACE;

		// 生成消息
		std::string msg = generate_message(thread_id, i);

		// 写入日志
		switch (level)
		{
		case logger::TRACE:
			LOG_TRACE << msg;
			break;
		case logger::DEBUG:
			LOG_DEBUG << msg;
			break;
		case logger::INFO:
			LOG_INFO << msg;
			break;
		case logger::WARN:
			LOG_WARN << msg;
			break;
		case logger::ERROR:
			LOG_ERROR << msg;
			break;
		case logger::FATAL:
			LOG_FATAL << msg;
			break;
		}

		// 统计
		total_logs_written++;
		total_bytes_written += msg.length() + 30; // 加上日志头大概30个字符

		// 偶尔加一点延迟，模拟真实场景
		if (rand() % 10000 == 0)
		{
			std::this_thread::sleep_for(std::chrono::microseconds(10));
		}
	}

	auto end_time = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
						end_time - start_time)
						.count();

	// 线程完成
	completed_threads++;

	// 输出线程结果
	std::cout << "Thread " << thread_id << " done: " << LOGS_PER_THREAD
			  << " logs in " << duration << " ms ("
			  << (LOGS_PER_THREAD * 1000.0 / duration) << " logs/sec)"
			  << std::endl;
}

// 内存监控线程
void memory_monitor()
{
	constexpr int INTERVAL_MS = 1000;

	while (completed_threads < THREAD_COUNT)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_MS));

		int64_t logs = total_logs_written.load();
		int64_t bytes = total_bytes_written.load();

		std::cout << "[MONITOR] Logs: " << logs << " ("
				  << (logs * 100.0 / (THREAD_COUNT * LOGS_PER_THREAD)) << "%) "
				  << "Bytes: " << (bytes / (1024 * 1024)) << " MB" << std::endl;
	}
}

// 主测试函数
void run_stress_test()
{
	std::cout << "=== LOGGER STRESS TEST ===" << std::endl;
	std::cout << "Threads: " << THREAD_COUNT << std::endl;
	std::cout << "Logs per thread: " << LOGS_PER_THREAD << std::endl;
	std::cout << "Total logs: " << (THREAD_COUNT * LOGS_PER_THREAD)
			  << std::endl;
	std::cout << "Async logging: " << (USE_ASYNC ? "ON" : "OFF") << std::endl;
	std::cout << "Message size: " << MESSAGE_SIZE << " chars" << std::endl;
	std::cout << "=================================" << std::endl;

	// 初始化日志系统
	if (USE_ASYNC)
	{
		logger::Logger::initAsyncLogging(LYNX_WEB_SRC_DIR "/log",
										 "stress_test");
	}

	// 预热
	std::cout << "Warming up..." << std::endl;
	for (int i = 0; i < 1000; ++i)
	{
		LOG_INFO << "Warmup message " << i;
	}

	// 启动监控线程
	std::thread monitor(memory_monitor);

	// 启动工作线程
	std::vector<std::thread> threads;
	threads.reserve(THREAD_COUNT);

	std::cout << "Starting " << THREAD_COUNT << " threads..." << std::endl;
	auto start_time = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < THREAD_COUNT; ++i)
	{
		threads.emplace_back(worker_thread, i);
	}

	// 等待所有线程完成
	for (auto& thread : threads)
	{
		thread.join();
	}

	auto end_time = std::chrono::high_resolution_clock::now();
	auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
							  end_time - start_time)
							  .count();

	// 等待监控线程
	monitor.join();

	// 停止日志系统
	if (USE_ASYNC)
	{
		std::cout << "Shutting down async logging..." << std::endl;
		logger::Logger::shutdownAsyncLogging();
	}

	// 输出最终结果
	std::cout << "\n=== TEST RESULTS ===" << std::endl;
	std::cout << "Total logs written: " << total_logs_written << std::endl;
	std::cout << "Total bytes written: "
			  << (total_bytes_written / (1024 * 1024)) << " MB" << std::endl;
	std::cout << "Total time: " << total_duration << " ms" << std::endl;
	std::cout << "Logs per second: "
			  << (total_logs_written * 1000.0 / total_duration) << std::endl;
	std::cout << "MB per second: "
			  << (total_bytes_written / (1024.0 * 1024.0) * 1000.0 /
				  total_duration)
			  << std::endl;
	std::cout << "Average log size: "
			  << (total_bytes_written / (double)total_logs_written) << " bytes"
			  << std::endl;
}

int main(int argc, char* argv[])
{
	try
	{
		run_stress_test();
	}
	catch (const std::exception& e)
	{
		std::cerr << "Test failed with exception: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}