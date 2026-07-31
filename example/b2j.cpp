#include <cxxopts.hpp>
#include <je2be.hpp>

#if __has_include(<mimalloc.h>)
#include <mimalloc.h>
#endif
#include <defer.hpp>
#include <pbar.hpp>

#include "cli-world-data-overrides.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/resource.h>
#endif

using namespace std;
using namespace je2be;
using namespace je2be::bedrock;
namespace fs = std::filesystem;

struct StdoutProgressReporter : public Progress {
  struct State {
    optional<Rational<u64>> fConvert;
    u64 fNumConvertedChunks = 0;
    optional<Rational<u64>> fTerraform;
    u64 fNumTerraformedChunks = 0;
  };

  struct ProcessMetrics {
    long long fCpuUserMs = 0;
    long long fCpuSystemMs = 0;
    unsigned long long fMaxRssBytes = 0;
  };

  using Clock = chrono::steady_clock;

private:
  ostream *fProfile = nullptr;
  Clock::time_point fStart;

public:

  explicit StdoutProgressReporter(ostream *profile = nullptr)
      : fProfile(profile), fStart(Clock::now()) {
    if (fProfile) {
      *fProfile << "elapsed_s,convert_done,convert_total,converted_chunks,terraform_done,terraform_total,terraformed_chunks,phase,convert_rate_chunks_s,terraform_rate_chunks_s,total_rate_chunks_s,cpu_user_ms,cpu_system_ms,max_rss_bytes\n";
      fProfile->flush();
    }
    fIo.reset(new thread([this]() {
      State prev;
      State profilePrev;
      auto profilePrevAt = fStart;
      bool profileFirst = true;
      unique_ptr<pbar::pbar> convert;
      unique_ptr<pbar::pbar> terraform;

      while (!fStop) {
        State s;
        {
          lock_guard<mutex> lock(fMut);
          s = fState;
        }
        if (s.fConvert) {
          if (!convert) {
            convert.reset(new pbar::pbar(s.fConvert->fDen, "Convert"));
            convert->enable_recalc_console_width(10);
          }
          if (prev.fConvert) {
            if (prev.fConvert->fNum < prev.fConvert->fDen && s.fConvert->fNum > prev.fConvert->fNum) {
              convert->tick(s.fConvert->fNum - prev.fConvert->fNum);
            }
          } else {
            convert->tick(s.fConvert->fNum);
          }
        }
        if (s.fTerraform) {
          if (!terraform) {
            terraform.reset(new pbar::pbar(s.fTerraform->fDen, "Terraform"));
            terraform->enable_recalc_console_width(10);
          }
          if (prev.fTerraform) {
            if (prev.fTerraform->fNum < prev.fTerraform->fDen && s.fTerraform->fNum > prev.fTerraform->fNum) {
              terraform->tick(s.fTerraform->fNum - prev.fTerraform->fNum);
            }
          } else {
            terraform->tick(s.fTerraform->fNum);
          }
        }
        prev = s;

        auto now = Clock::now();
        if (fProfile && (profileFirst || now - profilePrevAt >= chrono::milliseconds(250))) {
          writeProfile(now, s, profilePrev, profilePrevAt);
          profilePrev = s;
          profilePrevAt = now;
          profileFirst = false;
        }
        this_thread::sleep_for(chrono::milliseconds(16));
      }

      if (fProfile) {
        State s;
        {
          lock_guard<mutex> lock(fMut);
          s = fState;
        }
        auto now = Clock::now();
        writeProfile(now, s, profilePrev, profilePrevAt);
      }
    }));
  }

  ~StdoutProgressReporter() {
    fStop = true;
    fIo->join();
    fIo.reset();
  }

  bool reportConvert(Rational<u64> const &progress, u64 numConvertedChunks) override {
    lock_guard<mutex> lock(fMut);
    if (progress.fDen > 0) {
      if (fState.fConvert) {
        fState.fConvert->fNum = std::max(fState.fConvert->fNum, progress.fNum);
      } else {
        fState.fConvert = progress;
      }
    }
    fState.fNumConvertedChunks = numConvertedChunks;
    return true;
  }

  bool reportTerraform(Rational<u64> const &progress, u64 numProcessedChunks) override {
    lock_guard<mutex> lock(fMut);
    if (progress.fDen > 0) {
      if (fState.fTerraform) {
        fState.fTerraform->fNum = std::max(fState.fTerraform->fNum, progress.fNum);
      } else {
        fState.fTerraform = progress;
      }
    }
    fState.fNumTerraformedChunks = numProcessedChunks;
    return true;
  }

  mutex fMut;
  unique_ptr<thread> fIo;
  State fState;
  atomic_bool fStop = false;

private:
  static u64 progressNum(optional<Rational<u64>> const &progress) {
    return progress ? progress->fNum : 0;
  }

  static u64 progressDen(optional<Rational<u64>> const &progress) {
    return progress ? progress->fDen : 0;
  }

