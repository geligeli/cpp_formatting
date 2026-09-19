// Thin RAII wrappers over the sqlite3 C API, shared by the importer and the
// reader.  Errors are returned as strings ("" is success); nothing throws.
#ifndef CODE_BROWSER_SQLITE_UTIL_H_
#define CODE_BROWSER_SQLITE_UTIL_H_

#include <cstdint>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace code_browser {

// An open connection.  Movable, closes on destruction.
class Db {
 public:
  Db() = default;
  Db(const Db&) = delete;
  auto operator=(const Db&) -> Db& = delete;
  Db(Db&& other) noexcept;
  auto operator=(Db&& other) noexcept -> Db&;
  ~Db();

  // `flags` are SQLITE_OPEN_* bits.  Returns "" or sqlite's message.
  auto Open(const std::string& path, int flags) -> std::string;
  void Close();
  auto handle() const -> sqlite3* { return db_; }
  auto is_open() const -> bool { return db_ != nullptr; }

  // Runs one or more statements with no result rows.
  auto Exec(std::string_view sql) -> std::string;
  auto ErrorMessage() const -> std::string;

 private:
  sqlite3* db_ = nullptr;
};

// A prepared statement.  Bind with 1-based parameter indexes, then Step()
// until it returns false; Reset() to run again.
class Statement {
 public:
  Statement() = default;
  Statement(const Statement&) = delete;
  auto operator=(const Statement&) -> Statement& = delete;
  Statement(Statement&& other) noexcept;
  auto operator=(Statement&& other) noexcept -> Statement&;
  ~Statement();

  auto Prepare(sqlite3* db, std::string_view sql) -> std::string;
  auto handle() const -> sqlite3_stmt* { return stmt_; }

  void BindInt(int index, int64_t value);
  void BindNull(int index);
  // The text is copied by sqlite (SQLITE_TRANSIENT); safe with temporaries.
  void BindText(int index, std::string_view value);

  // True while a row is available; false at SQLITE_DONE.  A failure sets
  // error() and returns false.
  auto Step() -> bool;
  auto Reset() -> void;
  auto error() const -> const std::string& { return error_; }

  auto ColumnInt(int index) const -> int64_t;
  auto ColumnText(int index) const
      -> std::string_view;  // valid until Step/Reset
  auto ColumnIsNull(int index) const -> bool;

 private:
  sqlite3_stmt* stmt_ = nullptr;
  std::string error_;
};

}  // namespace code_browser

#endif  // CODE_BROWSER_SQLITE_UTIL_H_
