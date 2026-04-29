#ifndef CPP_FORMATTING_EMBEDDED_CLANG_RESOURCE_H_
#define CPP_FORMATTING_EMBEDDED_CLANG_RESOURCE_H_

#include <string>

// Extracts the embedded clang_include_headers.tar.gz to a per-user cache
// directory keyed by the content hash, and returns its absolute path suitable
// for use as Clang's -resource-dir.  Subsequent calls skip extraction if the
// cache is already populated.  Returns an empty string on failure.
auto ensureClangResourceDir() -> std::string;

#endif  // CPP_FORMATTING_EMBEDDED_CLANG_RESOURCE_H_
