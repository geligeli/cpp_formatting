#ifndef CPP_FORMATTING_OUTPUT_MODE_H_
#define CPP_FORMATTING_OUTPUT_MODE_H_

enum class OutputMode {
  DryRun,   ///< Print rewritten source to stdout.
  InPlace,  ///< Overwrite the input file on disk.
  Debug,    ///< Print, per TU, every rename target and every reference site
            ///< found in the AST (with file:line:col and main-file flag).
            ///< Makes no modifications to source files.
};

#endif  // CPP_FORMATTING_OUTPUT_MODE_H_
