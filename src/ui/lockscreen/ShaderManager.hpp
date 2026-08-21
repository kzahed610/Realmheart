#pragma once

#include <glib.h>

#include <string_view>

namespace realmheart::ui::lockscreen {

// Owns the compiled lockscreen GL programs. Loads shader text from
// effects/lockscreen/, validates it against the lockscreen shader contract,
// compiles + links to a program, and hot-reloads when a shader file changes
// (250 ms watcher). Must only be used while a GL context is current.
class ShaderManager {
public:
    ShaderManager();
    ~ShaderManager();

    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;

    // Compiles (and later hot-reloads) the crystal shader program. Returns
    // false and fills error if the shader cannot be loaded or compiled.
    [[nodiscard]] bool ensure_loaded(std::string* error = nullptr);

    [[nodiscard]] unsigned int crystal_program() const noexcept;

private:
    struct Program;
    struct State;
    State* state_ = nullptr;

    static gboolean reload_tick(gpointer data);
    static void reload_program_if_changed(Program& program);
};

} // namespace realmheart::ui::lockscreen
