// Converts a binary file into a C++ translation unit that embeds its bytes as
// a const array.  Replaces the non-portable `xxd -i` invocation in the
// clang_include_headers_embed_cc genrule so the build works on any platform.
//
// Usage: embed_file <input> <output> <symbol>
//
// The output defines:
//   extern "C" const unsigned char <symbol>[];
//   extern "C" const size_t <symbol>Size;

#include <cstddef>
#include <cstdio>
#include <fstream>

namespace {

constexpr size_t kBytesPerLine = 12;

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <input> <output> <symbol>\n", argv[0]);
    return 1;
  }
  const char* symbol = argv[3];

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "embed_file: cannot open input '%s'\n", argv[1]);
    return 1;
  }
  std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "embed_file: cannot open output '%s'\n", argv[2]);
    return 1;
  }

  out << "#include <cstddef>\n\nextern \"C\" const unsigned char " << symbol
      << "[] = {";

  char buf[4096];
  size_t count = 0;
  while (in) {
    in.read(buf, sizeof(buf));
    for (std::streamsize i = 0; i < in.gcount(); ++i) {
      if (count % kBytesPerLine == 0) out << "\n    ";
      char hex[8];
      std::snprintf(hex, sizeof(hex), "0x%02x,",
                    static_cast<unsigned char>(buf[i]));
      out << hex;
      ++count;
    }
  }

  out << "\n};\nextern \"C\" const size_t " << symbol << "Size = sizeof("
      << symbol << ");\n";
  if (!out) {
    std::fprintf(stderr, "embed_file: failed writing output '%s'\n", argv[2]);
    return 1;
  }
  return 0;
}
