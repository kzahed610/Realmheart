#include "events/EventPersistence.hpp"

#include "events/EventProtocol.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace realmheart::events {
namespace {

class Statement {
public:
    Statement(sqlite3* database, const char* sql, std::string& error) {
        const int result = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
            error = sqlite3_errmsg(database);
            statement_ = nullptr;
        }
    }
    ~Statement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    [[nodiscard]] explicit operator bool() const noexcept { return statement_ != nullptr; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

bool exec_sql(sqlite3* database, const char* sql, std::string& error) {
    char* message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) return true;
    error = message != nullptr ? message : sqlite3_errmsg(database);
    sqlite3_free(message);
    return false;
}

bool bind_text(sqlite3_stmt* statement, int index, const std::string& value) {
    return sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string column_text(sqlite3_stmt* statement, int index) {
    const auto* text = sqlite3_column_text(statement, index);
    if (text == nullptr) return {};
    return reinterpret_cast<const char*>(text);
}

std::optional<Event> decode_event(sqlite3_stmt* statement, int payload_column) {
    try {
        const std::string payload = column_text(statement, payload_column);
        ValidationResult validation;
        auto event = event_from_json(Json::parse(payload), validation);
        if (!event || !validation.ok) return std::nullopt;
        return event;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

} // namespace

EventPersistence::~EventPersistence() { close(); }

std::string EventPersistence::default_database_path() {
    std::string state_root;
    if (const char* configured = std::getenv("XDG_STATE_HOME"); configured != nullptr && *configured != '\0') {
        state_root = configured;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        state_root = std::string(home) + "/.local/state";
    } else {
        state_root = std::string("/tmp/realmheart-state-") + std::to_string(::getuid());
    }
    return state_root + "/realmheart/events.db";
}

bool EventPersistence::open(std::string& error) {
    close();
    path_ = default_database_path();
    const std::filesystem::path path(path_);
    std::error_code fs_error;
    std::filesystem::create_directories(path.parent_path(), fs_error);
    if (fs_error) {
        error = "unable to create state directory: " + fs_error.message();
        return false;
    }
    if (::chmod(path.parent_path().c_str(), S_IRWXU) != 0) {
        error = "unable to secure state directory";
        return false;
    }

    sqlite3* opened = nullptr;
    const int result = sqlite3_open_v2(
        path_.c_str(),
        &opened,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (result != SQLITE_OK) {
        error = opened != nullptr ? sqlite3_errmsg(opened) : "unable to open SQLite database";
        if (opened != nullptr) sqlite3_close(opened);
        return false;
    }
    database_ = opened;
    sqlite3_busy_timeout(database_, 1500);
    if (::chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
        error = "unable to secure event database";
        close();
        return false;
    }
    if (!migrate(error)) {
        close();
        return false;
    }

    // Non-persistent active rows belong to a dead daemon instance and must not
    // become phantom history/state after a restart.
    if (!exec_sql(database_, "DELETE FROM events WHERE state='active' AND persistent=0;", error)) {
        close();
        return false;
    }
    return true;
}

void EventPersistence::close() {
    if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
    }
}

bool EventPersistence::migrate(std::string& error) {
    static constexpr const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS schema_meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT OR IGNORE INTO schema_meta(key, value) VALUES('database_version', '1');
CREATE TABLE IF NOT EXISTS events (
    source_id TEXT NOT NULL,
    event_id TEXT NOT NULL,
    revision INTEGER NOT NULL,
    severity TEXT NOT NULL,
    presentation TEXT NOT NULL,
    title TEXT NOT NULL,
    summary TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    state TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    resolved_at TEXT,
    acknowledged INTEGER NOT NULL DEFAULT 0,
    dismissed INTEGER NOT NULL DEFAULT 0,
    persistent INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(source_id, event_id)
);
CREATE INDEX IF NOT EXISTS events_history_order
    ON events(state, dismissed, revision DESC);
CREATE TABLE IF NOT EXISTS sources (
    source_id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    icon TEXT NOT NULL,
    uid INTEGER NOT NULL,
    last_pid INTEGER NOT NULL,
    executable TEXT NOT NULL,
    trust_class TEXT NOT NULL,
    first_seen TEXT NOT NULL,
    last_seen TEXT NOT NULL
);
)SQL";
    return exec_sql(database_, kSchema, error);
}

bool EventPersistence::save_event(const Event& event, bool dismissed, std::string& error) {
    if (database_ == nullptr) {
        error = "database unavailable";
        return false;
    }
    static constexpr const char* kSql = R"SQL(
INSERT INTO events(
    source_id, event_id, revision, severity, presentation, title, summary,
    payload_json, state, created_at, updated_at, resolved_at,
    acknowledged, dismissed, persistent
) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(source_id, event_id) DO UPDATE SET
    revision=excluded.revision,
    severity=excluded.severity,
    presentation=excluded.presentation,
    title=excluded.title,
    summary=excluded.summary,
    payload_json=excluded.payload_json,
    state=excluded.state,
    updated_at=excluded.updated_at,
    resolved_at=excluded.resolved_at,
    acknowledged=excluded.acknowledged,
    dismissed=excluded.dismissed,
    persistent=excluded.persistent;
)SQL";
    Statement statement(database_, kSql, error);
    if (!statement) return false;

    const std::string now = now_iso8601_utc();
    const std::string encoded = event_to_json(event).dump();
    const std::string state = to_string(event.lifecycle.state);
    const bool resolved = event.lifecycle.state == LifecycleState::Resolved;
    int index = 1;
    const bool bound =
        bind_text(statement.get(), index++, event.source.id) &&
        bind_text(statement.get(), index++, event.id) &&
        sqlite3_bind_int64(statement.get(), index++, static_cast<sqlite3_int64>(event.revision)) == SQLITE_OK &&
        bind_text(statement.get(), index++, to_string(event.severity)) &&
        bind_text(statement.get(), index++, to_string(event.presentation)) &&
        bind_text(statement.get(), index++, event.title) &&
        bind_text(statement.get(), index++, event.summary) &&
        bind_text(statement.get(), index++, encoded) &&
        bind_text(statement.get(), index++, state) &&
        bind_text(statement.get(), index++, now) &&
        bind_text(statement.get(), index++, now) &&
        (resolved ? bind_text(statement.get(), index++, now)
                  : sqlite3_bind_null(statement.get(), index++) == SQLITE_OK) &&
        sqlite3_bind_int(statement.get(), index++, event.lifecycle.acknowledged ? 1 : 0) == SQLITE_OK &&
        sqlite3_bind_int(statement.get(), index++, dismissed ? 1 : 0) == SQLITE_OK &&
        sqlite3_bind_int(statement.get(), index++, event.lifecycle.persistent ? 1 : 0) == SQLITE_OK;
    if (!bound) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool EventPersistence::save_active(const Event& event, std::string& error) {
    return save_event(event, false, error);
}

bool EventPersistence::save_resolved(const Event& event, std::string& error) {
    return save_event(event, false, error);
}

bool EventPersistence::save_dismissed(const Event& event, std::string& error) {
    return save_event(event, true, error);
}

bool EventPersistence::erase(const EventKey& key, std::string& error) {
    if (database_ == nullptr) {
        error = "database unavailable";
        return false;
    }
    Statement statement(database_, "DELETE FROM events WHERE source_id=? AND event_id=?;", error);
    if (!statement) return false;
    if (!bind_text(statement.get(), 1, key.source_id) || !bind_text(statement.get(), 2, key.event_id)) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

std::vector<Event> EventPersistence::load_active(std::string& error) const {
    std::vector<Event> result;
    if (database_ == nullptr) {
        error = "database unavailable";
        return result;
    }
    Statement statement(
        database_,
        "SELECT payload_json FROM events WHERE state='active' AND dismissed=0 AND persistent=1 ORDER BY revision DESC;",
        error
    );
    if (!statement) return result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        if (auto event = decode_event(statement.get(), 0)) result.push_back(std::move(*event));
    }
    return result;
}

std::vector<Event> EventPersistence::history(std::size_t limit, std::string& error) const {
    std::vector<Event> result;
    if (database_ == nullptr) {
        error = "database unavailable";
        return result;
    }
    const std::size_t bounded_limit = std::clamp<std::size_t>(limit, 1U, 1000U);
    Statement statement(
        database_,
        "SELECT payload_json FROM events WHERE state='resolved' OR dismissed=1 ORDER BY revision DESC LIMIT ?;",
        error
    );
    if (!statement) return result;
    sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(bounded_limit));
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        if (auto event = decode_event(statement.get(), 0)) result.push_back(std::move(*event));
    }
    return result;
}

