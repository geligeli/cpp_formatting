#ifndef CPP_FORMATTING_PROTO_INDEX_LIB_H_
#define CPP_FORMATTING_PROTO_INDEX_LIB_H_

// The protobuf producer of the symbol index (see index.proto): describes one
// `.proto` file as an IndexUnit -- its messages, fields, enums, oneofs,
// services and rpcs as `Language.PROTO` symbols with `proto:<full.name>` USRs,
// their definitions and every reference to a type -- and, given the metadata
// protoc writes next to a generated file (`--cpp_out=annotate_headers`), the
// `GENERATES` anchors that let the merge link the generated C++ to it.
//
// Clang-free: libprotobuf's compiler front end (the parser protoc itself
// uses) and cpp_index_merge only.  It knows nothing about C++ symbols; an
// anchor is a byte range of a generated file.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cpp_formatting/index.pb.h"
#include "google/protobuf/compiler/importer.h"

/// A generated file's anchors: what the generator said about which of its
/// bytes came from which descriptor.
struct ProtoAnchors {
  /// A serialized `google.protobuf.GeneratedCodeInfo` (a `.pb.h.meta`).
  std::string Metadata;
  /// The path the index records for the generated file -- the one the other
  /// language's producer opens it under (`bazel-out/.../foo.pb.h`).
  std::string GeneratedPath;
};

struct ProtoIndexOptions {
  /// (import name, the path the index records for it).  Under Bazel an import
  /// name resolves to a `_virtual_imports` symlink; the index wants the source
  /// it points at.
  std::vector<std::pair<std::string, std::string>> PathMap;
  /// Asked for an import name the map does not list (a file found through a
  /// search directory); empty, or unset, leaves the import name.
  std::function<std::string(const std::string& ImportName)> ResolvePath;
  std::vector<ProtoAnchors> Anchors;
};

/// Indexes the file \p Tree knows as \p ImportName.  Occurrences are recorded
/// for that file only; a symbol of an imported file that it references is in
/// the symbol table with its canonical location, as the C++ producer does for
/// a header it does not own.  The unit is normalized.  Returns false, with
/// the parser's diagnostics in \p Error, when the file does not compile.
auto indexProtoFile(google::protobuf::compiler::SourceTree& Tree,
                    const std::string& ImportName,
                    const ProtoIndexOptions& Options, cpp_index::IndexUnit& Out,
                    std::string& Error) -> bool;

/// `cpp_format --emit-proto-index=<unit> [--path-map=<file>]
/// [--proto-path=<dir>]... [--anchors=<metadata>=<generated path>]...
/// <file.proto>`: \p Args is everything after the program name.  The path map
/// has one `import name<TAB>path` per line and is both how imports are found
/// and what the index calls them; `--proto-path` (`-I`) adds a directory to
/// search, for use by hand.  Exit code: 0, 1 when the file does not compile or
/// the unit cannot be written, 2 on a usage error.
auto runEmitProtoIndex(const std::vector<std::string>& Args) -> int;

#endif  // CPP_FORMATTING_PROTO_INDEX_LIB_H_
