#include "ui/styles/CssModuleLoader.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifndef REALMHEART_INSTALL_STYLE_DIR
#define REALMHEART_INSTALL_STYLE_DIR "share/realmheart/styles"
#endif

#ifndef REALMHEART_SOURCE_STYLE_DIR
#define REALMHEART_SOURCE_STYLE_DIR "styles"
#endif

namespace realmheart::ui::styles {
namespace {

std::filesystem::path executable_directory() {
    std::array<char, 4096> buffer{};
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    return std::filesystem::path(
        std::string(buffer.data(), static_cast<std::size_t>(length))
    ).parent_path();
}

std::vector<std::filesystem::path> style_roots() {
    std::vector<std::filesystem::path> roots;
    if (const char* configured = std::getenv("REALMHEART_STYLE_DIR");
        configured != nullptr && *configured != '\0') {
        roots.emplace_back(configured);
    }

    roots.emplace_back(REALMHEART_INSTALL_STYLE_DIR);
    const auto executable = executable_directory();
    if (!executable.empty()) {
        roots.push_back(executable / "../share/realmheart/styles");
        roots.push_back(executable / "styles");
    }
    roots.emplace_back(REALMHEART_SOURCE_STYLE_DIR);
    roots.emplace_back("styles");
    return roots;
}

} // namespace

std::optional<std::filesystem::path> resolve_style_module(std::string_view relative_path) {
    if (relative_path.empty()) return std::nullopt;

    const std::filesystem::path relative(relative_path);
    if (relative.is_absolute()) return std::nullopt;

    for (const auto& root : style_roots()) {
        std::error_code error;
        const auto canonical_root = std::filesystem::weakly_canonical(root, error);
        if (error) continue;

        const auto candidate = std::filesystem::weakly_canonical(canonical_root / relative, error);
        if (error) continue;

        const auto mismatch = std::mismatch(
            canonical_root.begin(), canonical_root.end(), candidate.begin(), candidate.end()
        );
        if (mismatch.first != canonical_root.end()) continue;
        if (!std::filesystem::is_regular_file(candidate, error) || error) continue;
        return candidate;
    }
    return std::nullopt;
}

std::string load_css_modules(std::span<const std::string_view> module_paths) {
    std::string combined;

    for (const std::string_view module_path : module_paths) {
        const auto resolved = resolve_style_module(module_path);
        if (!resolved) {
            throw std::runtime_error(
                "Unable to resolve Realmheart CSS module: " + std::string(module_path)
            );
        }

        std::ifstream input(*resolved, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "Unable to read Realmheart CSS module: " + resolved->string()
            );
        }

        std::ostringstream contents;
        contents << input.rdbuf();
        combined += "\n/* module: " + std::string(module_path) + " */\n";
        combined += contents.str();
        if (combined.empty() || combined.back() != '\n') combined.push_back('\n');
    }

    return combined;
}

} // namespace realmheart::ui::styles
