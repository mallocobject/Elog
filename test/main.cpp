#include <elog/logger.hpp>

using namespace elog;

int main() {
    LOG_DEBUG("{}", "hello world");
    LOG_INFO("{}", "hello world");
    LOG_WARN("{}", "hello world");
    LOG_ERROR("{}", "hello world");
    LOG_FATAL("{}", "hello world");
}