#include "effects/core/ShaderSource.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <glib.h>

namespace realmheart::effects {
namespace {

std::string shell_contract_with(std::string progress_declaration) {
    return progress_declaration + R"GLSL(
uniform vec2 resolution;
uniform sampler2D tex;
uniform float radius;
uniform float reverse;
uniform vec3 uGold;
uniform vec3 uStarlight;
uniform vec3 uAstral;
uniform vec3 uVoid;
out vec4 fragColor;
)GLSL";
}

TEST(ShaderSourceTests, RejectsUnsafeAndNonFragmentPaths) {
    EXPECT_FALSE(is_safe_shader_asset_path(""));
    EXPECT_FALSE(is_safe_shader_asset_path("/tmp/void.frag"));
    EXPECT_FALSE(is_safe_shader_asset_path("../void.frag"));
    EXPECT_FALSE(is_safe_shader_asset_path("void/../../escape.frag"));
    EXPECT_FALSE(is_safe_shader_asset_path("void/void.vert"));
    EXPECT_TRUE(is_safe_shader_asset_path("windows/void/void.frag"));
}

TEST(ShaderSourceTests, LoadsRealmheartVoidShader) {
    std::string error;
    const auto shader = load_shader_source("windows/void/void.frag", &error);

    ASSERT_TRUE(shader.has_value()) << error;
    EXPECT_FALSE(shader->text.empty());
    EXPECT_EQ(shader->path.filename(), "void.frag");
}

TEST(ShaderSourceTests, VoidShaderSatisfiesShellContract) {
    std::string error;
    const auto shader = load_shader_source("windows/void/void.frag", &error);
    ASSERT_TRUE(shader.has_value()) << error;

    std::string missing;
    EXPECT_TRUE(validate_shell_shader_contract(shader->text, &missing)) << missing;
}

TEST(ShaderSourceTests, LoadsPowerMenuRippleShaderAndValidatesContract) {
    std::string error;
    const auto shader = load_shader_source(
        "power-menu/ripple-reveal/ripple-reveal.frag",
        &error
    );
    ASSERT_TRUE(shader.has_value()) << error;

    std::string missing;
    EXPECT_TRUE(validate_power_menu_ripple_shader_contract(shader->text, &missing))
        << missing;
}

TEST(ShaderSourceTests, LoadsWorkspaceMorphShaderAndValidatesContract) {
    std::string error;
    const auto shader = load_shader_source(
        "workspace/elemental-morph/elemental-morph.frag",
        &error
    );
    ASSERT_TRUE(shader.has_value()) << error;

    std::string missing;
    EXPECT_TRUE(validate_workspace_morph_shader_contract(
        shader->text,
        &missing
    )) << missing;
    EXPECT_NE(shader->text.find("progress <= 0.0005"), std::string::npos);
    EXPECT_NE(shader->text.find("progress >= 0.9995"), std::string::npos);
    EXPECT_NE(shader->text.find("texture(tex"), std::string::npos);
    EXPECT_NE(shader->text.find("source_trail"), std::string::npos);
    EXPECT_NE(shader->text.find("materialized"), std::string::npos);
}

TEST(ShaderSourceTests, ReportsMissingContractSymbol) {
    std::string missing;
    EXPECT_FALSE(validate_shell_shader_contract(
        "uniform float progress; out vec4 fragColor;",
        &missing
    ));
    EXPECT_EQ(missing, "uniform vec2 resolution");
}

TEST(ShaderSourceTests, ContractIgnoresCommentsAndIdentifierSuffixes) {
    std::string missing;
    EXPECT_FALSE(validate_shell_shader_contract(
        shell_contract_with(R"GLSL(
// uniform float progress
uniform float progress_extra;
)GLSL"),
        &missing
    ));
    EXPECT_EQ(missing, "uniform float progress");
}

TEST(ShaderSourceTests, AsyncLoadDeliversOnTheMainContext) {
    bool callback_called = false;
    const auto calling_thread = std::this_thread::get_id();
    std::thread::id callback_thread;
    std::optional<ShaderSource> loaded_source;
    std::string error;
    ASSERT_TRUE(load_shader_source_async(
        "windows/void/void.frag",
        [&](std::optional<ShaderSource> source, std::string load_error) {
            callback_thread = std::this_thread::get_id();
            loaded_source = std::move(source);
            error = std::move(load_error);
            callback_called = true;
        }
    ));

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!callback_called && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_TRUE(callback_called);
    EXPECT_EQ(callback_thread, calling_thread);
    ASSERT_TRUE(loaded_source.has_value()) << error;
    EXPECT_FALSE(loaded_source->text.empty());
}

TEST(ShaderSourceTests, RejectsOversizedShaderFiles) {
    const auto directory =
        std::filesystem::temp_directory_path() / "realmheart-shader-source-test";
    const auto path = directory / "oversized.frag";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output.good());
        output.seekp((1U << 20));
        output.put('x');
    }

    ASSERT_EQ(setenv("REALMHEART_EFFECT_DIR", directory.c_str(), 1), 0);
    std::string error;
    const auto shader = load_shader_source("oversized.frag", &error);
    EXPECT_FALSE(shader.has_value());
    EXPECT_NE(error.find("1 MiB"), std::string::npos);
    ASSERT_EQ(unsetenv("REALMHEART_EFFECT_DIR"), 0);
    std::filesystem::remove_all(directory);
}

TEST(ShaderSourceTests, ReportsMissingWorkspaceMorphContractSymbol) {
    std::string missing;
    EXPECT_FALSE(validate_workspace_morph_shader_contract(
        "uniform float progress; out vec4 fragColor;",
        &missing
    ));
    EXPECT_EQ(missing, "uniform float opening");
}

TEST(ShaderSourceTests, LoadsLockscreenScalesShaderAndValidatesContract) {
    std::string error;
    const auto shader = load_shader_source(
        "lockscreen/scales/scales.frag",
        &error
    );
    ASSERT_TRUE(shader.has_value()) << error;

    std::string missing;
    EXPECT_TRUE(validate_lockscreen_shader_contract(shader->text, &missing))
        << missing;
    EXPECT_NE(shader->text.find("uWarn"), std::string::npos);
    EXPECT_NE(shader->text.find("uLit"), std::string::npos);
}

TEST(ShaderSourceTests, ReportsMissingLockscreenContractSymbol) {
    std::string missing;
    EXPECT_FALSE(validate_lockscreen_shader_contract(
        "uniform float uTime; out vec4 fragColor;",
        &missing
    ));
    EXPECT_EQ(missing, "uniform vec2 uResolution");
}

} // namespace
} // namespace realmheart::effects
