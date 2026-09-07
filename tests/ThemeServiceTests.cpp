#include "services/ThemeService.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

realmheart::services::Palette valid_palette() {
    realmheart::services::Palette palette;
    palette.colors = {
        {"primary", "#123456"},
        {"accent", "#123456"},
        {"secondary", "#abcdef"},
        {"background", "#010203"},
        {"surface", "#111213"},
        {"text", "#fafafa"},
        {"error", "#ff3344"}
    };
    return palette;
}

void test_subscriptions_and_validation(const std::filesystem::path& cache_path) {
    realmheart::services::ThemeService service(cache_path);
    int calls = 0;
    std::string last_primary;

    auto subscription = service.subscribe([&](const auto& palette) {
        ++calls;
        last_primary = palette.get("primary");
    });

    auto first = valid_palette();
    require(service.update_palette(first), "valid palette update was rejected");
    require(calls == 1, "subscriber did not receive palette");
    require(last_primary == "#123456", "subscriber received wrong palette");

    auto moved = std::move(subscription);
    auto second = valid_palette();
    second.colors["primary"] = "#abcdef";
    require(service.update_palette(second), "second valid palette update was rejected");
    require(calls == 2, "moved subscription stopped receiving updates");

    moved.reset();
    auto third = valid_palette();
    third.colors["primary"] = "#ffffff";
    require(service.update_palette(third), "third valid palette update was rejected");
    require(calls == 2, "reset subscription still received updates");
    service.wait_for_persistence();
}

void test_invalid_update_is_not_published(const std::filesystem::path& cache_path) {
    realmheart::services::ThemeService service(cache_path);
    int calls = 0;
    auto subscription = service.subscribe([&](const auto&) { ++calls; });
    auto invalid = valid_palette();
    invalid.colors["error"] = "#123456; color: red";
    require(!service.update_palette(invalid), "CSS-injection palette must be rejected");
    require(calls == 0, "rejected palette must not notify subscribers");
    require(service.get_palette().get("primary") == "#cba6f7",
            "rejected palette must not replace the current palette");
}

void test_bounded_cache_rejects_excess_physical_lines(const std::filesystem::path& cache_path) {
    std::filesystem::create_directories(cache_path.parent_path());
    std::ofstream output(cache_path, std::ios::binary);
    output << "realmheart-theme-cache-v1\n";
    for (int index = 0; index < 65; ++index) output << '\n';
    output.close();
    realmheart::services::ThemeService service(cache_path);
    require(service.get_palette().get("primary") == "#cba6f7",
            "cache with excessive blank physical lines must be ignored");
}

void test_persistence_commit_and_failure_are_observable(
    const std::filesystem::path& root
) {
    const auto committed_path = root / "committed" / "palette.tsv";
    {
        realmheart::services::ThemeService service(committed_path);
        require(service.update_palette(valid_palette()), "commit test update was rejected");
        service.wait_for_persistence();
        require(service.persistence_status() ==
                    realmheart::services::ThemeService::PersistenceStatus::Committed,
                "successful persistence must publish committed status");
    }
    std::ifstream committed(committed_path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(committed)), {});
    require(contents.find("realmheart-theme-cache-v1\n") == 0,
            "committed cache must retain its versioned atomic format");

    realmheart::services::ThemeService failed("/proc/realmheart-theme-palette.tsv");
    require(failed.update_palette(valid_palette()), "failure test update was rejected");
    failed.wait_for_persistence();
    require(failed.persistence_status() ==
                realmheart::services::ThemeService::PersistenceStatus::Failed,
            "persistence failure must be observable");
    require(failed.retry_persistence(), "failed persistence must be retryable");
    failed.wait_for_persistence();
    require(failed.persistence_status() ==
                realmheart::services::ThemeService::PersistenceStatus::Failed,
            "retry must remain truthful when the destination is unavailable");
}

int main() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "realmheart-theme-service-XXXXXX").string();
    char* created = ::mkdtemp(pattern.data());
    require(created != nullptr, "test cache directory could not be created");
    const std::filesystem::path root = created;
    const auto cache_path = root / "subscriptions" / "palette.tsv";

    test_subscriptions_and_validation(cache_path);
    test_invalid_update_is_not_published(root / "invalid" / "palette.tsv");
    test_bounded_cache_rejects_excess_physical_lines(root / "bounded" / "palette.tsv");
    test_persistence_commit_and_failure_are_observable(root);
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    std::cout << "ThemeServiceTests passed\n";
    return 0;
}
