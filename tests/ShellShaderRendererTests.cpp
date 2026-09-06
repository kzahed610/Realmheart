#include "effects/shell/ShellShaderRenderer.hpp"

#include <gtest/gtest.h>

namespace realmheart::effects::shell {
namespace {

TEST(ShellShaderRendererTests, CreatesHiddenRendererAndSupportsIdempotentFinish) {
    gtk_init();

    ShellShaderRenderer renderer;
    ASSERT_NE(renderer.widget(), nullptr);
    EXPECT_FALSE(renderer.active());
    EXPECT_FALSE(renderer.frame_ready());

    renderer.finish();
    renderer.finish();
    EXPECT_FALSE(renderer.active());
    EXPECT_FALSE(renderer.frame_ready());
}

} // namespace
} // namespace realmheart::effects::shell
