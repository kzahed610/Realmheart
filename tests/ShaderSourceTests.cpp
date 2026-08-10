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

TEST(ShaderSourceTests, LoadsWorldscarReferenceShaderAndValidatesContract) {
    std::string error;
    const auto shader = load_shader_source(
        "worldscar/reference/reference.frag",
        &error
    );
    ASSERT_TRUE(shader.has_value()) << error;

    std::string missing;
    EXPECT_TRUE(validate_worldscar_reference_shader_contract(
        shader->text,
        &missing
    )) << missing;
    EXPECT_NE(shader->text.find("openProgress <= 0.0005"), std::string::npos);
    EXPECT_NE(shader->text.find("commitProgress >= 0.9995"), std::string::npos);
    EXPECT_EQ(shader->text.find("baseTex"), std::string::npos);
    EXPECT_NE(shader->text.find("vec2(0.355, 0.50)"), std::string::npos);
}

TEST(ShaderSourceTests, LoadsWorldscarProductionShaderAndValidatesContract) {
    std::string error;
    const auto shader = load_shader_source(
        "worldscar/worldscar.frag",
        &error
    );
    ASSERT_TRUE(shader.has_value()) << error;

    std::string missing;
    EXPECT_TRUE(validate_worldscar_shader_contract(
        shader->text,
        &missing
    )) << missing;
    EXPECT_NE(shader->text.find("openProgress <= 0.0005"), std::string::npos);
    EXPECT_NE(shader->text.find("finishProgress >= 0.9995"), std::string::npos);
    EXPECT_EQ(shader->text.find("baseTex"), std::string::npos);
    EXPECT_NE(shader->text.find("selected_world_field"), std::string::npos);
    EXPECT_NE(shader->text.find("previous_world_field"), std::string::npos);
    EXPECT_NE(shader->text.find("next_world_field"), std::string::npos);
    EXPECT_EQ(shader->text.find("connective_fracture_field"), std::string::npos);
    EXPECT_EQ(shader->text.find("micro_jitter"), std::string::npos);
    EXPECT_NE(shader->text.find("vec2 worldscar_domain_warp("), std::string::npos);
    EXPECT_NE(shader->text.find("vec3 content_reactive_rim("), std::string::npos);
    EXPECT_NE(shader->text.find("float worldscar_ember_field("), std::string::npos);
    EXPECT_NE(
        shader->text.find("const float authored_segment_alpha = 0.0;"),
        std::string::npos
    );
    EXPECT_NE(shader->text.find("float trench_layer_alpha ="), std::string::npos);
    EXPECT_NE(
        shader->text.find("float violet_body_layer_alpha ="),
        std::string::npos
    );
    EXPECT_NE(shader->text.find("float hot_core_layer_alpha ="), std::string::npos);
    EXPECT_NE(shader->text.find("float corona_layer_alpha ="), std::string::npos);
    EXPECT_NE(
        shader->text.find("float procedural_branch_trench_alpha ="),
        std::string::npos
    );
    EXPECT_NE(shader->text.find("navigationProgress"), std::string::npos);
    EXPECT_NE(shader->text.find("preview_local_uv"), std::string::npos);
    EXPECT_NE(shader->text.find("residual_slash_field"), std::string::npos);
    EXPECT_NE(shader->text.find("opening_frontier"), std::string::npos);
    EXPECT_NE(shader->text.find("previousFarTex"), std::string::npos);
    EXPECT_NE(shader->text.find("nextFarTex"), std::string::npos);
    EXPECT_EQ(
        shader->text.find("float previous_open = smoothstep"),
        std::string::npos
    );
    // Verify the authored terminal geometry itself instead of coupling the
    // test to prose in a shader comment.
    EXPECT_NE(shader->text.find("scar_point(vec2(0.070, 0.955), aspect)"), std::string::npos);
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
