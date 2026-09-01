#include "ui/workspace/WorkspaceOverviewViewport.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "Workspace overview viewport test failed: " << message << '\n';
        std::exit(1);
    }
}

bool near(double lhs, double rhs, double epsilon = 0.000001) {
    return std::abs(lhs - rhs) <= epsilon;
}

} // namespace

int main() {
    using realmheart::ui::workspace::workspace_overview_viewport_transform;

    const auto p1080 = workspace_overview_viewport_transform(1920, 1080);
    require(near(p1080.scale, 1.0), "1080p must preserve the authored stage exactly");
    require(near(p1080.content_width, 1920.0), "1080p content width must remain canonical");

    const auto p1440 = workspace_overview_viewport_transform(2560, 1440);
    require(near(p1440.scale, 4.0 / 3.0), "1440p must use a uniform 4/3 transform");
    require(near(p1440.content_width, 2560.0), "16:9 1440p must fill horizontally");

    const auto ultrawide = workspace_overview_viewport_transform(3440, 1440);
    require(near(ultrawide.scale, 4.0 / 3.0), "3440x1440 must retain 1440p visual density");
    require(near(ultrawide.content_width, 2560.0), "ultrawide must not stretch the 16:9 stage");
    require(near(ultrawide.content_height, 1440.0), "ultrawide must still fill the output height");
    require(near(ultrawide.to_reference_x(1280.0), 960.0), "input mapping must invert the uniform scale");

    const auto superwide = workspace_overview_viewport_transform(5120, 1440);
    require(near(superwide.scale, 4.0 / 3.0), "5120x1440 must still retain 1440p density");

    const auto portrait = workspace_overview_viewport_transform(1080, 1920);
    require(near(portrait.scale, 1080.0 / 1920.0), "portrait output must contain rather than crop the stage");
    require(portrait.content_height < 1920.0, "portrait output may letterbox vertically but must not distort");

    std::cout << "Workspace overview viewport tests passed\n";
    return 0;
}
