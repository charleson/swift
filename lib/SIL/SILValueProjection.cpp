//===--- SILValueProjection.cpp -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-value-projection"
#include "swift/SIL/SILValueProjection.h"
#include "swift/SIL/InstructionUtils.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                              Utility Functions
//===----------------------------------------------------------------------===//

static inline void removeLSLocations(LSLocationValueMap &Values,
                                     LSLocationList &FirstLevel) {
  for (auto &X : FirstLevel)
    Values.erase(X);
}

//===----------------------------------------------------------------------===//
//                              SILValue Projection
//===----------------------------------------------------------------------===//

void SILValueProjection::print(SILModule *Mod) {
  llvm::outs() << Base;
  Path.getValue().print(llvm::outs(), *Mod);
}

SILValue SILValueProjection::createExtract(SILValue Base,
                                           const Optional<ProjectionPath> &Path,
                                           SILInstruction *Inst,
                                           bool IsValExt) {
  // If we found a projection path, but there are no projections, then the two
  // loads must be the same, return PrevLI.
  if (!Path || Path->empty())
    return Base;

  // Ok, at this point we know that we can construct our aggregate projections
  // from our list of address projections.
  SILValue LastExtract = Base;
  SILBuilder Builder(Inst);
  Builder.setCurrentDebugScope(Inst->getFunction()->getDebugScope());

  // We use an auto-generated SILLocation for now.
  // TODO: make the sil location more precise.
  SILLocation Loc = RegularLocation::getAutoGeneratedLocation();

  // Construct the path!
  for (auto PI = Path->begin(), PE = Path->end(); PI != PE; ++PI) {
    if (IsValExt) {
      LastExtract =
          PI->createObjectProjection(Builder, Loc, LastExtract).get();
      continue;
    }
    LastExtract =
        PI->createAddressProjection(Builder, Loc, LastExtract).get();
  }

  // Return the last extract we created.
  return LastExtract;
}

//===----------------------------------------------------------------------===//
//                              Load Store Value
//===----------------------------------------------------------------------===//

void LSValue::expand(SILValue Base, SILModule *M, LSValueList &Vals,
                     TypeExpansionAnalysis *TE) {
  // To expand a LSValue to its indivisible parts, we first get the
  // address projection paths from the accessed type to each indivisible field,
  // i.e. leaf nodes, then we append these projection paths to the Base.
  for (const auto &P : TE->getTypeExpansion((*Base).getType(), M, TEKind::TELeaf)) {
    Vals.push_back(LSValue(Base, P.getValue()));
  }
}

