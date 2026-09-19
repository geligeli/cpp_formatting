#include "code_browser/sqlite_util.h"

#include <sqlite3.h>

#include <cstddef>
#include <utility>

namespace code_browser {

Db::Db(Db&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

auto Db::operator=(Db&& other) noexcept -> Db& {
  if (this != &other) {
    Close();
    db_ = std::exchange(other.db_, nullptr);
  }
  return *this;
}

Db::~Db() { Close(); }

auto Db::Open(const std::string& path, int flags) -> std::string {
  Close();
  const int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string message = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
    Close();
    return message;
  }
  return "";
}

void Db::Close() {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

auto Db::Exec(std::string_view sql) -> std::string {
  char* message = nullptr;
  const std::string owned(sql);
  const int rc = sqlite3_exec(db_, owned.c_str(), nullptr, nullptr, &message);
  std::string out;
  if (rc != SQLITE_OK) out = message ? message : sqlite3_errstr(rc);
  sqlite3_free(message);
  return out;
}

auto Db::ErrorMessage() const -> std::string {
  return db_ ? sqlite3_errmsg(db_) : "no database";
}

Statement::Statement(Statement&& other) noexcept
    : stmt_(std::exchange(other.stmt_, nullptr)),
      error_(std::move(other.error_)) {}

auto Statement::operator=(Statement&& other) noexcept -> Statement& {
  if (this != &other) {
    sqlite3_finalize(stmt_);
    stmt_ = std::exchange(other.stmt_, nullptr);
    error_ = std::move(other.error_);
  }
  return *this;
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

auto Statement::Prepare(sqlite3* db, std::string_view sql) -> std::string {
  sqlite3_finalize(stmt_);
  stmt_ = nullptr;
  const int rc = sqlite3_prepare_v2(
      db, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
  if (rc != SQLITE_OK)
    return std::string(sqlite3_errmsg(db)) + " in: " + std::string(sql);
  return "";
}

void Statement::BindInt(int index, int64_t value) {
  sqlite3_bind_int64(stmt_, index, value);
}

void Statement::BindNull(int index) { sqlite3_bind_null(stmt_, index); }

void Statement::BindText(int index, std::string_view value) {
  sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

auto Statement::Step() -> bool {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc != SQLITE_DONE) error_ = sqlite3_errstr(rc);
  return false;
}

auto Statement::Reset() -> void {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
  error_.clear();
}

auto Statement::ColumnInt(int index) const -> int64_t {
  return sqlite3_column_int64(stmt_, index);
}

auto Statement::ColumnText(int index) const -> std::string_view {
  const auto* text =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
  if (text == nullptr) return {};
  return {text, static_cast<size_t>(sqlite3_column_bytes(stmt_, index))};
}

auto Statement::ColumnIsNull(int index) const -> bool {
  return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

}  // namespace code_browser
