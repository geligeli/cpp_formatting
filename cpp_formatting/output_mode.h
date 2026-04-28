#ifndef CPP_FORMATTING_OUTPUT_MODE_H_
#define CPP_FORMATTING_OUTPUT_MODE_H_

enum class OutputMode {
  DryRun,   ///< Print rewritten source to stdout.
  InPlace,  ///< Overwrite the input file on disk.
};

#endif  // CPP_FORMATTING_OUTPUT_MODE_H_
