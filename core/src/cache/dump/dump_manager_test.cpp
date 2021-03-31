#include <cache/dump/dump_manager.hpp>

#include <set>

#include <cache/test_helpers.hpp>
#include <fs/blocking/read.hpp>
#include <fs/blocking/temp_directory.hpp>
#include <fs/blocking/write.hpp>
#include <utest/utest.hpp>
#include <utils/mock_now.hpp>

namespace {

cache::dump::TimePoint BaseTime() {
  return std::chrono::time_point_cast<cache::dump::TimePoint::duration>(
      utils::datetime::Stringtime("2015-03-22T09:00:00.000000", "UTC",
                                  cache::dump::kFilenameDateFormat));
}

std::vector<std::string> InitialFileNames() {
  return {
      "2015-03-22T09:00:00.000000-v5",  "2015-03-22T09:00:00.000000-v0",
      "2015-03-22T09:00:00.000000-v42", "2015-03-22T09:00:01.000000-v5",
      "2015-03-22T09:00:02.000000-v5",  "2015-03-22T09:00:03.000000-v5",
  };
}

std::vector<std::string> JunkFileNames() {
  return {
      "2015-03-22T09:00:00.000000-v0.tmp",
      "2015-03-22T09:00:00.000000-v5.tmp",
      "2000-01-01T00:00:00.000000-v42.tmp",
  };
}

std::vector<std::string> UnrelatedFileNames() {
  return {"blah-2015-03-22T09:00:00.000000-v5",
          "blah-2015-03-22T09:00:00.000000-v5.tmp",
          "foo",
          "foo.tmp",
          "2015-03-22T09:00:00.000000-v-5",
          "2015-03-22T09:00:00.000000-v-5.tmp",
          "2015-03-22T09:00:00.000000-5.tmp"};
}

template <typename Container, typename Range>
void InsertAll(Container& destination, Range&& source) {
  destination.insert(std::begin(source), std::end(source));
}

constexpr std::string_view kCacheName = "name";

}  // namespace

TEST(DumpManager, CleanupTmpTest) {
  const std::string kConfig = R"(
update-interval: 1s
dump:
    enable: true
    world-readable: false
    format-version: 5
    first-update-mode: skip
    max-count: 10
    max-age: null
)";
  const auto dir = fs::blocking::TempDirectory::Create();

  cache::CreateDumps(InitialFileNames(), dir, kCacheName);
  cache::CreateDumps(JunkFileNames(), dir, kCacheName);
  cache::CreateDumps(UnrelatedFileNames(), dir, kCacheName);

  // Expected to remove .tmp junk and a dump with version 0
  std::set<std::string> expected_files;
  InsertAll(expected_files, InitialFileNames());
  InsertAll(expected_files, UnrelatedFileNames());
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:00.000000-v0"));

  utils::datetime::MockNowSet(BaseTime());

  RunInCoro([&] {
    cache::dump::DumpManager dumper(
        cache::dump::Config{cache::ConfigFromYaml(kConfig, dir, kCacheName)});

    dumper.Cleanup();
  });

  EXPECT_EQ(cache::FilenamesInDirectory(dir, kCacheName), expected_files);
}

TEST(DumpManager, CleanupByAgeTest) {
  const std::string kConfig = R"(
update-interval: 1s
dump:
    enable: true
    world-readable: false
    format-version: 5
    first-update-mode: skip
    max-count: 10
    max-age: 1500ms
)";
  const auto dir = fs::blocking::TempDirectory::Create();

  cache::CreateDumps(InitialFileNames(), dir, kCacheName);
  cache::CreateDumps(JunkFileNames(), dir, kCacheName);
  cache::CreateDumps(UnrelatedFileNames(), dir, kCacheName);

  // Expected to remove .tmp junk, a dump with version 0, and old dumps
  std::set<std::string> expected_files;
  InsertAll(expected_files, InitialFileNames());
  InsertAll(expected_files, UnrelatedFileNames());
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:00.000000-v0"));
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:00.000000-v5"));
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:01.000000-v5"));
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:00.000000-v42"));

  utils::datetime::MockNowSet(BaseTime());
  utils::datetime::MockSleep(std::chrono::seconds{3});

  RunInCoro([&] {
    cache::dump::DumpManager dumper(
        cache::dump::Config{cache::ConfigFromYaml(kConfig, dir, kCacheName)});

    dumper.Cleanup();
  });

  EXPECT_EQ(cache::FilenamesInDirectory(dir, kCacheName), expected_files);
}

