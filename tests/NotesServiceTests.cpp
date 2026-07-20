#include "services/NotesService.hpp"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

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

    {
        realmheart::services::NotesService service(path, 5s);
        service.set_content("survives shutdown");
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
        service.set_content("cannot persist");
        std::this_thread::sleep_for(160ms);
        if (state.load() != static_cast<int>(realmheart::services::NotesSaveState::Failed)) {
            fail("persistence failure was not exposed through the save state");
        }
    }

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_set_content_is_debounced_and_persisted();
    test_destructor_flushes_pending_content();
    test_save_replaces_existing_file();
    test_save_state_reports_success_and_failure();
    std::cout << "NotesService tests PASSED\n";
    return 0;
}
