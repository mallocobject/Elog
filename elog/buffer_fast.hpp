#ifndef ELOG_BUFFER_HPP
#define ELOG_BUFFER_HPP

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
namespace elog
{
namespace details
{
template <std::size_t N> struct Buffer
{
	using iterator = std::string*;
	using const_iterator = const std::string*;

	Buffer() : data_(std::make_unique<std::string[]>(N))
	{
	}

	Buffer(Buffer&& other) noexcept
	{
		data_ = std::exchange(other.data_, nullptr);
		idx_.store(other.idx_.load(std::memory_order_relaxed),
				   std::memory_order_relaxed);
		other.idx_.store(0, std::memory_order_relaxed);
	}

	Buffer& operator=(Buffer&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}

		assert(data_ == nullptr);

		std::swap(data_, other.data_);
		idx_.store(other.idx_.load(std::memory_order_relaxed),
				   std::memory_order_relaxed);
		other.idx_.store(0, std::memory_order_relaxed);
		return *this;
	}

	std::size_t size() const noexcept
	{
		return idx_;
	}

	bool check() const noexcept
	{
		return data_ != nullptr;
	}

	std::size_t capacity() const noexcept
	{
		return N;
	}

	bool empty() const noexcept
	{
		return idx_.load(std::memory_order_acquire) == 0;
	}

	bool full() const noexcept
	{
		return idx_.load(std::memory_order_acquire) == N;
	}

	void clear()
	{
		idx_.store(0, std::memory_order_release);
	}

	bool push(const std::string& msg)
	{
		size_t old = idx_.load(std::memory_order_relaxed);
		while (true)
		{
			if (old >= N || !data_)
			{
				return false;
			}
			if (idx_.compare_exchange_weak(old, old + 1,
										   std::memory_order_acq_rel,
										   std::memory_order_relaxed))
			{
				if (!data_)
				{
					return false;
				}
				data_[old] = msg;
				std::atomic_thread_fence(std::memory_order_release);
				return true;
			}
		}
	}

	bool push(std::string&& msg)
	{
		size_t old = idx_.load(std::memory_order_relaxed);
		while (true)
		{
			if (old >= N || !data_)
			{
				return false;
			}
			if (idx_.compare_exchange_weak(old, old + 1,
										   std::memory_order_acq_rel,
										   std::memory_order_relaxed))
			{
				if (!data_)
				{
					return false;
				}
				data_[old] = std::move(msg);
				std::atomic_thread_fence(std::memory_order_release);
				return true;
			}
		}
	}

	iterator begin() noexcept
	{
		return data_ ? data_.get() : nullptr;
	}
	iterator end() noexcept
	{
		return data_ ? data_.get() + idx_.load(std::memory_order_relaxed)
					 : nullptr;
	}

	const_iterator begin() const noexcept
	{
		return data_ ? data_.get() : nullptr;
	}
	const_iterator end() const noexcept
	{
		return data_ ? data_.get() + idx_.load(std::memory_order_relaxed)
					 : nullptr;
	}

  private:
	std::unique_ptr<std::string[]> data_;
	std::atomic<std::size_t> idx_{0};
};
} // namespace details
} // namespace elog

#endif