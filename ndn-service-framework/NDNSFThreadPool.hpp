#ifndef NDNSF_THREAD_POOL_HPP
#define NDNSF_THREAD_POOL_HPP

#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <utility>
#include <optional>
#include <cstddef>

#include <boost/version.hpp>

#if BOOST_VERSION >= 107000
  #include <boost/asio/io_context.hpp>
  #include <boost/asio/executor_work_guard.hpp>
  #include <boost/asio/steady_timer.hpp>
  #include <boost/asio/post.hpp>
#else
  #include <boost/asio/io_service.hpp>
  #include <boost/asio.hpp>
#endif

// 内部细节命名空间：兼容不同 Boost 版本
namespace ndnsf_detail {

#if BOOST_VERSION >= 107000
  using IoCtx     = boost::asio::io_context;
  using WorkGuard = boost::asio::executor_work_guard<IoCtx::executor_type>;
#else
  using IoCtx     = boost::asio::io_service;
  // 旧版使用 io_service::work 保持 run() 不退出
  using Work      = IoCtx::work;
#endif

} // namespace ndnsf_detail

namespace ndnsf {

// ----------------------------------------------------------------------
// 共享线程池（单例）
//  - header-only 实现，无需 .cpp
//  - 线程数默认 std::thread::hardware_concurrency()
//  - 任意地方可以：NdnsfThreadPool::instance().post([]{ ... });
//    或：ndnsf::post([]{ ... });
// ----------------------------------------------------------------------
class ThreadPool
{
public:
  static ThreadPool& instance()
  {
    static ThreadPool inst;
    return inst;
  }

  // 幂等：可安全多次调用；只在第一次真正启动线程池
  void start(std::size_t threadCount = std::thread::hardware_concurrency())
  //void start(std::size_t threadCount = 1)
  {
    std::call_once(m_startFlag, [this, threadCount] () mutable {
      if (threadCount == 0) {
        threadCount = 1;
      }

#if BOOST_VERSION >= 107000
      m_workGuard.emplace(boost::asio::make_work_guard(m_io));
#else
      m_work.reset(new ndnsf_detail::Work(m_io));
#endif

      m_threads.reserve(threadCount);
      for (std::size_t i = 0; i < threadCount; ++i) {
        m_threads.emplace_back([this] {
          m_io.run();
        });
      }
    });
  }

  // 提交一个任务到线程池，F 可以是 lambda / std::function 等
  template<typename F>
  void post(F&& f)
  {
//     // 确保线程池已启动（多次调用无问题）
//     start();

// #if BOOST_VERSION >= 107000
//     boost::asio::post(m_io, std::forward<F>(f));
// #else
//     m_io.post(std::forward<F>(f));
// #endif
  f();
  }

  // 如需直接访问 io_context（例如定时器等），可以用这个
  ndnsf_detail::IoCtx& context()
  {
    return m_io;
  }

private:
  ThreadPool() = default;

  // 注意：这里没有在析构时 stop/join 线程，
  // 作为进程级共享线程池，通常让它跟随进程一起退出。
  ~ThreadPool() = default;

  ThreadPool(const ThreadPool&)            = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

private:
  ndnsf_detail::IoCtx m_io;

#if BOOST_VERSION >= 107000
  std::optional<ndnsf_detail::WorkGuard> m_workGuard;
#else
  std::unique_ptr<ndnsf_detail::Work>    m_work;
#endif

  std::vector<std::thread> m_threads;
  std::once_flag           m_startFlag;
};

// 方便使用的别名：和之前回答的类名保持兼容
using NdnsfThreadPool = ThreadPool;

// ----------------------------------------------------------------------
// 辅助函数：直接 ndnsf::post([]{ ... }); 即可
// ----------------------------------------------------------------------
template<typename F>
inline void post(F&& f)
{
  ThreadPool::instance().post(std::forward<F>(f));
}

} // namespace ndnsf

#endif // NDNSF_THREAD_POOL_HPP
