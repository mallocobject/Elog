#ifndef ELOG_TIMESTAMP_H
#define ELOG_TIMESTAMP_H

#include <cstdint>
#include <ostream>
#include <string>
namespace elog
{
extern const int kMicroSecond2Second;

class Timestamp
{
  private:
	int64_t micro_seconds_{0};

  public:
	Timestamp() = default;

	explicit Timestamp(int64_t micro_seconds) : micro_seconds_(micro_seconds)
	{
	}

	~Timestamp() = default;

	static Timestamp now();
	static Timestamp addTime(Timestamp timestamp, double add_seconds);

	std::string toFormattedString(bool date = true, bool time = true) const;

	int64_t microseconds() const
	{
		return micro_seconds_;
	}

	auto operator<=>(const Timestamp& rhs) const = default;

	friend std::ostream& operator<<(std::ostream& os, const Timestamp& ts);
};

std::ostream& operator<<(std::ostream& os, const Timestamp& ts);
} // namespace elog

#endif