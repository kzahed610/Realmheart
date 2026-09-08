#include "services/NotesService.hpp"

#include <chrono>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void test_set_content_is_debounced_and_persisted() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-debounce-test";
    const auto path = root / "notes.txt";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        realmheart::services::NotesService service(path, 100ms);
        service.set_content("first");
        service.set_content("second");

        if (std::filesystem::exists(path)) {
            fail("set_content performed a synchronous disk write");
        }

        std::this_thread::sleep_for(250ms);
        if (read_file(path) != "second") {
            fail("debounced write did not persist the latest content");
        }
    }

    std::filesystem::remove_all(root);
}

void test_destructor_flushes_pending_content() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-flush-test";
    const auto path = root / "notes.txt";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto shutdown_start = std::chrono::steady_clock::now();
    {
        realmheart::services::NotesService service(path, 5s);
        service.set_content("survives shutdown");
    }
    if (std::chrono::steady_clock::now() - shutdown_start > 1s) {
        fail("normal pending shutdown exceeded its bounded teardown contract");
    }

    if (read_file(path) != "survives shutdown") {
        fail("destructor did not flush pending content");
    }

    realmheart::services::NotesService reloaded(path, 100ms);
    if (reloaded.get_content() != "survives shutdown") {
        fail("persisted content did not reload");
    }

    std::filesystem::remove_all(root);
}

void test_save_replaces_existing_file() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-save-test";
    const auto path = root / "notes.txt";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream(path) << "old";
        realmheart::services::NotesService service(path, 5s);
        service.set_content("new");
        if (!service.save()) fail("explicit save reported failure");
    }

    if (read_file(path) != "new") {
        fail("save did not atomically replace existing content");
    }
    if (std::filesystem::exists(path.string() + ".tmp")) {
        fail("temporary file remained after successful save");
    }

    std::filesystem::remove_all(root);
}

void test_save_state_reports_success_and_failure() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-state-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::atomic<int> state{static_cast<int>(realmheart::services::NotesSaveState::Saved)};
        realmheart::services::NotesService service(root / "notes.txt", 40ms);
        service.set_save_state_callback([&](realmheart::services::NotesSaveState next) {
            state.store(static_cast<int>(next));
        });
        service.set_content("stateful");
        if (state.load() != static_cast<int>(realmheart::services::NotesSaveState::Pending)) {
            fail("editing did not publish a pending save state");
        }
        std::this_thread::sleep_for(160ms);
        if (state.load() != static_cast<int>(realmheart::services::NotesSaveState::Saved)) {
            fail("successful persistence did not publish a saved state");
        }
    }

    {
        const auto non_directory = root / "not-a-directory";
        std::ofstream(non_directory) << "blocking parent";
        std::atomic<int> state{static_cast<int>(realmheart::services::NotesSaveState::Saved)};
        realmheart::services::NotesService service(non_directory / "notes.txt", 40ms);
        service.set_save_state_callback([&](realmheart::services::NotesSaveState next) {
            state.store(static_cast<int>(next));
        });
        if (!service.acknowledge_load_failure()) {
            fail("failed-path test could not acknowledge its intentional load failure");
        }
        service.set_content("cannot persist");
        std::this_thread::sleep_for(160ms);
        if (state.load() != static_cast<int>(realmheart::services::NotesSaveState::Failed)) {
            fail("persistence failure was not exposed through the save state");
        }
    }

    std::filesystem::remove_all(root);
}

void test_load_failures_never_become_empty_overwrites() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-load-failure-test";
    const auto blocked_parent = root / "blocked";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(blocked_parent) << "preserved";

    realmheart::services::NotesService service(blocked_parent / "notes.txt", 20ms);
    if (service.save_state() != realmheart::services::NotesSaveState::LoadFailed) {
        fail("non-directory note parent was not reported as a load failure");
    }
    if (!service.get_content().empty()) {
        fail("failed note load exposed fabricated content");
    }
    if (service.set_content("replacement")) {
        fail("content edit bypassed an unacknowledged load failure");
    }
    if (read_file(blocked_parent) != "preserved") {
        fail("load failure path was overwritten");
    }

    std::filesystem::remove_all(root);
}

void test_non_regular_paths_return_promptly() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-non-regular-test";
    const auto fifo = root / "notes.fifo";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    if (::mkfifo(fifo.c_str(), 0600) != 0) fail("could not create FIFO fixture");

    const pid_t child = ::fork();
    if (child < 0) fail("could not fork bounded FIFO probe");
    if (child == 0) {
        realmheart::services::NotesService service(fifo, 20ms);
        _exit(service.save_state() == realmheart::services::NotesSaveState::LoadFailed ? 0 : 1);
    }

    int status = 0;
    bool exited = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            exited = true;
            break;
        }
        if (result < 0 && errno != EINTR) break;
        std::this_thread::sleep_for(10ms);
    }
    if (!exited) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
        std::filesystem::remove_all(root);
        fail("NotesService construction blocked on a FIFO");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::filesystem::remove_all(root);
        fail("FIFO activation did not report a bounded load failure");
    }

    std::filesystem::remove_all(root);
}

void test_malformed_and_oversized_notes_are_rejected() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-validation-test";
    const auto malformed = root / "malformed.txt";
    const auto oversized = root / "oversized.txt";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        std::ofstream file(malformed, std::ios::binary);
        const char malformed_bytes[] = "prefix\0suffix";
        file.write(malformed_bytes, sizeof(malformed_bytes) - 1);
    }
    {
        std::ofstream file(oversized, std::ios::binary);
        std::string content(realmheart::services::NotesService::max_note_bytes + 1, 'x');
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    realmheart::services::NotesService malformed_service(malformed, 20ms);
    realmheart::services::NotesService oversized_service(oversized, 20ms);
    if (malformed_service.save_state() != realmheart::services::NotesSaveState::LoadFailed ||
        oversized_service.save_state() != realmheart::services::NotesSaveState::LoadFailed) {
        fail("malformed or oversized note was accepted as loaded text");
    }

    std::filesystem::remove_all(root);
}

