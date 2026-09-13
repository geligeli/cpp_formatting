#include "cpp_formatting/rename_state.h"

#include <gtest/gtest.h>

namespace {

using Key = std::pair<std::string, unsigned>;

auto named(const DependentResolutions& M, const Key& K) -> std::string {
  auto It = M.find(K);
  if (It == M.end()) return "<absent>";
  if (It->second.Vetoed) return "<vetoed>";
  if (!It->second.HasName) return "<empty>";
  return It->second.NewName;
}

TEST(DependentResolutions, RecordThenMergeIsIdempotent) {
  DependentResolutions A;
  recordResolution(A, {"h", 10}, "val_", "val", 3, {"h", 1});
  DependentResolutions Into;
  mergeDependentResolutions(Into, A);
  mergeDependentResolutions(Into, A);
  EXPECT_EQ(named(Into, {"h", 10}), "val_");
  EXPECT_EQ(Into.size(), 1u);
  EXPECT_FALSE(dependentResolutionsDifferFor(Into, A, "h"));
}

TEST(DependentResolutions, MergeIsCommutativeOnResolvedState) {
  DependentResolutions A, B;
  recordResolution(A, {"h", 10}, "val_", "val", 3, {"h", 1});
  recordResolution(B, {"h", 20}, "x_", "x", 1, {"h", 2});
  DependentResolutions AB, BA;
  mergeDependentResolutions(AB, A);
  mergeDependentResolutions(AB, B);
  mergeDependentResolutions(BA, B);
  mergeDependentResolutions(BA, A);
  EXPECT_FALSE(dependentResolutionsDifferFor(AB, BA, "h"));
  EXPECT_EQ(named(AB, {"h", 10}), "val_");
  EXPECT_EQ(named(AB, {"h", 20}), "x_");
}

TEST(DependentResolutions, DisagreementVetoes) {
  DependentResolutions A, B, Into;
  recordResolution(A, {"h", 10}, "val_", "val", 3, {"h", 1});
  recordResolution(B, {"h", 10}, "m_val", "val", 3, {"h", 2});
  mergeDependentResolutions(Into, A);
  mergeDependentResolutions(Into, B);
  EXPECT_EQ(named(Into, {"h", 10}), "<vetoed>");
}

TEST(DependentResolutions, VetoIsAbsorbing) {
  DependentResolutions Vetoed, Named, Into;
  vetoResolution(Vetoed, {"h", 10});
  recordResolution(Named, {"h", 10}, "val_", "val", 3, {"h", 1});
  mergeDependentResolutions(Into, Vetoed);
  mergeDependentResolutions(Into, Named);
  EXPECT_EQ(named(Into, {"h", 10}), "<vetoed>");
  // Recording directly after a veto is ignored too.
  recordResolution(Into, {"h", 10}, "val_", "val", 3, {"h", 1});
  EXPECT_EQ(named(Into, {"h", 10}), "<vetoed>");
}

TEST(DependentResolutions, DifferForIsScopedToOneFile) {
  DependentResolutions A, B;
  recordResolution(A, {"a.h", 10}, "val_", "val", 3, {"a.h", 1});
  recordResolution(B, {"a.h", 10}, "val_", "val", 3, {"a.h", 1});
  recordResolution(B, {"b.h", 10}, "other_", "other", 5, {"b.h", 1});
  EXPECT_FALSE(dependentResolutionsDifferFor(A, B, "a.h"));
  EXPECT_TRUE(dependentResolutionsDifferFor(A, B, "b.h"));
  EXPECT_FALSE(dependentResolutionsDifferFor(A, B, "c.h"));
}

TEST(DependentResolutions, DifferForSeesEveryKindOfChange) {
  DependentResolutions Seed;
  recordResolution(Seed, {"h", 10}, "val_", "val", 3, {"h", 1});

  DependentResolutions Renamed = Seed;
  Renamed[{"h", 10}].NewName = "m_val";
  EXPECT_TRUE(dependentResolutionsDifferFor(Seed, Renamed, "h"));

  DependentResolutions Vetoed = Seed;
  vetoResolution(Vetoed, {"h", 10});
  EXPECT_TRUE(dependentResolutionsDifferFor(Seed, Vetoed, "h"));

  DependentResolutions Extra = Seed;
  recordResolution(Extra, {"h", 30}, "z_", "z", 1, {"h", 2});
  EXPECT_TRUE(dependentResolutionsDifferFor(Seed, Extra, "h"));
  EXPECT_TRUE(dependentResolutionsDifferFor(Extra, Seed, "h"));

  // A different owner is informational only: not a difference.
  DependentResolutions Owner = Seed;
  Owner[{"h", 10}].OwnerOffset = 99;
  EXPECT_FALSE(dependentResolutionsDifferFor(Seed, Owner, "h"));
}

}  // namespace
