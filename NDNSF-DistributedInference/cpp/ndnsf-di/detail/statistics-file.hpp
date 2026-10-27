#ifndef NDNSF_DI_DETAIL_STATISTICS_FILE_HPP
#define NDNSF_DI_DETAIL_STATISTICS_FILE_HPP

#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace ndnsf::di::detail {

/** Owned synchronous JSONL output; no queue or worker. Callers supply one row. */
class StatisticsFile
{
public:
  explicit StatisticsFile(const std::string& path)
    : m_output(path, std::ios::out | std::ios::app)
  {
    if (!m_output) throw std::runtime_error("cannot open statistics file: " + path);
  }

  void
  write(const std::string& record)
  {
    if (record.empty() || record.find_first_of("\r\n") != std::string::npos)
      throw std::invalid_argument("statistics record must occupy one line");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_output << record << '\n';
    m_output.flush();
    if (!m_output) throw std::runtime_error("statistics file write failed");
  }

private:
  std::ofstream m_output;
  std::mutex m_mutex;
};

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_STATISTICS_FILE_HPP
