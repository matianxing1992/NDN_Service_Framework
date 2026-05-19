#pragma once

#include "common.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <tuple>
#include <sys/file.h>  // flock
#include <unistd.h>    // close
#include <fcntl.h>     // open
#include <filesystem>
#include <cstdlib>
#include <algorithm>

namespace ndn_service_framework {

class ConfigManager {
public:
  ConfigManager(const std::string& path = "/etc/ndn/ndnsf.conf")
    : m_path(std::getenv("NDNSF_CONFIG") != nullptr
               ? std::getenv("NDNSF_CONFIG")
               : path)
  {
    // 内存数据只用于快速查询，持久化由文件负责
    loadToCache();
  }

  // 加载并增加该组/节点的 sessionId，并持久化写入
  int loadAndIncrement(const std::string& groupName, const std::string& nodeName)
  {
    using Entry = std::tuple<std::string, std::string, int>;
    std::vector<Entry> entries;
    bool updated = false;
    const char* sessionBaseEnv = std::getenv("NDNSF_SESSION_BASE");
    int minimumSessionId = 1;
    if (sessionBaseEnv != nullptr) {
      try {
        minimumSessionId = std::max(1, std::stoi(sessionBaseEnv));
      }
      catch (...) {
        minimumSessionId = 1;
      }
    }
    int newSessionId = minimumSessionId;

    // Step 1: 打开文件并加锁
    int fd = open(m_path.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      NDN_LOG_ERROR("Cannot open config file: " << m_path);
      return 0;
    }

    flock(fd, LOCK_EX); // 加写锁，防止并发写

    // Step 2: 读取原有条目
    {
      std::ifstream ifs(m_path);
      std::string line;
      while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string g, n, s;
        if (!std::getline(iss, g, ',') ||
            !std::getline(iss, n, ',') ||
            !std::getline(iss, s)) {
          continue;
        }

        try {
          int sid = std::stoi(s);
          if (g == groupName && n == nodeName) {
            sid += 1;
            sid = std::max(sid, minimumSessionId);
            newSessionId = sid;
            updated = true;
          }
          entries.emplace_back(g, n, sid);
        }
        catch (...) {
          NDN_LOG_ERROR("Invalid line in config: " << line);
        }
      }
    }

    if (!updated) {
      entries.emplace_back(groupName, nodeName, minimumSessionId);
      newSessionId = minimumSessionId;
    }

    // Step 3: 写回所有条目
    {
      std::ofstream ofs(m_path, std::ios::trunc);
      std::set<std::pair<std::string, std::string>> seen;

      for (const auto& [g, n, sid] : entries) {
        auto key = std::make_pair(g, n);
        if (seen.count(key) == 0) {
          ofs << g << "," << n << "," << sid << "\n";
          seen.insert(key);
        }
      }
    }

    flock(fd, LOCK_UN); // 解锁
    close(fd);          // 关闭文件描述符

    // Step 4: 更新内存缓存（可选）
    m_data[{groupName, nodeName}] = newSessionId;

    return newSessionId;
  }

  // 可选接口：查询 sessionId（仅查询缓存）
  int getSessionId(const std::string& groupName, const std::string& nodeName) const
  {
    auto it = m_data.find({groupName, nodeName});
    return (it != m_data.end()) ? it->second : 0;
  }

private:
  void loadToCache()
  {
    // 确保上级目录存在
    std::filesystem::create_directories(std::filesystem::path(m_path).parent_path());

    std::ifstream ifs(m_path);
    if (!ifs.is_open()) {
      // 创建空文件
      std::ofstream ofs(m_path);
      ofs.close();
      ifs.open(m_path);
    }

    std::string line;
    while (std::getline(ifs, line)) {
      std::istringstream iss(line);
      std::string groupName, nodeName, sessionIdStr;

      if (!std::getline(iss, groupName, ',')) continue;
      if (!std::getline(iss, nodeName, ',')) continue;
      if (!std::getline(iss, sessionIdStr)) continue;

      try {
        int sessionId = std::stoi(sessionIdStr);
        m_data[{groupName, nodeName}] = sessionId;
      }
      catch (...) {
        continue;
      }
    }
  }


private:
  NDN_LOG_MEMBER_DECL();
  std::string m_path;
  std::map<std::pair<std::string, std::string>, int> m_data; // 内存缓存，仅辅助查询
};

} // namespace ndn_service_framework
