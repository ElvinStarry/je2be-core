TEST_CASE("parallel") {
  SUBCASE("zero concurrency still runs work") {
    vector<int> work = {1, 2, 3, 4};
    atomic_int count(0);
    Status st = Parallel::Process<int>(work, 0, [&count](int const &) {
      count.fetch_add(1);
      return Status::Ok();
    });
    CHECK(st.ok());
    CHECK(count.load() == work.size());
  }

  SUBCASE("zero concurrency still maps work") {
    vector<int> work = {1, 2, 3, 4};
    vector<int> output;
    Status st = Parallel::Map<int, int>(
        work, 0,
        [](int const &value, int index) {
          return make_pair(value + index, Status::Ok());
        },
        output);
    CHECK(st.ok());
    CHECK(output == vector<int>({1, 3, 5, 7}));
  }

  SUBCASE("Reduce.1.ok") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    auto [ret, st] = Parallel::Reduce<int, int>(
        work, thread::hardware_concurrency(), 0,
        [](int w) -> pair<int, Status> {
          return make_pair(2 * w, Status::Ok());
        },
        [](int const &from, int &to) {
          to += from;
        });
    CHECK(st.ok());
    CHECK(ret == size * 2);
  }
  SUBCASE("Reduce.1.cancel") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    atomic_int count(0);
    auto [ret, st] = Parallel::Reduce<int, int>(
        work, thread::hardware_concurrency(), 0,
        [&](int w) -> pair<int, Status> {
          if (count.fetch_add(1) + 1 == size / 2) {
            return make_pair(0, JE2BE_ERROR);
          } else {
            return make_pair(2 * w, Status::Ok());
          }
        },
        [](int const &from, int &to) {
          to += from;
        });
    CHECK(!st.ok());
  }

  SUBCASE("Reduce.2.ok") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    auto [ret, st] = Parallel::Reduce<int, int>(
        work, thread::hardware_concurrency(),
        []() -> int { return 0; },
        [](int w) -> pair<int, Status> {
          return make_pair(w + 1, Status::Ok());
        },
        [](int const &from, int &to) {
          to += from;
        });
    CHECK(st.ok());
    CHECK(ret == size * 2);
  }
  SUBCASE("Reduce.2.cancel") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    atomic_int count(0);
    auto [ret, st] = Parallel::Reduce<int, int>(
        work, thread::hardware_concurrency(),
        []() -> int { return 0; },
        [&count, size](int w) -> pair<int, Status> {
          if (count.fetch_add(1) + 1 == size / 2) {
            return make_pair(0, JE2BE_ERROR);
          } else {
            return make_pair(w + 1, Status::Ok());
          }
        },
        [](int const &from, int &to) {
          to += from;
        });
    CHECK(!st.ok());
  }

  SUBCASE("Map.ok") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    vector<int> out;
    Status st = Parallel::Map<int, int>(
        work, thread::hardware_concurrency(),
        [](int work, int index) -> pair<int, Status> {
          return make_pair(index, Status::Ok());
        },
        out);
    CHECK(st.ok());
    CHECK(out.size() == size);
    for (size_t i = 0; i < size; i++) {
      CHECK(out[i] == i);
    }
  }
  SUBCASE("Map.cancel") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    vector<int> out;
    Status st = Parallel::Map<int, int>(
        work, thread::hardware_concurrency(),
        [size](int work, int index) -> pair<int, Status> {
          if (index == size / 2) {
            return make_pair(index, JE2BE_ERROR);
          } else {
            return make_pair(index, Status::Ok());
          }
        },
        out);
    CHECK(!st.ok());
  }

  SUBCASE("Process.ok") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    atomic_int count(0);
    Status st = Parallel::Process<int>(
        work, thread::hardware_concurrency(),
        [&count](int const &w) -> Status {
          count.fetch_add(1);
          return Status::Ok();
        });
    CHECK(st.ok());
    CHECK(count.load() == size);
  }
  SUBCASE("Process.cancel") {
    size_t size = 2000;
    vector<int> work;
    work.resize(size, 1);
    atomic_int count(0);
    Status st = Parallel::Process<int>(
        work, thread::hardware_concurrency(),
        [&count, size](int const &w) -> Status {
          if (count.fetch_add(1) + 1 == size / 2) {
            return JE2BE_ERROR;
          } else {
            return Status::Ok();
          }
        });
    CHECK(!st.ok());
  }
}
