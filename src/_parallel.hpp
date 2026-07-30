#pragma once

#include <je2be/status.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

namespace je2be {

class Parallel {
public:
  template <class Work, class Result>
  static Status Map(
      std::vector<Work> const &works,
      unsigned int concurrency,
      std::function<std::pair<Result, Status>(Work const &work, int index)> func,
      std::vector<Result> &out) {
    using namespace std;

    unsigned int workerCount = (unsigned int)(std::min)(
        (size_t)(std::max)(1u, concurrency),
        (std::max)(size_t(1), works.size()));
    int numThreads = (int)workerCount - 1;
    std::latch latch(workerCount);

    out.resize(works.size());
    Result *outPtr = out.data();
    Work const *worksPtr = works.data();

    atomic_size_t nextIndex(0);
    mutex mut;
    atomic_bool cancel = false;
    Status status;

    auto action = [&latch, worksPtr, outPtr, &nextIndex, &mut, &cancel, &status, &works, func]() {
      while (!cancel) {
        size_t index = nextIndex.fetch_add(1);
        if (index >= works.size()) {
          break;
        } else {
          auto [ret, st] = func(worksPtr[index], (int)index);
          outPtr[index] = ret;
          if (!st.ok()) {
            lock_guard<mutex> lock(mut);
            if (status.ok()) {
              status = st;
            }
            cancel = true;
            break;
          }
        }
      }
      latch.count_down();
    };

    vector<thread> threads;
    for (int i = 0; i < numThreads; i++) {
      threads.emplace_back(action);
    }
    action();
    latch.wait();

    for (auto &th : threads) {
      th.join();
    }
    return status;
  }

  template <class Work>
  static Status Process(std::vector<Work> const &works, unsigned int concurrency, std::function<Status(Work const &work)> func) {
    using namespace std;
    auto [ret, status] = Reduce<Work, int>(
        works, concurrency, 0, [func](Work const &work) -> pair<int, Status> { return make_pair(0, func(work)); },
        [](int const &, int) {});
    return status;
  }

  template <class Work, class Result>
  static std::pair<Result, Status> Reduce(
      std::vector<Work> const &works,
      unsigned int concurrency,
      Result init,
      std::function<std::pair<Result, Status>(Work const &)> func,
      std::function<void(Result const &, Result &)> join) {
    return Reduce<Work, Result>(
        works, concurrency, [&init]() { return init; }, func, join);
  }

  template <class Work, class Result>
  static std::pair<Result, Status> Reduce(
      std::vector<Work> const &works,
      unsigned int concurrency,
      std::function<Result(void)> zero,
      std::function<std::pair<Result, Status>(Work const &)> func,
      std::function<void(Result const &, Result &)> join) {
    using namespace std;

    unsigned int workerCount = (unsigned int)(std::min)(
        (size_t)(std::max)(1u, concurrency),
        (std::max)(size_t(1), works.size()));
    int numThreads = (int)workerCount - 1;
    std::latch latch(workerCount);

    atomic_size_t nextIndex(0);
    mutex joinMut;
    Result total = zero();
    atomic_bool cancel = false;
    Status status;

    auto action = [&latch, zero, &nextIndex, &joinMut, &works, &func, join, &total, &cancel, &status]() {
      Result sum = zero();
      while (!cancel) {
        size_t index = nextIndex.fetch_add(1);
        if (index < works.size()) {
          auto [result, st] = func(works[index]);
          join(result, sum);
          if (!st.ok()) {
            lock_guard<mutex> lock(joinMut);
            if (status.ok()) {
              status = st;
            }
            cancel = true;
            break;
          }
        } else {
          break;
        }
      }
      {
        lock_guard<mutex> lock(joinMut);
        join(sum, total);
      }
      latch.count_down();
    };

    vector<thread> threads;
    for (int i = 0; i < numThreads; i++) {
      threads.emplace_back(action);
    }
    action();
    latch.wait();

    for (auto &th : threads) {
      th.join();
    }
    return make_pair(total, status);
  }

  static void MergeBool(bool const &from, bool &to) {
    to = from && to;
  }

  template <class T>
  static void MergeVector(std::vector<T> const &from, std::vector<T> &to) {
    std::copy(from.begin(), from.end(), std::back_inserter(to));
  }
};

} // namespace je2be
