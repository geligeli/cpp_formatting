// apply_patch: a strict `git apply` for the integration tests, so they need no
// git on the machine (a remote-execution worker has none).  Applies a unified
// diff to the files it names, relative to the working directory:
//
//   apply_patch PATCH
//
// Stricter than git, never looser, so a patch this accepts is one git accepts:
// every hunk must match at exactly the line its header names (git also tries
// other offsets), the line counts in each `@@` header must be right, both paths
// carry git's `a/` / `b/` prefixes and name the same file, and nothing is
// written unless every hunk of every file applies.  No creation, deletion or
// rename -- `--format=diff` emits none.  Exits 1 with PATCH:LINE: reason.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// A file as lines, each keeping its "\n" -- the last one may lack it, which is
// what "\ No newline at end of file" talks about.
std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = nl == std::string::npos ? text.size() : nl + 1;
    lines.push_back(text.substr(start, end - start));
    start = end;
  }
  return lines;
}

struct Failure {
  size_t line;  // 1-based line of the patch
  std::string message;
};

// "a/src/x.cpp" -> "src/x.cpp", or "" if the path is not one git would apply.
std::string strip_prefix(const std::string& path, const char* prefix) {
  if (path.rfind(prefix, 0) != 0) return "";
  std::string rest = path.substr(2);
  if (rest.empty() || rest[0] == '/') return "";
  std::istringstream parts(rest);
  for (std::string part; std::getline(parts, part, '/');)
    if (part == "..") return "";
  return rest;
}

// "@@ -12,3 +12,4 @@ ..." -> {12, 3, 12, 4}; a count left out is 1.
bool parse_hunk_header(const std::string& line, long range[4]) {
  const char* p = line.c_str();
  if (line.rfind("@@ -", 0) != 0) return false;
  p += 4;
  for (int side = 0; side < 2; ++side) {
    char* end = nullptr;
    range[2 * side] = std::strtol(p, &end, 10);
    if (end == p) return false;
    p = end;
    range[2 * side + 1] = 1;
    if (*p == ',') {
      ++p;
      range[2 * side + 1] = std::strtol(p, &end, 10);
      if (end == p) return false;
      p = end;
    }
    if (side == 0) {
      if (p[0] != ' ' || p[1] != '+') return false;
      p += 2;
    }
  }
  return p[0] == ' ' && p[1] == '@' && p[2] == '@';
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), {});
  return true;
}