std::uint64_t EventPersistence::max_revision(std::string& error) const {
    if (database_ == nullptr) {
        error = "database unavailable";
        return 0;
    }
    Statement statement(database_, "SELECT COALESCE(MAX(revision), 0) FROM events;", error);
    if (!statement) return 0;
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        error = sqlite3_errmsg(database_);
        return 0;
    }
    const sqlite3_int64 revision = sqlite3_column_int64(statement.get(), 0);
    return revision > 0 ? static_cast<std::uint64_t>(revision) : 0U;
}

bool EventPersistence::clear_history(std::string& error) {
    if (database_ == nullptr) {
        error = "database unavailable";
        return false;
    }
    return exec_sql(database_, "DELETE FROM events WHERE state='resolved' OR dismissed=1;", error);
}

bool EventPersistence::cleanup_history(int max_days, std::size_t max_events, std::string& error) {
    if (database_ == nullptr) {
        error = "database unavailable";
        return false;
    }
    const int bounded_days = std::clamp(max_days, 1, 3650);
    const std::size_t bounded_events = std::clamp<std::size_t>(max_events, 1U, 1000000U);

    Statement age_statement(
        database_,
        "DELETE FROM events WHERE (state='resolved' OR dismissed=1) AND julianday(updated_at) < julianday('now', ?);",
        error
    );
    if (!age_statement) return false;
    const std::string age = "-" + std::to_string(bounded_days) + " days";
    if (!bind_text(age_statement.get(), 1, age) || sqlite3_step(age_statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }

    Statement count_statement(
        database_,
        "DELETE FROM events WHERE rowid IN ("
        " SELECT rowid FROM events WHERE state='resolved' OR dismissed=1"
        " ORDER BY revision DESC LIMIT -1 OFFSET ?"
        ");",
        error
    );
    if (!count_statement) return false;
    sqlite3_bind_int64(count_statement.get(), 1, static_cast<sqlite3_int64>(bounded_events));
    if (sqlite3_step(count_statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

bool EventPersistence::upsert_source(const SourceRecord& source, std::string& error) {
    if (database_ == nullptr) {
        error = "database unavailable";
        return false;
    }
    static constexpr const char* kSql = R"SQL(
INSERT INTO sources(source_id, display_name, icon, uid, last_pid, executable, trust_class, first_seen, last_seen)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(source_id) DO UPDATE SET
    display_name=excluded.display_name,
    icon=excluded.icon,
    uid=excluded.uid,
    last_pid=excluded.last_pid,
    executable=excluded.executable,
    trust_class=excluded.trust_class,
    last_seen=excluded.last_seen;
)SQL";
    Statement statement(database_, kSql, error);
    if (!statement) return false;
    int index = 1;
    const bool bound =
        bind_text(statement.get(), index++, source.id) &&
        bind_text(statement.get(), index++, source.display_name) &&
        bind_text(statement.get(), index++, source.icon) &&
        sqlite3_bind_int64(statement.get(), index++, static_cast<sqlite3_int64>(source.uid)) == SQLITE_OK &&
        sqlite3_bind_int64(statement.get(), index++, static_cast<sqlite3_int64>(source.last_pid)) == SQLITE_OK &&
        bind_text(statement.get(), index++, source.executable) &&
        bind_text(statement.get(), index++, source.trust_class) &&
        bind_text(statement.get(), index++, source.first_seen) &&
        bind_text(statement.get(), index++, source.last_seen);
    if (!bound || sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
    }
    return true;
}

std::vector<SourceRecord> EventPersistence::sources(std::string& error) const {
    std::vector<SourceRecord> result;
    if (database_ == nullptr) {
        error = "database unavailable";
        return result;
    }
    Statement statement(
        database_,
        "SELECT source_id, display_name, icon, uid, last_pid, executable, trust_class, first_seen, last_seen "
        "FROM sources ORDER BY last_seen DESC;",
        error
    );
    if (!statement) return result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        SourceRecord source;
        source.id = column_text(statement.get(), 0);
        source.display_name = column_text(statement.get(), 1);
        source.icon = column_text(statement.get(), 2);
        source.uid = static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 3));
        source.last_pid = static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 4));
        source.executable = column_text(statement.get(), 5);
        source.trust_class = column_text(statement.get(), 6);
        source.first_seen = column_text(statement.get(), 7);
        source.last_seen = column_text(statement.get(), 8);
        result.push_back(std::move(source));
    }
    return result;
}

} // namespace realmheart::events
