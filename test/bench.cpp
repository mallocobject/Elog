#include "elog/logger.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace elog;
using namespace std::chrono_literals;

int main(int argc, char* argv[])
{
	// Set up log path
	std::string log_dir = "./logs";
	std::filesystem::create_directories(log_dir);

	// Clear any existing log files
	for (const auto& entry : std::filesystem::directory_iterator(log_dir))
	{
		std::filesystem::remove(entry.path());
	}

	// Enable file logging
	set_log_path(log_dir, "test", 100 * 1024 * 1024, 1s, 1024);

	// Set log threshold to INFO to include all logs
	set_log_threshold(elog::LogLevel::ERROR);

	const int thread_count = 8;
	const int logs_per_thread = 1000000;
	const int expected_logs = thread_count * logs_per_thread;
	std::vector<std::thread> threads;

	auto start = std::chrono::high_resolution_clock::now();

	for (int i = 0; i < thread_count; ++i)
	{
		threads.emplace_back(
			[logs_per_thread, i]()
			{
				for (int j = 0; j < logs_per_thread; ++j)
				{
					LOG_INFO("Thread {} log {}", i, j);
				}
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	// Give some time for the async logger to flush
	std::this_thread::sleep_for(2s);

	auto end = std::chrono::high_resolution_clock::now();
	auto duration =
		std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
			.count();
	double throughput = (thread_count * logs_per_thread) / (duration / 1000.0);

	// Count the number of logs written
	int actual_logs = 0;
	for (const auto& entry : std::filesystem::directory_iterator(log_dir))
	{
		if (entry.is_regular_file())
		{
			std::ifstream log_file(entry.path());
			std::string line;
			while (std::getline(log_file, line))
			{
				actual_logs++;
			}
		}
	}

	std::cout << "Throughput: " << throughput << " logs/sec" << std::endl;
	std::cout << "Total time: " << duration << " ms" << std::endl;
	std::cout << "Expected logs: " << expected_logs << std::endl;
	std::cout << "Actual logs: " << actual_logs << std::endl;

	if (actual_logs == expected_logs)
	{
		std::cout << "SUCCESS: No logs lost!" << std::endl;
	}
	else
	{
		std::cout << "ERROR: Logs lost! Expected " << expected_logs << ", got "
				  << actual_logs << std::endl;
	}

	return 0;
}