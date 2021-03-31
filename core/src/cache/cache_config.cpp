#include <cache/cache_config.hpp>

#include <logging/log.hpp>
#include <utils/algo.hpp>
#include <utils/string_to_duration.hpp>
#include <utils/traceful_exception.hpp>

namespace cache {

namespace {

constexpr std::string_view kUpdateIntervalMs = "update-interval-ms";
constexpr std::string_view kUpdateJitterMs = "update-jitter-ms";
constexpr std::string_view kFullUpdateIntervalMs = "full-update-interval-ms";
constexpr std::string_view kCleanupIntervalMs =
    "additional-cleanup-interval-ms";

constexpr std::string_view kUpdateInterval = "update-interval";
constexpr std::string_view kUpdateJitter = "update-jitter";
constexpr std::string_view kFullUpdateInterval = "full-update-interval";
constexpr std::string_view kCleanupInterval = "additional-cleanup-interval";

constexpr std::string_view kFirstUpdateFailOk = "first-update-fail-ok";
constexpr std::string_view kUpdateTypes = "update-types";
constexpr std::string_view kForcePeriodicUpdates =
    "testsuite-force-periodic-update";
constexpr std::string_view kConfigSettings = "config-settings";

constexpr std::string_view kWays = "ways";
constexpr std::string_view kSize = "size";
constexpr std::string_view kLifetime = "lifetime";
constexpr std::string_view kBackgroundUpdate = "background-update";
constexpr std::string_view kLifetimeMs = "lifetime-ms";

constexpr std::string_view kFirstUpdateMode = "first-update-mode";
constexpr std::string_view kForceFullSecondUpdate = "force-full-second-update";

constexpr auto kDefaultCleanupInterval = std::chrono::seconds{10};

std::chrono::milliseconds GetDefaultJitter(std::chrono::milliseconds interval) {
  return interval / 10;
}

AllowedUpdateTypes ParseUpdateMode(const yaml_config::YamlConfig& config) {
  const auto update_types_str =
      config[kUpdateTypes].As<std::optional<std::string>>();
  if (!update_types_str) {
    if (config.HasMember(kFullUpdateInterval) &&
        config.HasMember(kUpdateInterval)) {
      return AllowedUpdateTypes::kFullAndIncremental;
    } else {
      return AllowedUpdateTypes::kOnlyFull;
    }
  } else if (*update_types_str == "full-and-incremental") {
    return AllowedUpdateTypes::kFullAndIncremental;
  } else if (*update_types_str == "only-full") {
    return AllowedUpdateTypes::kOnlyFull;
  } else if (*update_types_str == "only-incremental") {
    return AllowedUpdateTypes::kOnlyIncremental;
  }

  throw std::logic_error(fmt::format("Invalid update types '{}' at '{}'",
                                     *update_types_str, config.GetPath()));
}

}  // namespace

using ::cache::dump::impl::ParseMs;

FirstUpdateMode Parse(const yaml_config::YamlConfig& config,
                      formats::parse::To<FirstUpdateMode>) {
  const auto as_string = config.As<std::string>();

  if (as_string == "required") return FirstUpdateMode::kRequired;
  if (as_string == "best-effort") return FirstUpdateMode::kBestEffort;
  if (as_string == "skip") return FirstUpdateMode::kSkip;

  throw yaml_config::ParseException(fmt::format(
      "Invalid first update mode '{}' at '{}'", as_string, config.GetPath()));
}

CacheConfig::CacheConfig(const components::ComponentConfig& config)
    : update_interval(config[kUpdateInterval].As<std::chrono::milliseconds>(0)),
      update_jitter(config[kUpdateJitter].As<std::chrono::milliseconds>(
          GetDefaultJitter(update_interval))),
      full_update_interval(
          config[kFullUpdateInterval].As<std::chrono::milliseconds>(0)),
      cleanup_interval(config[kCleanupInterval].As<std::chrono::milliseconds>(
          kDefaultCleanupInterval)) {}

CacheConfig::CacheConfig(const formats::json::Value& value)
    : update_interval(ParseMs(value[kUpdateIntervalMs])),
      update_jitter(ParseMs(value[kUpdateJitterMs])),
      full_update_interval(ParseMs(value[kFullUpdateIntervalMs])),
      cleanup_interval(
          ParseMs(value[kCleanupIntervalMs], kDefaultCleanupInterval)) {
  if (!update_interval.count() && !full_update_interval.count()) {
    throw utils::impl::AttachTraceToException(
        std::logic_error("Update interval is not set for cache"));
  } else if (!full_update_interval.count()) {
    full_update_interval = update_interval;
  } else if (!update_interval.count()) {
    update_interval = full_update_interval;
  }

  if (update_jitter > update_interval) {
    update_jitter = GetDefaultJitter(update_interval);
  }
}

CacheConfigStatic::CacheConfigStatic(
    const components::ComponentConfig& config,
    const std::optional<dump::Config>& dump_config)
    : CacheConfig(config),
      allowed_update_types(ParseUpdateMode(config)),
      allow_first_update_failure(config[kFirstUpdateFailOk].As<bool>(false)),
      force_periodic_update(
          config[kForcePeriodicUpdates].As<std::optional<bool>>()),
      config_updates_enabled(config[kConfigSettings].As<bool>(true)),
      first_update_mode(
          config[dump::kDump][kFirstUpdateMode].As<FirstUpdateMode>(
              FirstUpdateMode::kSkip)),
      force_full_second_update(
          config[dump::kDump][kForceFullSecondUpdate].As<bool>(false)) {
  switch (allowed_update_types) {
    case AllowedUpdateTypes::kFullAndIncremental:
      if (!update_interval.count() || !full_update_interval.count()) {
        throw std::logic_error(
            fmt::format("Both {} and {} must be set for cache '{}'",
                        kUpdateInterval, kFullUpdateInterval, config.Name()));
      }
      if (update_interval >= full_update_interval) {
        LOG_WARNING() << "Incremental updates requested for cache '"
                      << config.Name()
                      << "' but have lower frequency than full updates and "
                         "will never happen. Remove "
                      << kFullUpdateInterval
                      << " config field if this is intended.";
      }
      break;
    case AllowedUpdateTypes::kOnlyFull:
    case AllowedUpdateTypes::kOnlyIncremental:
      if (full_update_interval.count()) {
        throw std::logic_error(fmt::format(
            "{} config field must only be used with full-and-incremental "
            "updated cache '{}'. Please rename it to {}.",
            kFullUpdateInterval, config.Name(), kUpdateInterval));
      }
      if (!update_interval.count()) {
        throw std::logic_error(fmt::format("{} is not set for cache '{}'",
                                           kUpdateInterval, config.Name()));
      }
      full_update_interval = update_interval;
      break;
  }

  if (config.HasMember(dump::kDump)) {
    if (!config[dump::kDump].HasMember(kFirstUpdateMode)) {
      throw std::logic_error(fmt::format(
          "If dumps are enabled, then '{}' must be set for cache '{}'",
          kFirstUpdateMode, config.Name()));
    }

    if (first_update_mode != FirstUpdateMode::kRequired &&
        !dump_config->max_dump_age_set) {
      throw std::logic_error(fmt::format(
          "If '{}' is not 'required', then '{}' must be set for cache '{}'. If "
          "using severely outdated data is not harmful for this cache, please "
          "add to config.yaml: '{}:  # outdated data is not harmful'",
          kFirstUpdateMode, dump::kMaxDumpAge, config.Name(), dump::kMaxDumpAge,
          dump::kMaxDumpAge));
    }

    if (allowed_update_types == AllowedUpdateTypes::kOnlyIncremental &&
        !config[dump::kDump].HasMember(kForceFullSecondUpdate)) {
      throw std::logic_error(fmt::format(
          "If '{}' is not 'skip', then '{}' must be set for cache '{}'",
          kFirstUpdateMode, kForceFullSecondUpdate, config.Name()));
    }
  }
}

CacheConfigStatic CacheConfigStatic::MergeWith(const CacheConfig& other) const {
  CacheConfigStatic copy = *this;
  static_cast<CacheConfig&>(copy) = other;
  return copy;
}

LruCacheConfig::LruCacheConfig(const components::ComponentConfig& config)
    : size(config[kSize].As<size_t>()),
      lifetime(config[kLifetime].As<std::chrono::milliseconds>(0)),
      background_update(config[kBackgroundUpdate].As<bool>(false)
                            ? BackgroundUpdateMode::kEnabled
                            : BackgroundUpdateMode::kDisabled) {
  if (size == 0) throw std::runtime_error("cache-size is non-positive");
}

LruCacheConfig::LruCacheConfig(const formats::json::Value& value)
    : size(value[kSize].As<size_t>()),
      lifetime(ParseMs(value[kLifetimeMs])),
      background_update(value[kBackgroundUpdate].As<bool>(false)
                            ? BackgroundUpdateMode::kEnabled
                            : BackgroundUpdateMode::kDisabled) {
  if (size == 0) throw std::runtime_error("cache-size is non-positive");
}

LruCacheConfigStatic::LruCacheConfigStatic(
    const components::ComponentConfig& component_config)
    : config(component_config), ways(component_config[kWays].As<size_t>()) {
  if (ways <= 0) throw std::runtime_error("cache-ways is non-positive");
}

size_t LruCacheConfigStatic::GetWaySize() const {
  auto way_size = config.size / ways;
  return way_size == 0 ? 1 : way_size;
}

LruCacheConfigStatic LruCacheConfigStatic::MergeWith(
    const LruCacheConfig& other) const {
  LruCacheConfigStatic copy = *this;
  copy.config = other;
  return copy;
}

CacheConfigSet::CacheConfigSet(const taxi_config::DocsMap& docs_map) {
  const auto& config_name = ConfigName();
  if (!config_name.empty()) {
    auto caches_json = docs_map.Get(config_name);
    for (const auto& [name, value] : Items(caches_json)) {
      configs_.try_emplace(name, value);
    }
  }

  const auto& lru_config_name = LruConfigName();
  if (!lru_config_name.empty()) {
    auto lru_caches_json = docs_map.Get(lru_config_name);
    for (const auto& [name, value] : Items(lru_caches_json)) {
      lru_configs_.try_emplace(name, value);
    }
  }
}

std::optional<CacheConfig> CacheConfigSet::GetConfig(
    const std::string& cache_name) const {
  return utils::FindOptional(configs_, cache_name);
}

std::optional<LruCacheConfig> CacheConfigSet::GetLruConfig(
    const std::string& cache_name) const {
  return utils::FindOptional(lru_configs_, cache_name);
}

bool CacheConfigSet::IsConfigEnabled() { return !ConfigName().empty(); }

bool CacheConfigSet::IsLruConfigEnabled() { return !LruConfigName().empty(); }

void CacheConfigSet::SetConfigName(const std::string& name) {
  ConfigName() = name;
}

std::string& CacheConfigSet::ConfigName() {
  static std::string name;
  return name;
}

void CacheConfigSet::SetLruConfigName(const std::string& name) {
  LruConfigName() = name;
}

std::string& CacheConfigSet::LruConfigName() {
  static std::string name;
  return name;
}

}  // namespace cache
