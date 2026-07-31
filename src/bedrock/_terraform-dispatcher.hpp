#pragma once

#include <je2be/pos2.hpp>
#include <je2be/status.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace je2be::bedrock {

class TerraformDispatcher {
  struct Work {
    Pos2i fRegion;
    size_t fWeight;
  };

  struct WorkLess {
    bool operator()(Work const &a, Work const &b) const {
      if (a.fWeight != b.fWeight) {
        return a.fWeight < b.fWeight;
      }
      return a.fRegion.fZ == b.fRegion.fZ ? a.fRegion.fX > b.fRegion.fX : a.fRegion.fZ > b.fRegion.fZ;
    }
  };

public:
  TerraformDispatcher(
      unsigned concurrency,
      std::function<size_t(Pos2i const &)> weight,
      std::function<Status(Pos2i const &)> process,
      std::function<Status(Pos2i const &)> complete)
      : fWeight(std::move(weight)), fProcess(std::move(process)), fComplete(std::move(complete)) {
    unsigned const workerCount = (std::max)(1u, concurrency);
    fWorkers.reserve(workerCount);
    for (unsigned i = 0; i < workerCount; i++) {
      fWorkers.emplace_back([this]() { run(); });
    }
  }

  ~TerraformDispatcher() {
    {
      std::lock_guard<std::mutex> lock(fMutex);
      fStopping = true;
      fQueue = {};
    }
    fCondition.notify_all();
    for (std::thread &worker : fWorkers) {
      worker.join();
    }
  }

  Status enqueue(std::vector<Pos2i> const &regions) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fStatus.ok()) {
      return fStatus;
    }
    for (Pos2i const &region : regions) {
      fQueue.push({region, fWeight(region)});
    }
    fCondition.notify_all();
    return Status::Ok();
  }

  Status finish() {
    std::unique_lock<std::mutex> lock(fMutex);
    fConversionsFinished = true;
    fCondition.notify_all();
    fCondition.wait(lock, [this]() { return fQueue.empty() && fActive == 0; });
    return fStatus;
  }

private:
  void run() {
    while (true) {
      Work work;
      {
        std::unique_lock<std::mutex> lock(fMutex);
        fCondition.wait(lock, [this]() { return fStopping || !fQueue.empty() || fConversionsFinished; });
        if (fStopping || (fQueue.empty() && fConversionsFinished)) {
          return;
        }
        if (!fStatus.ok()) {
          fQueue = {};
          if (fActive == 0) {
            fCondition.notify_all();
          }
          return;
        }
        work = fQueue.top();
        fQueue.pop();
        fActive++;
      }

      Status status = fProcess(work.fRegion);
      if (status.ok()) {
        status = fComplete(work.fRegion);
      }

      {
        std::lock_guard<std::mutex> lock(fMutex);
        fActive--;
        if (!status.ok() && fStatus.ok()) {
          fStatus = status;
          fQueue = {};
        }
        fCondition.notify_all();
      }
    }
  }

private:
  std::function<size_t(Pos2i const &)> fWeight;
  std::function<Status(Pos2i const &)> fProcess;
  std::function<Status(Pos2i const &)> fComplete;
  std::priority_queue<Work, std::vector<Work>, WorkLess> fQueue;
  std::vector<std::thread> fWorkers;
  std::mutex fMutex;
  std::condition_variable fCondition;
  Status fStatus;
  size_t fActive = 0;
  bool fConversionsFinished = false;
  bool fStopping = false;
};

} // namespace je2be::bedrock