void test_unique_temporary_files_do_not_follow_predictable_entries() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-secure-file-test";
    const auto path = root / "notes.txt";
    const auto target = root / "unrelated.txt";
    const auto predictable = root / "notes.txt.tmp";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(target) << "must remain";
    std::ofstream(path) << "old";
    std::filesystem::create_symlink(target, predictable);

    {
        realmheart::services::NotesService service(path, 20ms);
        service.set_content("new");
        if (!service.save()) fail("secure atomic save rejected a valid note");
    }

    if (read_file(path) != "new" || read_file(target) != "must remain") {
        fail("atomic save followed a predictable temporary symlink");
    }
    if (!std::filesystem::is_symlink(predictable)) {
        fail("secure save consumed the pre-existing temporary entry");
    }

    std::filesystem::remove_all(root);
}

void test_relative_note_paths_are_rejected() {
    realmheart::services::NotesService service("relative-notes.txt", 20ms);
    if (service.save_state() != realmheart::services::NotesSaveState::LoadFailed) {
        fail("relative note path was accepted");
    }
    if (service.set_content("must not escape")) {
        fail("relative note path accepted content");
    }
}

void test_newer_generation_wins_over_concurrent_save() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-generation-test";
    const auto path = root / "notes.txt";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(path) << "seed";

    const std::string older_content(256 * 1024, 'o');
    for (int attempt = 0; attempt < 6; ++attempt) {
        realmheart::services::NotesService service(path, 10ms);
        if (!service.set_content(older_content)) fail("could not prepare stale-generation save");

        std::atomic<bool> save_started{false};
        std::thread saver([&] {
            save_started.store(true, std::memory_order_release);
            (void)service.save();
        });
        while (!save_started.load(std::memory_order_acquire)) std::this_thread::yield();
        if (!service.set_content("newest")) fail("new generation edit was rejected");
        saver.join();

        std::this_thread::sleep_for(120ms);
        if (read_file(path) != "newest") {
            fail("a stale save committed over a newer generation");
        }
    }

    std::filesystem::remove_all(root);
}

void test_permanent_failure_is_latched_until_new_edit() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-failure-latch-test";
    const auto blocked_parent = root / "blocked";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(blocked_parent) << "blocking parent";

    realmheart::services::NotesService service(blocked_parent / "notes.txt", 20ms);
    service.acknowledge_load_failure();
    if (!service.set_content("cannot persist")) fail("failed to prepare retry latch test");
    std::this_thread::sleep_for(100ms);
    if (service.save_state() != realmheart::services::NotesSaveState::Failed) {
        fail("permanent persistence failure was not reported");
    }
    std::this_thread::sleep_for(120ms);
    if (service.save_state() != realmheart::services::NotesSaveState::Failed) {
        fail("permanent persistence failure was retried without an explicit action");
    }

    std::filesystem::remove_all(root);
}

void test_edited_text_is_bounded_and_validated() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-notes-edit-validation-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    realmheart::services::NotesService service(root / "notes.txt", 20ms);

    std::string oversized(realmheart::services::NotesService::max_note_bytes + 1, 'x');
    if (service.set_content(oversized)) fail("oversized edit was accepted");
    const std::string invalid_utf8("bad\xfftext", 8);
    if (service.set_content(invalid_utf8)) fail("invalid UTF-8 edit was accepted");
    if (!service.set_content("valid")) fail("valid bounded edit was rejected");
    if (!service.save() || read_file(root / "notes.txt") != "valid") {
        fail("valid bounded edit was not persisted");
    }

    std::filesystem::remove_all(root);
}

void test_default_path_rejects_relative_xdg_configuration() {
    const char* old_xdg = std::getenv("XDG_CONFIG_HOME");
    const char* old_home = std::getenv("HOME");
    const std::string saved_xdg = old_xdg != nullptr ? old_xdg : "";
    const std::string saved_home = old_home != nullptr ? old_home : "";
    const bool had_xdg = old_xdg != nullptr;
    const bool had_home = old_home != nullptr;

    setenv("XDG_CONFIG_HOME", "relative-config", 1);
    setenv("HOME", "relative-home", 1);
    realmheart::services::NotesService service;
    if (!std::filesystem::path(service.get_file_path()).is_absolute()) {
        fail("relative XDG/HOME values redirected the default notes path");
    }

    if (had_xdg) setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
    else unsetenv("XDG_CONFIG_HOME");
    if (had_home) setenv("HOME", saved_home.c_str(), 1);
    else unsetenv("HOME");
}

} // namespace

int main() {
    test_set_content_is_debounced_and_persisted();
    test_destructor_flushes_pending_content();
    test_save_replaces_existing_file();
    test_save_state_reports_success_and_failure();
    test_load_failures_never_become_empty_overwrites();
    test_non_regular_paths_return_promptly();
    test_malformed_and_oversized_notes_are_rejected();
    test_unique_temporary_files_do_not_follow_predictable_entries();
    test_relative_note_paths_are_rejected();
    test_newer_generation_wins_over_concurrent_save();
    test_permanent_failure_is_latched_until_new_edit();
    test_edited_text_is_bounded_and_validated();
    test_default_path_rejects_relative_xdg_configuration();
    std::cout << "NotesService tests PASSED\n";
    return 0;
}
