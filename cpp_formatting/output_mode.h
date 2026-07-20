#ifndef CPP_FORMATTING_OUTPUT_MODE_H_
#define CPP_FORMATTING_OUTPUT_MODE_H_

enum class OutputMode {
  DryRun,   ///< Print rewritten source to stdout.
  InPlace,  ///< Overwrite the input file on disk.
  Debug,    ///< Print, per TU, every rename target and every reference site
            ///< found in the AST (with file:line:col and main-file flag).
            ///< Makes no modifications to source files.
  Lint,     ///< Buffer rewrites like DryRun and record one lint diagnostic
            ///< per rewrite; never writes to disk or prints source.
  Emit,     ///< Emit structured edit records (+ a dependent-token resolution
            ///< sidecar) for later cross-TU aggregation; never writes to disk.
  ///< Template-dependent tokens are NOT applied here — their edits are
  ///< produced by the aggregation phase from the sidecar.
};

#endif  // CPP_FORMATTING_OUTPUT_MODE_H_
