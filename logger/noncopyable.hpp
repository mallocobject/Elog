#ifndef LOGGER_NONCOPYABLE_HPP
#define LOGGER_NONCOPYABLE_HPP

namespace logger
{
class noncopyable
{
  protected:
	noncopyable() = default;
	~noncopyable() = default;

	noncopyable(const noncopyable&) = delete;
	noncopyable& operator=(const noncopyable&) = delete;
};
} // namespace logger

#endif