SILValue LSValue::reduce(LSLocation &Base, SILModule *M,
                         LSLocationValueMap &Values,
                         SILInstruction *InsertPt,
                         TypeExpansionAnalysis *TE) {
  // Walk bottom up the projection tree, try to reason about how to construct
  // a single SILValue out of all the available values for all the memory
  // locations.
  //
  // First, get a list of all the leaf nodes and intermediate nodes for the
  // Base memory location.
  LSLocationList ALocs;
  const ProjectionPath &BasePath = Base.getPath().getValue();
  for (const auto &P : TE->getTypeExpansion(Base.getType(M), M, TEKind::TENode)) {
    ALocs.push_back(LSLocation(Base.getBase(), BasePath, P.getValue()));
  }

  // Second, go from leaf nodes to their parents. This guarantees that at the
  // point the parent is processed, its children have been processed already.
  for (auto I = ALocs.rbegin(), E = ALocs.rend(); I != E; ++I) {
    // This is a leaf node, we have a value for it.
    //
    // Reached the end of the projection tree, this is a leaf node.
    LSLocationList FirstLevel;
    I->getFirstLevelLSLocations(FirstLevel, M);
    if (FirstLevel.empty())
      continue;

    // If this is a class reference type, we have reached end of the type tree.
    if (I->getType(M).getClassOrBoundGenericClass())
      continue;

    // This is NOT a leaf node, we need to construct a value for it.

    // There is only 1 children node and its value's projection path is not
    // empty, keep stripping it.
    auto Iter = FirstLevel.begin();
    LSValue &FirstVal = Values[*Iter];
    if (FirstLevel.size() == 1 && !FirstVal.hasEmptyProjectionPath()) {
      Values[*I] = FirstVal.stripLastLevelProjection();
      // We have a value for the parent, remove all the values for children.
      removeLSLocations(Values, FirstLevel);
      continue;
    }
    
    // If there are more than 1 children and all the children nodes have
    // LSValues with the same base and non-empty projection path. we can get
    // away by not extracting value for every single field.
    //
    // Simply create a new node with all the aggregated base value, i.e.
    // stripping off the last level projection.
    bool HasIdenticalValueBase = true;
    SILValue FirstBase = FirstVal.getBase();
    Iter = std::next(Iter);
    for (auto EndIter = FirstLevel.end(); Iter != EndIter; ++Iter) {
      LSValue &V = Values[*Iter];
      HasIdenticalValueBase &= (FirstBase == V.getBase());
    }

    if (FirstLevel.size() > 1 && HasIdenticalValueBase && 
        !FirstVal.hasEmptyProjectionPath()) {
      Values[*I] = FirstVal.stripLastLevelProjection();
      // We have a value for the parent, remove all the values for children.
      removeLSLocations(Values, FirstLevel);
      continue;
    }

    // In 3 cases do we need aggregation.
    //
    // 1. If there is only 1 child and we cannot strip off any projections,
    // that means we need to create an aggregation.
    // 
    // 2. There are multiple children and they have the same base, but empty
    // projection paths.
    //
    // 3. Children have values from different bases, We need to create
    // extractions and aggregation in this case.
    //
    llvm::SmallVector<SILValue, 8> Vals;
    for (auto &X : FirstLevel) {
      Vals.push_back(Values[X].materialize(InsertPt));
    }
    SILBuilder Builder(InsertPt);
    Builder.setCurrentDebugScope(InsertPt->getFunction()->getDebugScope());
    
    // We use an auto-generated SILLocation for now.
    // TODO: make the sil location more precise.
    NullablePtr<swift::SILInstruction> AI =
        Projection::createAggFromFirstLevelProjections(
            Builder, RegularLocation::getAutoGeneratedLocation(),
            I->getType(M).getObjectType(),
            Vals);
    // This is the Value for the current node.
    ProjectionPath P(Base.getType(M));
    Values[*I] = LSValue(SILValue(AI.get()), P);
    removeLSLocations(Values, FirstLevel);

    // Keep iterating until we have reach the top-most level of the projection
    // tree.
    // i.e. the memory location represented by the Base.
  }

  assert(Values.size() == 1 && "Should have a single location this point");

  // Finally materialize and return the forwarding SILValue.
  return Values.begin()->second.materialize(InsertPt);
}

//===----------------------------------------------------------------------===//
//                                  Memory Location
//===----------------------------------------------------------------------===//

bool LSLocation::isMustAliasLSLocation(const LSLocation &RHS,
                                       AliasAnalysis *AA) {
  // If the bases are not must-alias, the locations may not alias.
  if (!AA->isMustAlias(Base, RHS.getBase()))
    return false;
  // If projection paths are different, then the locations cannot alias.
  if (!hasIdenticalProjectionPath(RHS))
    return false;
  return true;
}

bool LSLocation::isMayAliasLSLocation(const LSLocation &RHS,
                                      AliasAnalysis *AA) {
  // If the bases do not alias, then the locations cannot alias.
  if (AA->isNoAlias(Base, RHS.getBase()))
    return false;
  // If one projection path is a prefix of another, then the locations
  // could alias.
  if (hasNonEmptySymmetricPathDifference(RHS))
    return false;
  return true;
}


bool LSLocation::isNonEscapingLocalLSLocation(SILFunction *Fn,
                                              EscapeAnalysis *EA) {
  // An alloc_stack is definitely dead at the end of the function.
  if (isa<AllocStackInst>(Base))
    return true;
  // For other allocations we ask escape analysis.		
  auto *ConGraph = EA->getConnectionGraph(Fn);
  if (isa<AllocationInst>(Base)) {
    auto *Node = ConGraph->getNodeOrNull(Base, EA);
    if (Node && !Node->escapes()) {
      return true;
    }
  }
  return false;
}

void LSLocation::getFirstLevelLSLocations(LSLocationList &Locs,
                                          SILModule *Mod) {
  SILType Ty = getType(Mod);
  llvm::SmallVector<Projection, 8> Out;
  Projection::getFirstLevelProjections(Ty, *Mod, Out);
  for (auto &X : Out) {
    ProjectionPath P((*Base).getType());
    P.append(Path.getValue());
    P.append(X);
    Locs.push_back(LSLocation(Base, P));
  }
}

