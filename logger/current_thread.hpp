#ifndef LOGGER_CURRENT_THREAD_HPP
#define LOGGER_CURRENT_THREAD_HPP

#include <cstdint>
#include <functional>
#include <thread>
namespace logger
{
namespace CurrentThread
{
inline uint64_t tid()
{
	static thread_local uint64_t cached_tid =
		std::hash<std::thread::id>{}(std::this_thread::get_id());
	return cached_tid;
}
} // namespace CurrentThread
} // namespace logger

#endif