  static ProcessMetrics readProcessMetrics() {
    ProcessMetrics ret;
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
      ret.fCpuUserMs = (long long)usage.ru_utime.tv_sec * 1000 + usage.ru_utime.tv_usec / 1000;
      ret.fCpuSystemMs = (long long)usage.ru_stime.tv_sec * 1000 + usage.ru_stime.tv_usec / 1000;
#if defined(__APPLE__)
      ret.fMaxRssBytes = (unsigned long long)usage.ru_maxrss;
#else
      ret.fMaxRssBytes = (unsigned long long)usage.ru_maxrss * 1024;
#endif
    }
#endif
    return ret;
  }

  void writeProfile(Clock::time_point now, State const &current, State const &previous, Clock::time_point previousAt) {
    if (!fProfile || !*fProfile) {
      return;
    }

    double const elapsed = chrono::duration<double>(now - fStart).count();
    double const interval = chrono::duration<double>(now - previousAt).count();
    u64 const convertDone = progressNum(current.fConvert);
    u64 const convertTotal = progressDen(current.fConvert);
    u64 const convertedChunks = current.fNumConvertedChunks;
    u64 const terraformDone = progressNum(current.fTerraform);
    u64 const terraformTotal = progressDen(current.fTerraform);
    u64 const terraformedChunks = current.fNumTerraformedChunks;
    u64 const previousConvertDone = progressNum(previous.fConvert);
    u64 const previousTerraformDone = progressNum(previous.fTerraform);
    double const convertRate = interval > 0 ? double(convertDone - previousConvertDone) / interval : 0;
    double const terraformRate = interval > 0 ? double(terraformDone - previousTerraformDone) / interval : 0;
    string phase = "idle";
    if (convertDone > previousConvertDone && terraformDone > previousTerraformDone) {
      phase = "convert+terraform";
    } else if (convertDone > previousConvertDone) {
      phase = "convert";
    } else if (terraformDone > previousTerraformDone) {
      phase = "terraform";
    } else if (terraformTotal > 0 && terraformDone >= terraformTotal) {
      phase = "finalize";
    }

    ProcessMetrics const metrics = readProcessMetrics();
    *fProfile << fixed << setprecision(3)
              << elapsed << ','
              << convertDone << ','
              << convertTotal << ','
              << convertedChunks << ','
              << terraformDone << ','
              << terraformTotal << ','
              << terraformedChunks << ','
              << phase << ','
              << convertRate << ','
              << terraformRate << ','
              << (convertRate + terraformRate) << ','
              << metrics.fCpuUserMs << ','
              << metrics.fCpuSystemMs << ','
              << metrics.fMaxRssBytes << '\n';
    fProfile->flush();
    if (!*fProfile) {
      fProfile = nullptr;
    }
  }
};

int main(int argc, char *argv[]) {
#if __has_include(<mimalloc.h>)
  mi_version();
#endif

  cxxopts::Options parser("b2j");
  parser.add_options()                                                                                                 //
      ("i,input", "input directory", cxxopts::value<string>())                                                         //
      ("o,output", "output directory", cxxopts::value<string>())                                                       //
      ("n", "num threads", cxxopts::value<unsigned int>()->default_value(to_string(thread::hardware_concurrency()))) //
      ("profile", "write periodic runtime metrics to a CSV file", cxxopts::value<string>())                           //
      ("O,override", "override target world data; accepts one or more key=value arguments", cxxopts::value<vector<string>>());

  vector<string> cliArguments;
  vector<WorldDataOverride> overrides;
  string overrideError;
  if (!je2be::cli::ExtractWorldDataOverrides(argc, argv, cliArguments, overrides, overrideError)) {
    cerr << overrideError << endl;
    cerr << parser.help() << endl;
    return -1;
  }
  auto cliArgv = je2be::cli::MutableArgv(cliArguments);
  int cliArgc = static_cast<int>(cliArgv.size());

  cxxopts::ParseResult result;
  try {
    result = parser.parse(cliArgc, cliArgv.data());
  } catch (cxxopts::exceptions::exception &e) {
    cerr << e.what() << endl;
    cerr << parser.help() << endl;
    return -1;
  }

  string inputString = result["i"].as<string>();
  fs::path input(inputString);
  if (!fs::is_directory(input)) {
    cerr << "error: input directory does not exist" << endl;
    return -1;
  }

  string outputString = result["o"].as<string>();
  if (outputString.empty()) {
    cerr << "error: output directory not set" << endl;
    return -1;
  }
  fs::path output(outputString);

  unsigned int concurrency = result["n"].as<unsigned int>();

  ofstream profileFile;
  if (result.count("profile") > 0) {
    string profilePath = result["profile"].as<string>();
    if (profilePath.empty()) {
      cerr << "error: profile path not set" << endl;
      return -1;
    }
    profileFile.open(profilePath, ios::out | ios::trunc);
    if (!profileFile) {
      cerr << "error: failed to open profile path: " << profilePath << endl;
      return -1;
    }
  }

  auto start = chrono::high_resolution_clock::now();
  defer {
    auto elapsed = chrono::high_resolution_clock::now() - start;
    cout << float(chrono::duration_cast<chrono::milliseconds>(elapsed).count() / 1000.0f) << "s" << endl;
  };

  Options options;
  options.fWorldDataOverrides = std::move(overrides);
  options.fTempDirectory = mcfile::File::CreateTempDir(fs::temp_directory_path());
  defer {
    if (options.fTempDirectory) {
      je2be::Fs::DeleteAll(*options.fTempDirectory);
    }
  };
#if 0
  int s = 1;
  options.fDimensionFilter.insert(mcfile::Dimension::Overworld);
  for (int x = -s; x <= s; x++) {
    for (int z = -s; z <= s; z++) {
      options.fChunkFilter.insert({x, z});
    }
  }
#endif
  unique_ptr<StdoutProgressReporter> progress(new StdoutProgressReporter(profileFile ? &profileFile : nullptr));
  auto st = Converter::Run(input, output, options, concurrency, progress.get());
  if (auto err = st.error(); err) {
    cerr << "what: " << err->fWhat << endl;
    cerr << "trace: " << endl;
    for (int i = err->fTrace.size() - 1; i >= 0; i--) {
      cerr << "  " << err->fTrace[i].fFile << ":" << err->fTrace[i].fLine << endl;
    }
    return -1;
  } else {
    cout << progress->fState.fNumConvertedChunks << " chunks converted" << endl;
    cout << progress->fState.fNumTerraformedChunks << " chunks terraformed" << endl;
    return 0;
  }
}