void LSLocation::expand(LSLocation Base, SILModule *M, LSLocationList &Locs,
                        TypeExpansionAnalysis *TE) {
  // To expand a memory location to its indivisible parts, we first get the
  // address projection paths from the accessed type to each indivisible field,
  // i.e. leaf nodes, then we append these projection paths to the Base.
  //
  // Construct the LSLocation by appending the projection path from the
  // accessed node to the leaf nodes.
  const ProjectionPath &BasePath = Base.getPath().getValue();
  for (const auto &P : TE->getTypeExpansion(Base.getType(M), M, TEKind::TELeaf)) {
    Locs.push_back(LSLocation(Base.getBase(), BasePath, P.getValue()));
  }
}

void LSLocation::reduce(LSLocation Base, SILModule *M, LSLocationSet &Locs,
                        TypeExpansionAnalysis *TE) {
  // First, construct the LSLocation by appending the projection path from the
  // accessed node to the leaf nodes.
  LSLocationList Nodes;
  const ProjectionPath &BasePath = Base.getPath().getValue();
  for (const auto &P : TE->getTypeExpansion(Base.getType(M), M, TEKind::TENode)) {
    Nodes.push_back(LSLocation(Base.getBase(), BasePath, P.getValue()));
  }

  // Second, go from leaf nodes to their parents. This guarantees that at the
  // point the parent is processed, its children have been processed already.
  for (auto I = Nodes.rbegin(), E = Nodes.rend(); I != E; ++I) {
    LSLocationList FirstLevel;
    I->getFirstLevelLSLocations(FirstLevel, M);
    // Reached the end of the projection tree, this is a leaf node.
    if (FirstLevel.empty())
      continue;

    // If this is a class reference type, we have reached end of the type tree.
    if (I->getType(M).getClassOrBoundGenericClass())
      continue;

    // This is NOT a leaf node, check whether all its first level children are
    // alive.
    bool Alive = true;
    for (auto &X : FirstLevel) {
      Alive &= Locs.find(X) != Locs.end();
    }

    // All first level locations are alive, create the new aggregated location.
    if (Alive) {
      for (auto &X : FirstLevel)
        Locs.erase(X);
      Locs.insert(*I);
    }
  }
}

void LSLocation::enumerateLSLocation(SILModule *M, SILValue Mem,
                                     std::vector<LSLocation> &Locations,
                                     LSLocationIndexMap &IndexMap,
                                     LSLocationBaseMap &BaseMap,
                                     TypeExpansionAnalysis *TypeCache) {
  // We have processed this SILValue before.
  if (BaseMap.find(Mem) != BaseMap.end())
    return;

  // Construct a Location to represent the memory written by this instruction.
  SILValue UO = getUnderlyingObject(Mem);
  LSLocation L(UO, ProjectionPath::getProjectionPath(UO, Mem));

  // If we cant figure out the Base or Projection Path for the memory location,
  // simply ignore it for now.
  if (!L.isValid())
    return;

  // Record the SILValue to location mapping.
  BaseMap[Mem] = L; 

  // Expand the given Mem into individual fields and add them to the
  // locationvault.
  LSLocationList Locs;
  LSLocation::expand(L, M, Locs, TypeCache);
  for (auto &Loc : Locs) {
   if (IndexMap.find(Loc) != IndexMap.end())
     continue;
   IndexMap[Loc] = Locations.size();
   Locations.push_back(Loc);
  }

}

void
LSLocation::
enumerateLSLocations(SILFunction &F,
                     std::vector<LSLocation> &Locations,
                     LSLocationIndexMap &IndexMap,
                     LSLocationBaseMap &BaseMap,
                     TypeExpansionAnalysis *TypeCache,
                     std::pair<int, int> &LSCount) {
  // Enumerate all locations accessed by the loads or stores.
  for (auto &B : F) {
    for (auto &I : B) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        enumerateLSLocation(&I.getModule(), LI->getOperand(), Locations,
                            IndexMap, BaseMap, TypeCache);
        ++LSCount.first;
        continue;
      }
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        enumerateLSLocation(&I.getModule(), SI->getDest(), Locations,
                            IndexMap, BaseMap, TypeCache);
        ++LSCount.second;
        continue;
      }
    }
  }
}