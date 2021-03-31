#include <cache/dump/dump_manager.hpp>

#include <algorithm>

#include <fmt/compile.h>
#include <fmt/format.h>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>

#include <engine/async.hpp>
#include <fs/blocking/write.hpp>
#include <utils/assert.hpp>
#include <utils/datetime.hpp>
#include <utils/from_string.hpp>

namespace cache::dump {

namespace {

const std::string kTimeZone = "UTC";

}  // namespace

DumpManager::DumpManager(Config&& config)
    : name_(config.name),
      config_(std::move(config)),
      filename_regex_(GenerateFilenameRegex(FileFormatType::kNormal)),
      tmp_filename_regex_(GenerateFilenameRegex(FileFormatType::kTmp)) {}

DumpFileStats DumpManager::RegisterNewDump(TimePoint update_time) {
  const auto config = config_.Read();
  std::string dump_path = GenerateDumpPath(update_time, *config);

  if (boost::filesystem::exists(dump_path)) {
    throw std::runtime_error(fmt::format(
        "Could not dump cache {} to \"{}\", because the file already exists",
        name_, dump_path));
  }

  try {
    fs::blocking::CreateDirectories(config->dump_directory);
  } catch (const std::exception& ex) {
    throw std::runtime_error(fmt::format(
        "Error while creating cache dump for cache {} at \"{}\". Cause: {}",
        name_, dump_path, ex.what()));
  }

  return {update_time, std::move(dump_path), config->dump_format_version};
}

std::optional<DumpFileStats> DumpManager::GetLatestDump() const {
  const auto config = config_.Read();

  try {
    std::optional<DumpFileStats> stats = GetLatestDump(*config);
    if (!stats) {
      LOG_INFO() << "No usable cache dumps found for cache " << name_;
      return std::nullopt;
    }

    LOG_DEBUG() << "A usable cache dump found for cache " << name_ << ": \""
                << stats->full_path << "\"";

    return *std::move(stats);
  } catch (const std::exception& ex) {
    LOG_ERROR()
        << "Error while trying to read the contents of cache dump for cache "
        << name_ << ". Cause: " << ex;
    return std::nullopt;
  }
}

bool DumpManager::BumpDumpTime(TimePoint old_update_time,
                               TimePoint new_update_time) {
  UASSERT(old_update_time <= new_update_time);
  const auto config = config_.Read();

  const std::string old_name = GenerateDumpPath({old_update_time}, *config);
  const std::string new_name = GenerateDumpPath({new_update_time}, *config);

  try {
    if (!boost::filesystem::is_regular_file(old_name)) {
      LOG_WARNING()
          << "The previous cache dump \"" << old_name << "\" of cache " << name_
          << " has suddenly disappeared. A new cache dump will be created.";
      return false;
    }
    boost::filesystem::rename(old_name, new_name);
    LOG_INFO() << "Renamed cache dump \"" << old_name << "\" of cache " << name_
               << " to \"" << new_name << "\"";
    return true;
  } catch (const boost::filesystem::filesystem_error& ex) {
    LOG_ERROR() << "Error while trying to rename cache dump \"" << old_name
                << "\" of cache " << name_ << " to \"" << new_name
                << "\". Reason: " << ex;
    return false;
  }
}

void DumpManager::Cleanup() {
  config_.Cleanup();
  const auto config = config_.Read();
  DoCleanup(*config);
}

void DumpManager::SetConfig(Config&& config) {
  config_.Assign(std::move(config));
}

std::optional<DumpFileStats> DumpManager::ParseDumpName(
    std::string full_path) const {
  const auto filename = boost::filesystem::path{full_path}.filename().string();

  boost::smatch regex;
  if (boost::regex_match(filename, regex, filename_regex_)) {
    UASSERT_MSG(regex.size() == 3,
                fmt::format("Incorrect sub-match count: {} for filename {}",
                            regex.size(), filename));

    try {
      const auto date = utils::datetime::Stringtime(regex[1].str(), kTimeZone,
                                                    kFilenameDateFormat);
      const auto version = utils::FromString<uint64_t>(regex[2].str());
      return DumpFileStats{{Round(date)}, std::move(full_path), version};
    } catch (const std::exception& ex) {
      LOG_WARNING() << "A filename looks like a cache dump of cache " << name_
                    << ", but it is not: \"" << filename
                    << "\". Reason: " << ex;
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<DumpFileStats> DumpManager::GetLatestDump(
    const Config& config) const {
  const auto min_update_time = MinAcceptableUpdateTime(config);
  std::optional<DumpFileStats> best_dump;

  try {
    if (!boost::filesystem::exists(config.dump_directory)) {
      LOG_DEBUG() << "Cache dump directory \"" << config.dump_directory
                  << "\" does not exist";
      return {};
    }

    for (const auto& file :
         boost::filesystem::directory_iterator{config.dump_directory}) {
      if (!boost::filesystem::is_regular_file(file.status())) {
        continue;
      }

      auto curr_dump = ParseDumpName(file.path().string());
      if (!curr_dump) {
        if (boost::regex_match(file.path().filename().string(),
                               tmp_filename_regex_)) {
          LOG_DEBUG() << "A leftover tmp file found: \"" << file.path().string()
                      << "\". It will be removed on next Cleanup";
        } else {
          LOG_WARNING()
              << "Unrelated file in the cache dump directory for cache "
              << name_ << ": \"" << file.path().string() << "\"";
        }
        continue;
      }

      if (curr_dump->format_version != config.dump_format_version) {
        LOG_DEBUG() << "Ignoring cache dump \"" << curr_dump->full_path
                    << "\", because its format version ("
                    << curr_dump->format_version << ") != current version ("
                    << config.dump_format_version << ")";
        continue;
      }

      if (curr_dump->update_time < min_update_time && config.max_dump_age) {
        LOG_DEBUG() << "Ignoring cache dump \"" << curr_dump->full_path
                    << "\", because its age is greater than the maximum "
                       "allowed cache dump age ("
                    << config.max_dump_age->count() << "ms)";
        continue;
      }

      if (!best_dump || curr_dump->update_time > best_dump->update_time) {
        best_dump = std::move(curr_dump);
      }
    }
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Error while trying to fetch cache dumps for cache " << name_
                << ". Cause: " << ex;
    // proceed to return best_dump
  }

  return best_dump ? std::optional{std::move(best_dump)} : std::nullopt;
}

void DumpManager::DoCleanup(const Config& config) {
  const auto min_update_time = MinAcceptableUpdateTime(config);
  std::vector<DumpFileStats> dumps;

  try {
    if (!boost::filesystem::exists(config.dump_directory)) {
      LOG_INFO() << "Cache dump directory \"" << config.dump_directory
                 << "\" does not exist";
      return;
    }

    for (const auto& file :
         boost::filesystem::directory_iterator{config.dump_directory}) {
      if (!boost::filesystem::is_regular_file(file.status())) {
        continue;
      }

      std::string filename = file.path().filename().string();

      if (boost::regex_match(filename, tmp_filename_regex_)) {
        LOG_DEBUG() << "Removing a leftover tmp file \"" << file.path().string()
                    << "\"";
        boost::filesystem::remove(file);
        continue;
      }

      auto dump = ParseDumpName(file.path().string());
      if (!dump) {
        LOG_WARNING() << "Unrelated file in the cache dump directory for cache "
                      << name_ << ": \"" << file.path().string() << "\"";
        continue;
      }

      if (dump->format_version < config.dump_format_version ||
          dump->update_time < min_update_time) {
        LOG_DEBUG() << "Removing an expired dump \"" << file.path().string()
                    << "\" for cache " << name_;
        boost::filesystem::remove(file);
        continue;
      }

      if (dump->format_version == config.dump_format_version) {
        dumps.push_back(std::move(*dump));
      }
    }

    std::sort(dumps.begin(), dumps.end(),
              [](const DumpFileStats& a, const DumpFileStats& b) {
                return a.update_time > b.update_time;
              });

    for (size_t i = config.max_dump_count; i < dumps.size(); ++i) {
      LOG_DEBUG() << "Removing an excessive dump \"" << dumps[i].full_path
                  << "\" for cache " << name_;
      boost::filesystem::remove(dumps[i].full_path);
    }
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Error while cleaning up old dumps for cache " << name_
                << ". Cause: " << ex;
  }
}

std::string DumpManager::GenerateDumpPath(TimePoint update_time,
                                          const Config& config) {
  return fmt::format(
      FMT_COMPILE("{}/{}-v{}"), config.dump_directory,
      utils::datetime::Timestring(update_time, kTimeZone, kFilenameDateFormat),
      config.dump_format_version);
}

std::string DumpManager::GenerateFilenameRegex(FileFormatType type) {
  return std::string{
             R"(^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6})-v(\d+))"} +
         (type == FileFormatType::kTmp ? "\\.tmp$" : "$");
}

TimePoint DumpManager::MinAcceptableUpdateTime(const Config& config) {
  return config.max_dump_age
             ? Round(utils::datetime::Now()) - *config.max_dump_age
             : TimePoint::min();
}

TimePoint DumpManager::Round(std::chrono::system_clock::time_point time) {
  return std::chrono::round<TimePoint::duration>(time);
}

}  // namespace cache::dump
