#include "effects/core/ShaderSource.hpp"

#include <gtest/gtest.h>

namespace realmheart::effects {
namespace {

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

TEST(ShaderSourceTests, ReportsMissingWorkspaceMorphContractSymbol) {
    std::string missing;
    EXPECT_FALSE(validate_workspace_morph_shader_contract(
        "uniform float progress; out vec4 fragColor;",
        &missing
    ));
    EXPECT_EQ(missing, "uniform float opening");
}

} // namespace
} // namespace realmheart::effects