// Applies every file section of `patch` to `files` (path -> lines, loaded on
// first mention).  Throws Failure.
void apply_patch(const std::vector<std::string>& patch,
                 std::map<std::string, std::vector<std::string>>& files) {
  size_t i = 0;
  int sections = 0;
  while (i < patch.size()) {
    // git skips anything before a header ("diff --git ...", commentary).
    if (patch[i].rfind("--- ", 0) != 0) {
      ++i;
      continue;
    }
    const size_t header = i + 1;
    if (i + 1 >= patch.size() || patch[i + 1].rfind("+++ ", 0) != 0)
      throw Failure{header, "'---' without a following '+++'"};
    auto path_of = [](const std::string& l) {
      std::string p = l.substr(4);
      if (!p.empty() && p.back() == '\n') p.pop_back();
      size_t tab = p.find('\t');  // a timestamp after the name
      return tab == std::string::npos ? p : p.substr(0, tab);
    };
    const std::string old_path = strip_prefix(path_of(patch[i]), "a/");
    const std::string new_path = strip_prefix(path_of(patch[i + 1]), "b/");
    if (old_path.empty() || new_path.empty())
      throw Failure{header, "paths must be relative and start with a/ and b/"};
    if (old_path != new_path)
      throw Failure{header, "a/ and b/ name different files: " + old_path +
                                " vs " + new_path};
    i += 2;
    ++sections;

    if (!files.count(old_path)) {
      std::string text;
      if (!read_file(old_path, text))
        throw Failure{header, "cannot read " + old_path};
      files[old_path] = split_lines(text);
    }
    std::vector<std::string>& lines = files[old_path];
    std::vector<std::string> result;
    size_t copied = 0;  // lines of the old file already in `result`
    long delta = 0;     // new line number minus old, so far
    int hunks = 0;

    while (i < patch.size() && patch[i].rfind("@@ ", 0) == 0) {
      const size_t at = i + 1;
      long r[4];
      if (!parse_hunk_header(patch[i], r) || r[0] < 0 || r[1] < 0 || r[2] < 0 ||
          r[3] < 0)
        throw Failure{at, "malformed hunk header"};
      ++i;
      ++hunks;
      // A range of 0 lines names the line *after* which it applies.
      const long old_first = r[1] == 0 ? r[0] : r[0] - 1;
      if (old_first < static_cast<long>(copied))
        throw Failure{at, "hunk out of order or overlapping the previous one"};
      if ((r[3] == 0 ? r[2] : r[2] - 1) != old_first + delta)
        throw Failure{at, "new-file line number does not follow"};

      std::vector<std::string> old_side, new_side;
      long old_left = r[1], new_left = r[3];
      char last = 0;
      while (i < patch.size() &&
             (old_left > 0 || new_left > 0 || patch[i].rfind("\\ ", 0) == 0)) {
        const std::string& l = patch[i];
        if (l.rfind("\\ ", 0) == 0) {  // no newline after the previous line
          auto chop = [](std::vector<std::string>& v) {
            if (!v.empty() && !v.back().empty() && v.back().back() == '\n')
              v.back().pop_back();
          };
          if (last == ' ' || last == '-') chop(old_side);
          if (last == ' ' || last == '+') chop(new_side);
          if (!last) throw Failure{i + 1, "'\\' marker with no line before"};
          last = 0;
          ++i;
          continue;
        }
        const char kind = l.empty() ? 0 : l[0];
        std::string text = l.substr(1);
        if (text.empty() || text.back() != '\n') text += '\n';
        if (kind == ' ') {
          old_side.push_back(text);
          new_side.push_back(text);
          --old_left;
          --new_left;
        } else if (kind == '-') {
          old_side.push_back(text);
          --old_left;
        } else if (kind == '+') {
          new_side.push_back(text);
          --new_left;
        } else {
          throw Failure{i + 1, "hunk ends before its header's line counts"};
        }
        if (old_left < 0 || new_left < 0)
          throw Failure{i + 1, "hunk is longer than its header says"};
        last = kind;
        ++i;
      }
      if (old_left > 0 || new_left > 0)
        throw Failure{at, "patch ends inside a hunk"};

      if (old_first + static_cast<long>(old_side.size()) >
          static_cast<long>(lines.size()))
        throw Failure{at, "hunk runs past the end of " + old_path};
      for (size_t k = 0; k < old_side.size(); ++k) {
        if (lines[old_first + k] != old_side[k]) {
          throw Failure{at, old_path + ":" + std::to_string(old_first + k + 1) +
                                " does not match the patch"};
        }
      }
      result.insert(result.end(), lines.begin() + copied,
                    lines.begin() + old_first);
      result.insert(result.end(), new_side.begin(), new_side.end());
      copied = old_first + old_side.size();
      delta += static_cast<long>(new_side.size()) -
               static_cast<long>(old_side.size());
    }
    if (hunks == 0) throw Failure{header, "file header with no hunks"};
    result.insert(result.end(), lines.begin() + copied, lines.end());
    lines = std::move(result);
  }
  if (sections == 0) throw Failure{0, "no patch in input"};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: apply_patch PATCH\n");
    return 1;
  }
  std::string text;
  if (!read_file(argv[1], text)) {
    std::fprintf(stderr, "apply_patch: cannot read %s\n", argv[1]);
    return 1;
  }
  std::map<std::string, std::vector<std::string>> files;
  try {
    apply_patch(split_lines(text), files);
  } catch (const Failure& f) {
    std::fprintf(stderr, "apply_patch: %s:%zu: %s\n", argv[1], f.line,
                 f.message.c_str());
    return 1;
  }
  for (const auto& [path, lines] : files) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const std::string& l : lines) out << l;
    if (!out) {
      std::fprintf(stderr, "apply_patch: cannot write %s\n", path.c_str());
      return 1;
    }
  }
  return 0;
}