TEST(DumpManager, CleanupByCountTest) {
  const std::string kConfig = R"(
update-interval: 1s
dump:
    enable: true
    world-readable: false
    format-version: 5
    first-update-mode: skip
    max-count: 1
    max-age: null
)";
  const auto dir = fs::blocking::TempDirectory::Create();

  cache::CreateDumps(InitialFileNames(), dir, kCacheName);
  cache::CreateDumps(JunkFileNames(), dir, kCacheName);
  cache::CreateDumps(UnrelatedFileNames(), dir, kCacheName);

  // Expected to remove .tmp junk, a dump with version 0, and some current dumps
  std::set<std::string> expected_files;
  InsertAll(expected_files, InitialFileNames());
  InsertAll(expected_files, UnrelatedFileNames());
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:00.000000-v0"));
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:00.000000-v5"));
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:01.000000-v5"));
  ASSERT_TRUE(expected_files.erase("2015-03-22T09:00:02.000000-v5"));

  RunInCoro([&] {
    cache::dump::DumpManager dumper(
        cache::dump::Config{cache::ConfigFromYaml(kConfig, dir, kCacheName)});

    dumper.Cleanup();
  });

  EXPECT_EQ(cache::FilenamesInDirectory(dir, kCacheName), expected_files);
}

TEST(DumpManager, ReadLatestDumpTest) {
  const std::string kConfig = R"(
update-interval: 1s
dump:
    enable: true
    world-readable: false
    format-version: 5
    first-update-mode: skip
    max-age: null
)";
  const auto dir = fs::blocking::TempDirectory::Create();

  cache::CreateDumps(InitialFileNames(), dir, kCacheName);
  cache::CreateDumps(JunkFileNames(), dir, kCacheName);
  cache::CreateDumps(UnrelatedFileNames(), dir, kCacheName);

  // Expected not to write or remove anything
  std::set<std::string> expected_files;
  InsertAll(expected_files, InitialFileNames());
  InsertAll(expected_files, JunkFileNames());
  InsertAll(expected_files, UnrelatedFileNames());

  RunInCoro([&] {
    cache::dump::DumpManager dumper(
        cache::dump::Config{cache::ConfigFromYaml(kConfig, dir, kCacheName)});

    auto dump_info = dumper.GetLatestDump();
    EXPECT_TRUE(dump_info);

    if (dump_info) {
      using namespace std::chrono_literals;
      EXPECT_EQ(dump_info->update_time, BaseTime() + 3s);
      EXPECT_EQ(fs::blocking::ReadFileContents(dump_info->full_path),
                "2015-03-22T09:00:03.000000-v5");
    }
  });

  EXPECT_EQ(cache::FilenamesInDirectory(dir, kCacheName), expected_files);
}

TEST(DumpManager, DumpAndBumpTest) {
  const std::string kConfig = R"(
update-interval: 1s
dump:
    enable: true
    world-readable: false
    format-version: 5
    first-update-mode: skip
    max-age: null
)";
  const auto dir = fs::blocking::TempDirectory::Create();

  // Expected to get a new dump with update_time = BaseTime() + 3s
  std::set<std::string> expected_files;
  expected_files.insert("2015-03-22T09:00:03.000000-v5");

  RunInCoro([&] {
    using namespace std::chrono_literals;

    cache::dump::DumpManager dumper(
        cache::dump::Config{cache::ConfigFromYaml(kConfig, dir, kCacheName)});

    auto old_update_time = BaseTime();
    auto dump_stats = dumper.RegisterNewDump(old_update_time);

    fs::blocking::RewriteFileContents(dump_stats.full_path, "abc");

    // Emulate a new update that happened 3s later and got identical data
    auto new_update_time = BaseTime() + 3s;
    EXPECT_TRUE(dumper.BumpDumpTime(old_update_time, new_update_time));

    auto dump_info = dumper.GetLatestDump();
    ASSERT_TRUE(dump_info);

    EXPECT_EQ(fs::blocking::ReadFileContents(dump_info->full_path), "abc");
    EXPECT_EQ(dump_info->update_time, new_update_time);
  });

  EXPECT_EQ(cache::FilenamesInDirectory(dir, kCacheName), expected_files);
}
