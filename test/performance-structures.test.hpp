#pragma once

#include "_queue2d.hpp"
#include "bedrock/_terraform-dispatcher.hpp"
#include "bedrock/_terraform-region-scheduler.hpp"
#include "terraform/lighting/_light-cache.hpp"
#include "terraform/lighting/_lighting.hpp"

#include <defer.hpp>
#include <sparse.hpp>

TEST_CASE("performance data structures") {
  SUBCASE("sparse Queue2d does not scan its coordinate bounds") {
    using Queue = Queue2d<0, true, Sparse>;
    Queue queue({-10000, -10000}, 20001, 20001);
    queue.markTask({-10000, -10000}, 1);
    queue.markTask({0, 0}, 3);
    queue.markTask({10000, 10000}, 5);
    queue.markTask({0, 0}, 7);

    auto first = queue.next();
    REQUIRE(first);
    REQUIRE(holds_alternative<Queue::Dequeue>(*first));
    CHECK(get<Queue::Dequeue>(*first).fRegion == Pos2i(0, 0));

    auto second = queue.next();
    REQUIRE(second);
    REQUIRE(holds_alternative<Queue::Dequeue>(*second));
    CHECK(get<Queue::Dequeue>(*second).fRegion == Pos2i(10000, 10000));

    auto third = queue.next();
    REQUIRE(third);
    REQUIRE(holds_alternative<Queue::Dequeue>(*third));
    CHECK(get<Queue::Dequeue>(*third).fRegion == Pos2i(-10000, -10000));
    CHECK_FALSE(queue.next());
  }

  SUBCASE("optimized Queue2d preserves dynamic locking") {
    using Queue = Queue2d<0, true, Sparse>;
    Queue queue({0, 0}, 1, 1);
    queue.markTask({0, 0}, 1);
    auto first = queue.next();
    REQUIRE(first);
    REQUIRE(holds_alternative<Queue::Dequeue>(*first));

    queue.markTask({0, 0}, 2);
    auto busy = queue.next();
    REQUIRE(busy);
    CHECK(holds_alternative<Queue::Busy>(*busy));

    queue.unlock({0, 0});
    auto second = queue.next();
    REQUIRE(second);
    CHECK(holds_alternative<Queue::Dequeue>(*second));
  }

  SUBCASE("isolated Terraform regions release immediately") {
    vector<Pos2i> regions = {{0, 0}, {10000, -10000}, {-10000, 10000}};
    bedrock::TerraformRegionScheduler scheduler(regions);
    CHECK(scheduler.size() == regions.size());
    for (Pos2i const &region : regions) {
      auto ready = scheduler.markConverted(region);
      REQUIRE(ready);
      REQUIRE(ready->size() == 1);
      CHECK(ready->front() == region);

      auto releasable = scheduler.markCompleted(region);
      REQUIRE(releasable);
      REQUIRE(releasable->size() == 1);
      CHECK(releasable->front() == region);
    }
    CHECK(scheduler.allCompleted());
  }

  SUBCASE("row-major Terraform keeps temporary regions to a bounded frontier") {
    int constexpr width = 64;
    int constexpr height = 64;
    vector<Pos2i> regions;
    for (int z = 0; z < height; z++) {
      for (int x = 0; x < width; x++) {
        regions.emplace_back(x, z);
      }
    }
    bedrock::TerraformRegionScheduler scheduler(regions);
    size_t resident = 0;
    size_t maximumResident = 0;
    for (Pos2i const &converted : regions) {
      resident++;
      maximumResident = (std::max)(maximumResident, resident);
      auto ready = scheduler.markConverted(converted);
      REQUIRE(ready);
      for (Pos2i const &target : *ready) {
        auto releasable = scheduler.markCompleted(target);
        REQUIRE(releasable);
        REQUIRE(resident >= releasable->size());
        resident -= releasable->size();
      }
    }
    CHECK(scheduler.allCompleted());
    CHECK(resident == 0);
    CHECK(maximumResident < width * 4);
  }

  SUBCASE("dense Terraform regions wait for all existing neighbors") {
    vector<Pos2i> regions;
    for (int z = 0; z < 3; z++) {
      for (int x = 0; x < 3; x++) {
        regions.emplace_back(x, z);
      }
    }
    bedrock::TerraformRegionScheduler scheduler(regions);
    for (Pos2i const &region : regions) {
      if (region == Pos2i(1, 1)) {
        continue;
      }
      auto ready = scheduler.markConverted(region);
      REQUIRE(ready);
      CHECK(ready->empty());
    }

    auto ready = scheduler.markConverted({1, 1});
    REQUIRE(ready);
    CHECK(ready->size() == regions.size());
    unordered_set<Pos2i, Pos2iHasher> scheduled(ready->begin(), ready->end());
    CHECK(scheduled.size() == regions.size());

    unordered_set<Pos2i, Pos2iHasher> released;
    for (Pos2i const &region : *ready) {
      auto releasable = scheduler.markCompleted(region);
      REQUIRE(releasable);
      for (Pos2i const &source : *releasable) {
        CHECK(released.insert(source).second);
      }
    }
    CHECK(released.size() == regions.size());
    CHECK(scheduler.allCompleted());
    CHECK_FALSE(scheduler.markConverted({1, 1}));
    CHECK_FALSE(scheduler.markCompleted({1, 1}));
  }

  SUBCASE("Terraform scheduler is safe under concurrent completion") {
    vector<Pos2i> regions;
    for (int z = 0; z < 32; z++) {
      for (int x = 0; x < 32; x++) {
        regions.emplace_back(x, z);
      }
    }
    bedrock::TerraformRegionScheduler scheduler(regions);
    atomic_size_t next(0);
    atomic_bool ok(true);
    mutex resultMutex;
    unordered_set<Pos2i, Pos2iHasher> scheduled;
    unordered_set<Pos2i, Pos2iHasher> released;
    vector<thread> workers;
    for (int i = 0; i < 8; i++) {
      workers.emplace_back([&]() {
        while (true) {
          size_t index = next.fetch_add(1);
          if (index >= regions.size()) {
            return;
          }
          auto ready = scheduler.markConverted(regions[index]);
          if (!ready) {
            ok = false;
            return;
          }
          for (Pos2i const &target : *ready) {
            auto releasable = scheduler.markCompleted(target);
            if (!releasable) {
              ok = false;
              return;
            }
            lock_guard<mutex> lock(resultMutex);
            ok = ok && scheduled.insert(target).second;
            for (Pos2i const &source : *releasable) {
              ok = ok && released.insert(source).second;
            }
          }
        }
      });
    }
    for (thread &worker : workers) {
      worker.join();
    }
    CHECK(ok.load());
    CHECK(scheduled.size() == regions.size());
    CHECK(released.size() == regions.size());
    CHECK(scheduler.allCompleted());
  }

  SUBCASE("Terraform dispatcher runs asynchronously in descending weight order") {
    thread::id const caller = this_thread::get_id();
    unordered_map<Pos2i, size_t, Pos2iHasher> weights = {
        {{1, 0}, 1},
        {{2, 0}, 5},
        {{3, 0}, 3},
    };
    vector<Pos2i> processed;
    vector<Pos2i> completed;
    bool asynchronous = true;

    bedrock::TerraformDispatcher dispatcher(
        1,
        [&weights](Pos2i const &region) { return weights.at(region); },
        [&processed, &asynchronous, caller](Pos2i const &region) {
          asynchronous = asynchronous && this_thread::get_id() != caller;
          processed.push_back(region);
          return Status::Ok();
        },
        [&completed](Pos2i const &region) {
          completed.push_back(region);
          return Status::Ok();
        });
    REQUIRE(dispatcher.enqueue({{1, 0}, {2, 0}, {3, 0}}).ok());
    REQUIRE(dispatcher.finish().ok());
    CHECK(asynchronous);
    CHECK(processed == vector<Pos2i>({{2, 0}, {3, 0}, {1, 0}}));
    CHECK(completed == processed);
  }

  SUBCASE("Terraform dispatcher propagates worker errors") {
    atomic_int completed(0);
    bedrock::TerraformDispatcher dispatcher(
        1,
        [](Pos2i const &region) { return region.fX == 2 ? 10 : 1; },
        [](Pos2i const &region) { return region.fX == 2 ? JE2BE_ERROR_WHAT("expected") : Status::Ok(); },
        [&completed](Pos2i const &) {
          completed++;
          return Status::Ok();
        });
    REQUIRE(dispatcher.enqueue({{1, 0}, {2, 0}, {3, 0}}).ok());
    CHECK_FALSE(dispatcher.finish().ok());
    CHECK(completed.load() == 0);
  }

  SUBCASE("LightCache dispose advances instead of rescanning") {
    terraform::lighting::LightCache cache(0, 0);
    auto a = make_shared<terraform::lighting::ChunkLightingModel>(-1, 0, -1);
    auto b = make_shared<terraform::lighting::ChunkLightingModel>(0, 0, -1);
    auto c = make_shared<terraform::lighting::ChunkLightingModel>(1, 0, -1);
    auto d = make_shared<terraform::lighting::ChunkLightingModel>(-1, 0, 0);
    auto e = make_shared<terraform::lighting::ChunkLightingModel>(0, 0, 0);
    cache.setModel(-1, -1, a);
    cache.setModel(0, -1, b);
    cache.setModel(1, -1, c);
    cache.setModel(-1, 0, d);
    cache.setModel(0, 0, e);

    cache.dispose(0, -1);
    CHECK_FALSE(cache.getModel(-1, -1));
    CHECK_FALSE(cache.getModel(0, -1));
    CHECK(cache.getModel(1, -1) == c);
    CHECK(cache.getModel(-1, 0) == d);

    cache.dispose(-1, 0);
    CHECK_FALSE(cache.getModel(1, -1));
    CHECK_FALSE(cache.getModel(-1, 0));
    CHECK(cache.getModel(0, 0) == e);

    cache.dispose(32, -1);
    CHECK(cache.getModel(0, 0) == e);
    cache.dispose(0, 0);
    CHECK_FALSE(cache.getModel(0, 0));
  }

  SUBCASE("LightCache relocation retains only adjacent region boundaries") {
    terraform::lighting::LightCache cache(0, 0);
    auto west = make_shared<terraform::lighting::ChunkLightingModel>(30, 0, 0);
    auto boundary = make_shared<terraform::lighting::ChunkLightingModel>(31, 0, 0);
    auto first = make_shared<terraform::lighting::ChunkLightingModel>(32, 0, 0);
    weak_ptr<terraform::lighting::ChunkLightingModel> westReleased = west;
    weak_ptr<terraform::lighting::ChunkLightingModel> boundaryReleased = boundary;
    cache.setModel(30, 0, west);
    cache.setModel(31, 0, boundary);
    cache.setModel(32, 0, first);
    west.reset();

    cache.relocate(1, 0);
    CHECK_FALSE(cache.getModel(30, 0));
    CHECK(westReleased.expired());
    CHECK(cache.getModel(31, 0) == boundary);
    CHECK(cache.getModel(32, 0) == first);

    boundary.reset();
    cache.relocate(100, 100);
    CHECK_FALSE(cache.getModel(31, 0));
    CHECK(boundaryReleased.expired());
  }

  SUBCASE("Data3dSq relocation preserves storage shape and resets values") {
    Data3dSq<int, 4> data({0, 0, 0}, 2, 1);
    data[{3, 1, 3}] = 9;

    data.relocate({10, -2, 20}, 7);
    CHECK(data.fStart == Pos3i(10, -2, 20));
    CHECK(data.fEnd == Pos3i(13, -1, 23));
    CHECK(data.height() == 2);
    CHECK(data[{10, -2, 20}] == 7);
    CHECK(data[{13, -1, 23}] == 7);
  }

  SUBCASE("bucket lighting diffusion visits only successful updates") {
    using namespace terraform::lighting;
    Data3dSq<LightingModel, 44> models({0, 0, 0}, 1, LightingModel(CLEAR));
    Data3dSq<u8, 44> light({0, 0, 0}, 1, 0);
    Data2d<optional<Volume>> volumes({0, 0}, 1, 1, nullopt);
    volumes[{0, 0}] = Volume({8, 0, 8}, {22, 0, 22});
    light[{15, 0, 15}] = 15;

    size_t updates = Lighting::DiffuseLight(models, light, volumes);
    CHECK(light[{15, 0, 15}] == 15);
    CHECK(light[{16, 0, 15}] == 14);
    CHECK(light[{22, 0, 15}] == 8);
    CHECK(light[{23, 0, 15}] == 7);
    CHECK(light[{24, 0, 15}] == 0);
    CHECK(updates <= 44 * 44);
  }

  SUBCASE("lighting diffusion skips work without diffuse volumes") {
    using namespace terraform::lighting;
    Data3dSq<LightingModel, 44> models({0, 0, 0}, 1, LightingModel(CLEAR));
    Data3dSq<u8, 44> light({0, 0, 0}, 1, 0);
    Data2d<optional<Volume>> volumes({0, 0}, 1, 1, nullopt);
    light[{15, 0, 15}] = 15;

    CHECK(Lighting::DiffuseLight(models, light, volumes) == 0);
    CHECK(light[{15, 0, 15}] == 15);
    CHECK(light[{16, 0, 15}] == 0);
  }

  SUBCASE("block light initialization visits indexed emitters only") {
    using namespace terraform::lighting;
    LightCache cache(0, 0);
    auto chunkModel = make_shared<ChunkLightingModel>(0, 0, 0);
    chunkModel->addEmitter({8, 0, 8}, 15);
    cache.setModel(0, 0, chunkModel);

    Data3dSq<LightingModel, 44> models({0, 0, 0}, 1, LightingModel(CLEAR));
    Data3dSq<u8, 44> light({0, 0, 0}, 1, 0);
    Data2d<bool> cached({0, 0}, 1, 1, false);
    Data2d<optional<Volume>> volumes({0, 0}, 1, 1, nullopt);
    Data2d<optional<Volume>> diffuse({0, 0}, 1, 1, nullopt);
    volumes[{0, 0}] = Volume({0, 0, 0}, {15, 0, 15});

    size_t visits = Lighting::InitializeBlockLight(cache, models, light, cached, volumes, diffuse);
    CHECK(visits == 1);
    CHECK(light[{8, 0, 8}] == 15);
    CHECK(light[{9, 0, 8}] == 14);
    CHECK(diffuse[{0, 0}]);
  }

  SUBCASE("uniform section lighting packs once and preserves unchanged vectors") {
    using namespace terraform::lighting;
    Data3dSq<u8, 44> light({0, 0, 0}, 16, 0);
    array<u8, 2048> packed;
    vector<u8> destination(2048, 0x22);

    CHECK(Lighting::WriteSectionLight(light, {0, 0, 0}, destination, packed));
    CHECK(destination.empty());

    light.fill(15);
    CHECK(Lighting::WriteSectionLight(light, {0, 0, 0}, destination, packed));
    REQUIRE(destination.size() == 2048);
    CHECK(destination.front() == 0xff);
    u8 const *storage = destination.data();
    CHECK_FALSE(Lighting::WriteSectionLight(light, {0, 0, 0}, destination, packed));
    CHECK(destination.data() == storage);

    light.fill(0);
    light[{0, 0, 0}] = 1;
    light[{1, 0, 0}] = 2;
    CHECK(Lighting::WriteSectionLight(light, {0, 0, 0}, destination, packed));
    CHECK(destination[0] == 0x21);
    CHECK(destination[1] == 0);

    light.fill(0);
    CHECK(Lighting::WriteSectionLight(light, {0, 0, 0}, destination, packed, false));
    REQUIRE(destination.size() == 2048);
    CHECK(destination.front() == 0);
    CHECK(destination.back() == 0);
  }

  SUBCASE("empty chunk light cache retains the processed marker") {
    using namespace terraform::lighting;
    auto empty = ChunkLightCache::CreateEmpty(0, 0);
    REQUIRE(empty);
    CHECK(empty->empty());
    Data3dSq<u8, 44> light({-14, 0, -14}, 1, 0);
    empty->copyTo(light);
    CHECK(light[{0, 0, 0}] == 0);
  }

  SUBCASE("Java cache uses the in-memory editor and preserves missing chunks") {
    auto temp = mcfile::File::CreateTempDir(fs::temp_directory_path());
    REQUIRE(temp);
    defer {
      Fs::DeleteAll(*temp);
    };

    auto file = *temp / mcfile::je::Region::GetDefaultRegionFileName(0, 0);
    auto editor = mcfile::je::McaEditor::Open(file);
    REQUIRE(editor);
    auto chunk = mcfile::je::WritableChunk::MakeEmpty(1, 0, 1);
    REQUIRE(chunk);
    auto tag = chunk->toCompoundTag(mcfile::Dimension::Overworld);
    REQUIRE(tag);
    REQUIRE(editor->insert(1, 1, *tag));
    REQUIRE(editor->write(file));
    editor.reset();

    editor = mcfile::je::McaEditor::Open(file);
    REQUIRE(editor);
    REQUIRE(editor->extract(1, 1));

    terraform::java::BlockAccessorJavaDirectory<3, 3> cache(0, 0, *temp);
    cache.loadAllWith(*editor, 0, 0);
    CHECK_FALSE(cache.chunkAt(1, 1));

    unique_ptr<terraform::java::BlockAccessorJavaDirectory<3, 3>> relocated(cache.makeRelocated(0, 0));
    REQUIRE(relocated);
    CHECK_FALSE(relocated->chunkAt(1, 1));
  }
}
