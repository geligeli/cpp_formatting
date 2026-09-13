#include "cpp_formatting/rename_state.h"

void recordResolution(DependentResolutions& DepRes,
                      const std::pair<std::string, unsigned>& Key,
                      const std::string& NewName, llvm::StringRef OldName,
                      unsigned Length, std::pair<std::string, unsigned> Owner) {
  DependentResolution& R = DepRes[Key];
  if (R.Vetoed) return;
  if (R.HasName && R.NewName != NewName) {
    R.Vetoed = true;  // instantiations disagree — leave the token alone.
    return;
  }
  R.NewName = NewName;
  R.HasName = true;
  R.OldName = OldName.str();
  R.Length = Length;
  R.OwnerFile = std::move(Owner.first);
  R.OwnerOffset = Owner.second;
}

void vetoResolution(DependentResolutions& DepRes,
                    const std::pair<std::string, unsigned>& Key) {
  DepRes[Key].Vetoed = true;
}

void mergeDependentResolutions(DependentResolutions& Into,
                               const DependentResolutions& From) {
  for (const auto& [Key, R] : From) {
    if (R.Vetoed) {
      vetoResolution(Into, Key);
    } else if (R.HasName) {
      recordResolution(Into, Key, R.NewName, R.OldName, R.Length,
                       {R.OwnerFile, R.OwnerOffset});
    }
  }
}

auto dependentResolutionsDifferFor(const DependentResolutions& A,
                                   const DependentResolutions& B,
                                   llvm::StringRef File) -> bool {
  const std::pair<std::string, unsigned> First{File.str(), 0};
  auto ItA = A.lower_bound(First);
  auto ItB = B.lower_bound(First);
  while (true) {
    const bool EndA = ItA == A.end() || ItA->first.first != File;
    const bool EndB = ItB == B.end() || ItB->first.first != File;
    if (EndA && EndB) return false;
    if (EndA != EndB) return true;
    if (ItA->first.second != ItB->first.second) return true;
    const DependentResolution& RA = ItA->second;
    const DependentResolution& RB = ItB->second;
    if (RA.Vetoed != RB.Vetoed || RA.HasName != RB.HasName) return true;
    if (RA.HasName && !RA.Vetoed && RA.NewName != RB.NewName) return true;
    ++ItA;
    ++ItB;
  }
}
