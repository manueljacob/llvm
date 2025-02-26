//===- lib/Linker/IRMover.cpp ---------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Linker/IRMover.h"
#include "LinkDiagnosticInfo.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/GVMaterializer.h"
#include "llvm/IR/TypeFinder.h"
#include "llvm/Transforms/Utils/Cloning.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
// TypeMap implementation.
//===----------------------------------------------------------------------===//

namespace {
class TypeMapTy : public ValueMapTypeRemapper {
  /// This is a mapping from a source type to a destination type to use.
  DenseMap<Type *, Type *> MappedTypes;

  /// When checking to see if two subgraphs are isomorphic, we speculatively
  /// add types to MappedTypes, but keep track of them here in case we need to
  /// roll back.
  SmallVector<Type *, 16> SpeculativeTypes;

  SmallVector<StructType *, 16> SpeculativeDstOpaqueTypes;

  /// This is a list of non-opaque structs in the source module that are mapped
  /// to an opaque struct in the destination module.
  SmallVector<StructType *, 16> SrcDefinitionsToResolve;

  /// This is the set of opaque types in the destination modules who are
  /// getting a body from the source module.
  SmallPtrSet<StructType *, 16> DstResolvedOpaqueTypes;

public:
  TypeMapTy(IRMover::IdentifiedStructTypeSet &DstStructTypesSet)
      : DstStructTypesSet(DstStructTypesSet) {}

  IRMover::IdentifiedStructTypeSet &DstStructTypesSet;
  /// Indicate that the specified type in the destination module is conceptually
  /// equivalent to the specified type in the source module.
  void addTypeMapping(Type *DstTy, Type *SrcTy);

  /// Produce a body for an opaque type in the dest module from a type
  /// definition in the source module.
  void linkDefinedTypeBodies();

  /// Return the mapped type to use for the specified input type from the
  /// source module.
  Type *get(Type *SrcTy);
  Type *get(Type *SrcTy, SmallPtrSet<StructType *, 8> &Visited);

  void finishType(StructType *DTy, StructType *STy, ArrayRef<Type *> ETypes);

  FunctionType *get(FunctionType *T) {
    return cast<FunctionType>(get((Type *)T));
  }

private:
  Type *remapType(Type *SrcTy) override { return get(SrcTy); }

  bool areTypesIsomorphic(Type *DstTy, Type *SrcTy);
};
}

void TypeMapTy::addTypeMapping(Type *DstTy, Type *SrcTy) {
  assert(SpeculativeTypes.empty());
  assert(SpeculativeDstOpaqueTypes.empty());

  // Check to see if these types are recursively isomorphic and establish a
  // mapping between them if so.
  if (!areTypesIsomorphic(DstTy, SrcTy)) {
    // Oops, they aren't isomorphic.  Just discard this request by rolling out
    // any speculative mappings we've established.
    for (Type *Ty : SpeculativeTypes)
      MappedTypes.erase(Ty);

    SrcDefinitionsToResolve.resize(SrcDefinitionsToResolve.size() -
                                   SpeculativeDstOpaqueTypes.size());
    for (StructType *Ty : SpeculativeDstOpaqueTypes)
      DstResolvedOpaqueTypes.erase(Ty);
  } else {
    for (Type *Ty : SpeculativeTypes)
      if (auto *STy = dyn_cast<StructType>(Ty))
        if (STy->hasName())
          STy->setName("");
  }
  SpeculativeTypes.clear();
  SpeculativeDstOpaqueTypes.clear();
}

/// Recursively walk this pair of types, returning true if they are isomorphic,
/// false if they are not.
bool TypeMapTy::areTypesIsomorphic(Type *DstTy, Type *SrcTy) {
  // Two types with differing kinds are clearly not isomorphic.
  if (DstTy->getTypeID() != SrcTy->getTypeID())
    return false;

  // If we have an entry in the MappedTypes table, then we have our answer.
  Type *&Entry = MappedTypes[SrcTy];
  if (Entry)
    return Entry == DstTy;

  // Two identical types are clearly isomorphic.  Remember this
  // non-speculatively.
  if (DstTy == SrcTy) {
    Entry = DstTy;
    return true;
  }

  // Okay, we have two types with identical kinds that we haven't seen before.

  // If this is an opaque struct type, special case it.
  if (StructType *SSTy = dyn_cast<StructType>(SrcTy)) {
    // Mapping an opaque type to any struct, just keep the dest struct.
    if (SSTy->isOpaque()) {
      Entry = DstTy;
      SpeculativeTypes.push_back(SrcTy);
      return true;
    }

    // Mapping a non-opaque source type to an opaque dest.  If this is the first
    // type that we're mapping onto this destination type then we succeed.  Keep
    // the dest, but fill it in later. If this is the second (different) type
    // that we're trying to map onto the same opaque type then we fail.
    if (cast<StructType>(DstTy)->isOpaque()) {
      // We can only map one source type onto the opaque destination type.
      if (!DstResolvedOpaqueTypes.insert(cast<StructType>(DstTy)).second)
        return false;
      SrcDefinitionsToResolve.push_back(SSTy);
      SpeculativeTypes.push_back(SrcTy);
      SpeculativeDstOpaqueTypes.push_back(cast<StructType>(DstTy));
      Entry = DstTy;
      return true;
    }
  }

  // If the number of subtypes disagree between the two types, then we fail.
  if (SrcTy->getNumContainedTypes() != DstTy->getNumContainedTypes())
    return false;

  // Fail if any of the extra properties (e.g. array size) of the type disagree.
  if (isa<IntegerType>(DstTy))
    return false; // bitwidth disagrees.
  if (PointerType *PT = dyn_cast<PointerType>(DstTy)) {
    if (PT->getAddressSpace() != cast<PointerType>(SrcTy)->getAddressSpace())
      return false;

  } else if (FunctionType *FT = dyn_cast<FunctionType>(DstTy)) {
    if (FT->isVarArg() != cast<FunctionType>(SrcTy)->isVarArg())
      return false;
  } else if (StructType *DSTy = dyn_cast<StructType>(DstTy)) {
    StructType *SSTy = cast<StructType>(SrcTy);
    if (DSTy->isLiteral() != SSTy->isLiteral() ||
        DSTy->isPacked() != SSTy->isPacked())
      return false;
  } else if (ArrayType *DATy = dyn_cast<ArrayType>(DstTy)) {
    if (DATy->getNumElements() != cast<ArrayType>(SrcTy)->getNumElements())
      return false;
  } else if (VectorType *DVTy = dyn_cast<VectorType>(DstTy)) {
    if (DVTy->getNumElements() != cast<VectorType>(SrcTy)->getNumElements())
      return false;
  }

  // Otherwise, we speculate that these two types will line up and recursively
  // check the subelements.
  Entry = DstTy;
  SpeculativeTypes.push_back(SrcTy);

  for (unsigned I = 0, E = SrcTy->getNumContainedTypes(); I != E; ++I)
    if (!areTypesIsomorphic(DstTy->getContainedType(I),
                            SrcTy->getContainedType(I)))
      return false;

  // If everything seems to have lined up, then everything is great.
  return true;
}

void TypeMapTy::linkDefinedTypeBodies() {
  SmallVector<Type *, 16> Elements;
  for (StructType *SrcSTy : SrcDefinitionsToResolve) {
    StructType *DstSTy = cast<StructType>(MappedTypes[SrcSTy]);
    assert(DstSTy->isOpaque());

    // Map the body of the source type over to a new body for the dest type.
    Elements.resize(SrcSTy->getNumElements());
    for (unsigned I = 0, E = Elements.size(); I != E; ++I)
      Elements[I] = get(SrcSTy->getElementType(I));

    DstSTy->setBody(Elements, SrcSTy->isPacked());
    DstStructTypesSet.switchToNonOpaque(DstSTy);
  }
  SrcDefinitionsToResolve.clear();
  DstResolvedOpaqueTypes.clear();
}

void TypeMapTy::finishType(StructType *DTy, StructType *STy,
                           ArrayRef<Type *> ETypes) {
  DTy->setBody(ETypes, STy->isPacked());

  // Steal STy's name.
  if (STy->hasName()) {
    SmallString<16> TmpName = STy->getName();
    STy->setName("");
    DTy->setName(TmpName);
  }

  DstStructTypesSet.addNonOpaque(DTy);
}

Type *TypeMapTy::get(Type *Ty) {
  SmallPtrSet<StructType *, 8> Visited;
  return get(Ty, Visited);
}

Type *TypeMapTy::get(Type *Ty, SmallPtrSet<StructType *, 8> &Visited) {
  // If we already have an entry for this type, return it.
  Type **Entry = &MappedTypes[Ty];
  if (*Entry)
    return *Entry;

  // These are types that LLVM itself will unique.
  bool IsUniqued = !isa<StructType>(Ty) || cast<StructType>(Ty)->isLiteral();

#ifndef NDEBUG
  if (!IsUniqued) {
    for (auto &Pair : MappedTypes) {
      assert(!(Pair.first != Ty && Pair.second == Ty) &&
             "mapping to a source type");
    }
  }
#endif

  if (!IsUniqued && !Visited.insert(cast<StructType>(Ty)).second) {
    StructType *DTy = StructType::create(Ty->getContext());
    return *Entry = DTy;
  }

  // If this is not a recursive type, then just map all of the elements and
  // then rebuild the type from inside out.
  SmallVector<Type *, 4> ElementTypes;

  // If there are no element types to map, then the type is itself.  This is
  // true for the anonymous {} struct, things like 'float', integers, etc.
  if (Ty->getNumContainedTypes() == 0 && IsUniqued)
    return *Entry = Ty;

  // Remap all of the elements, keeping track of whether any of them change.
  bool AnyChange = false;
  ElementTypes.resize(Ty->getNumContainedTypes());
  for (unsigned I = 0, E = Ty->getNumContainedTypes(); I != E; ++I) {
    ElementTypes[I] = get(Ty->getContainedType(I), Visited);
    AnyChange |= ElementTypes[I] != Ty->getContainedType(I);
  }

  // If we found our type while recursively processing stuff, just use it.
  Entry = &MappedTypes[Ty];
  if (*Entry) {
    if (auto *DTy = dyn_cast<StructType>(*Entry)) {
      if (DTy->isOpaque()) {
        auto *STy = cast<StructType>(Ty);
        finishType(DTy, STy, ElementTypes);
      }
    }
    return *Entry;
  }

  // If all of the element types mapped directly over and the type is not
  // a nomed struct, then the type is usable as-is.
  if (!AnyChange && IsUniqued)
    return *Entry = Ty;

  // Otherwise, rebuild a modified type.
  switch (Ty->getTypeID()) {
  default:
    llvm_unreachable("unknown derived type to remap");
  case Type::ArrayTyID:
    return *Entry = ArrayType::get(ElementTypes[0],
                                   cast<ArrayType>(Ty)->getNumElements());
  case Type::VectorTyID:
    return *Entry = VectorType::get(ElementTypes[0],
                                    cast<VectorType>(Ty)->getNumElements());
  case Type::PointerTyID:
    return *Entry = PointerType::get(ElementTypes[0],
                                     cast<PointerType>(Ty)->getAddressSpace());
  case Type::FunctionTyID:
    return *Entry = FunctionType::get(ElementTypes[0],
                                      makeArrayRef(ElementTypes).slice(1),
                                      cast<FunctionType>(Ty)->isVarArg());
  case Type::StructTyID: {
    auto *STy = cast<StructType>(Ty);
    bool IsPacked = STy->isPacked();
    if (IsUniqued)
      return *Entry = StructType::get(Ty->getContext(), ElementTypes, IsPacked);

    // If the type is opaque, we can just use it directly.
    if (STy->isOpaque()) {
      DstStructTypesSet.addOpaque(STy);
      return *Entry = Ty;
    }

    if (StructType *OldT =
            DstStructTypesSet.findNonOpaque(ElementTypes, IsPacked)) {
      STy->setName("");
      return *Entry = OldT;
    }

    if (!AnyChange) {
      DstStructTypesSet.addNonOpaque(STy);
      return *Entry = Ty;
    }

    StructType *DTy = StructType::create(Ty->getContext());
    finishType(DTy, STy, ElementTypes);
    return *Entry = DTy;
  }
  }
}

LinkDiagnosticInfo::LinkDiagnosticInfo(DiagnosticSeverity Severity,
                                       const Twine &Msg)
    : DiagnosticInfo(DK_Linker, Severity), Msg(Msg) {}
void LinkDiagnosticInfo::print(DiagnosticPrinter &DP) const { DP << Msg; }

//===----------------------------------------------------------------------===//
// IRLinker implementation.
//===----------------------------------------------------------------------===//

namespace {
class IRLinker;

/// Creates prototypes for functions that are lazily linked on the fly. This
/// speeds up linking for modules with many/ lazily linked functions of which
/// few get used.
class GlobalValueMaterializer final : public ValueMaterializer {
  IRLinker *TheIRLinker;

public:
  GlobalValueMaterializer(IRLinker *TheIRLinker) : TheIRLinker(TheIRLinker) {}
  Value *materializeDeclFor(Value *V) override;
  void materializeInitFor(GlobalValue *New, GlobalValue *Old) override;
  Metadata *mapTemporaryMetadata(Metadata *MD) override;
  void replaceTemporaryMetadata(const Metadata *OrigMD,
                                Metadata *NewMD) override;
  bool isMetadataNeeded(Metadata *MD) override;
};

class LocalValueMaterializer final : public ValueMaterializer {
  IRLinker *TheIRLinker;

public:
  LocalValueMaterializer(IRLinker *TheIRLinker) : TheIRLinker(TheIRLinker) {}
  Value *materializeDeclFor(Value *V) override;
  void materializeInitFor(GlobalValue *New, GlobalValue *Old) override;
  Metadata *mapTemporaryMetadata(Metadata *MD) override;
  void replaceTemporaryMetadata(const Metadata *OrigMD,
                                Metadata *NewMD) override;
  bool isMetadataNeeded(Metadata *MD) override;
};

/// This is responsible for keeping track of the state used for moving data
/// from SrcM to DstM.
class IRLinker {
  Module &DstM;
  Module &SrcM;

  std::function<void(GlobalValue &, IRMover::ValueAdder)> AddLazyFor;

  TypeMapTy TypeMap;
  GlobalValueMaterializer GValMaterializer;
  LocalValueMaterializer LValMaterializer;

  /// Mapping of values from what they used to be in Src, to what they are now
  /// in DstM.  ValueToValueMapTy is a ValueMap, which involves some overhead
  /// due to the use of Value handles which the Linker doesn't actually need,
  /// but this allows us to reuse the ValueMapper code.
  ValueToValueMapTy ValueMap;
  ValueToValueMapTy AliasValueMap;

  DenseSet<GlobalValue *> ValuesToLink;
  std::vector<GlobalValue *> Worklist;

  void maybeAdd(GlobalValue *GV) {
    if (ValuesToLink.insert(GV).second)
      Worklist.push_back(GV);
  }

  /// Set to true when all global value body linking is complete (including
  /// lazy linking). Used to prevent metadata linking from creating new
  /// references.
  bool DoneLinkingBodies = false;

  bool HasError = false;

  /// Flag indicating that we are just linking metadata (after function
  /// importing).
  bool IsMetadataLinkingPostpass;

  /// Flags to pass to value mapper invocations.
  RemapFlags ValueMapperFlags = RF_MoveDistinctMDs;

  /// Association between metadata values created during bitcode parsing and
  /// the value id. Used to correlate temporary metadata created during
  /// function importing with the final metadata parsed during the subsequent
  /// metadata linking postpass.
  DenseMap<const Metadata *, unsigned> MetadataToIDs;

  /// Association between metadata value id and temporary metadata that
  /// remains unmapped after function importing. Saved during function
  /// importing and consumed during the metadata linking postpass.
  DenseMap<unsigned, MDNode *> *ValIDToTempMDMap;

  /// Set of subprogram metadata that does not need to be linked into the
  /// destination module, because the functions were not imported directly
  /// or via an inlined body in an imported function.
  SmallPtrSet<const Metadata *, 16> UnneededSubprograms;

  /// Handles cloning of a global values from the source module into
  /// the destination module, including setting the attributes and visibility.
  GlobalValue *copyGlobalValueProto(const GlobalValue *SGV, bool ForDefinition);

  /// Helper method for setting a message and returning an error code.
  bool emitError(const Twine &Message) {
    SrcM.getContext().diagnose(LinkDiagnosticInfo(DS_Error, Message));
    HasError = true;
    return true;
  }

  void emitWarning(const Twine &Message) {
    SrcM.getContext().diagnose(LinkDiagnosticInfo(DS_Warning, Message));
  }

  /// Check whether we should be linking metadata from the source module.
  bool shouldLinkMetadata() {
    // ValIDToTempMDMap will be non-null when we are importing or otherwise want
    // to link metadata lazily, and then when linking the metadata.
    // We only want to return true for the former case.
    return ValIDToTempMDMap == nullptr || IsMetadataLinkingPostpass;
  }

  /// Given a global in the source module, return the global in the
  /// destination module that is being linked to, if any.
  GlobalValue *getLinkedToGlobal(const GlobalValue *SrcGV) {
    // If the source has no name it can't link.  If it has local linkage,
    // there is no name match-up going on.
    if (!SrcGV->hasName() || SrcGV->hasLocalLinkage())
      return nullptr;

    // Otherwise see if we have a match in the destination module's symtab.
    GlobalValue *DGV = DstM.getNamedValue(SrcGV->getName());
    if (!DGV)
      return nullptr;

    // If we found a global with the same name in the dest module, but it has
    // internal linkage, we are really not doing any linkage here.
    if (DGV->hasLocalLinkage())
      return nullptr;

    // Otherwise, we do in fact link to the destination global.
    return DGV;
  }

  void computeTypeMapping();

  Constant *linkAppendingVarProto(GlobalVariable *DstGV,
                                  const GlobalVariable *SrcGV);

  bool shouldLink(GlobalValue *DGV, GlobalValue &SGV);
  Constant *linkGlobalValueProto(GlobalValue *GV, bool ForAlias);

  bool linkModuleFlagsMetadata();

  void linkGlobalInit(GlobalVariable &Dst, GlobalVariable &Src);
  bool linkFunctionBody(Function &Dst, Function &Src);
  void linkAliasBody(GlobalAlias &Dst, GlobalAlias &Src);
  bool linkGlobalValueBody(GlobalValue &Dst, GlobalValue &Src);

  /// Functions that take care of cloning a specific global value type
  /// into the destination module.
  GlobalVariable *copyGlobalVariableProto(const GlobalVariable *SGVar);
  Function *copyFunctionProto(const Function *SF);
  GlobalValue *copyGlobalAliasProto(const GlobalAlias *SGA);

  void linkNamedMDNodes();

  /// Populate the UnneededSubprograms set with the DISubprogram metadata
  /// from the source module that we don't need to link into the dest module,
  /// because the functions were not imported directly or via an inlined body
  /// in an imported function.
  void findNeededSubprograms();

  /// The value mapper leaves nulls in the list of subprograms for any
  /// in the UnneededSubprograms map. Strip those out after metadata linking.
  void stripNullSubprograms();

public:
  IRLinker(Module &DstM, IRMover::IdentifiedStructTypeSet &Set, Module &SrcM,
           ArrayRef<GlobalValue *> ValuesToLink,
           std::function<void(GlobalValue &, IRMover::ValueAdder)> AddLazyFor,
           DenseMap<unsigned, MDNode *> *ValIDToTempMDMap = nullptr,
           bool IsMetadataLinkingPostpass = false)
      : DstM(DstM), SrcM(SrcM), AddLazyFor(AddLazyFor), TypeMap(Set),
        GValMaterializer(this), LValMaterializer(this),
        IsMetadataLinkingPostpass(IsMetadataLinkingPostpass),
        ValIDToTempMDMap(ValIDToTempMDMap) {
    for (GlobalValue *GV : ValuesToLink)
      maybeAdd(GV);

    // If appropriate, tell the value mapper that it can expect to see
    // temporary metadata.
    if (!shouldLinkMetadata())
      ValueMapperFlags = ValueMapperFlags | RF_HaveUnmaterializedMetadata;
  }

  ~IRLinker() {
    // In the case where we are not linking metadata, we unset the CanReplace
    // flag on all temporary metadata in the MetadataToIDs map to ensure
    // none was replaced while being a map key. Now that we are destructing
    // the map, set the flag back to true, so that it is replaceable during
    // metadata linking.
    if (!shouldLinkMetadata()) {
      for (auto MDI : MetadataToIDs) {
        Metadata *MD = const_cast<Metadata *>(MDI.first);
        MDNode *Node = dyn_cast<MDNode>(MD);
        assert((Node && Node->isTemporary()) &&
               "Found non-temp metadata in map when not linking metadata");
        Node->setCanReplace(true);
      }
    }
  }

  bool run();
  Value *materializeDeclFor(Value *V, bool ForAlias);
  void materializeInitFor(GlobalValue *New, GlobalValue *Old, bool ForAlias);

  /// Save the mapping between the given temporary metadata and its metadata
  /// value id. Used to support metadata linking as a postpass for function
  /// importing.
  Metadata *mapTemporaryMetadata(Metadata *MD);

  /// Replace any temporary metadata saved for the source metadata's id with
  /// the new non-temporary metadata. Used when metadata linking as a postpass
  /// for function importing.
  void replaceTemporaryMetadata(const Metadata *OrigMD, Metadata *NewMD);

  /// Indicates whether we need to map the given metadata into the destination
  /// module. Used to prevent linking of metadata only needed by functions not
  /// linked into the dest module.
  bool isMetadataNeeded(Metadata *MD);
};
}

/// The LLVM SymbolTable class autorenames globals that conflict in the symbol
/// table. This is good for all clients except for us. Go through the trouble
/// to force this back.
static void forceRenaming(GlobalValue *GV, StringRef Name) {
  // If the global doesn't force its name or if it already has the right name,
  // there is nothing for us to do.
  if (GV->hasLocalLinkage() || GV->getName() == Name)
    return;

  Module *M = GV->getParent();

  // If there is a conflict, rename the conflict.
  if (GlobalValue *ConflictGV = M->getNamedValue(Name)) {
    GV->takeName(ConflictGV);
    ConflictGV->setName(Name); // This will cause ConflictGV to get renamed
    assert(ConflictGV->getName() != Name && "forceRenaming didn't work");
  } else {
    GV->setName(Name); // Force the name back
  }
}

Value *GlobalValueMaterializer::materializeDeclFor(Value *V) {
  return TheIRLinker->materializeDeclFor(V, false);
}

void GlobalValueMaterializer::materializeInitFor(GlobalValue *New,
                                                 GlobalValue *Old) {
  TheIRLinker->materializeInitFor(New, Old, false);
}

Metadata *GlobalValueMaterializer::mapTemporaryMetadata(Metadata *MD) {
  return TheIRLinker->mapTemporaryMetadata(MD);
}

void GlobalValueMaterializer::replaceTemporaryMetadata(const Metadata *OrigMD,
                                                       Metadata *NewMD) {
  TheIRLinker->replaceTemporaryMetadata(OrigMD, NewMD);
}

bool GlobalValueMaterializer::isMetadataNeeded(Metadata *MD) {
  return TheIRLinker->isMetadataNeeded(MD);
}

Value *LocalValueMaterializer::materializeDeclFor(Value *V) {
  return TheIRLinker->materializeDeclFor(V, true);
}

void LocalValueMaterializer::materializeInitFor(GlobalValue *New,
                                                GlobalValue *Old) {
  TheIRLinker->materializeInitFor(New, Old, true);
}

Metadata *LocalValueMaterializer::mapTemporaryMetadata(Metadata *MD) {
  return TheIRLinker->mapTemporaryMetadata(MD);
}

void LocalValueMaterializer::replaceTemporaryMetadata(const Metadata *OrigMD,
                                                      Metadata *NewMD) {
  TheIRLinker->replaceTemporaryMetadata(OrigMD, NewMD);
}

bool LocalValueMaterializer::isMetadataNeeded(Metadata *MD) {
  return TheIRLinker->isMetadataNeeded(MD);
}

Value *IRLinker::materializeDeclFor(Value *V, bool ForAlias) {
  auto *SGV = dyn_cast<GlobalValue>(V);
  if (!SGV)
    return nullptr;

  return linkGlobalValueProto(SGV, ForAlias);
}

void IRLinker::materializeInitFor(GlobalValue *New, GlobalValue *Old,
                                  bool ForAlias) {
  // If we already created the body, just return.
  if (auto *F = dyn_cast<Function>(New)) {
    if (!F->isDeclaration())
      return;
  } else if (auto *V = dyn_cast<GlobalVariable>(New)) {
    if (V->hasInitializer())
      return;
  } else {
    auto *A = cast<GlobalAlias>(New);
    if (A->getAliasee())
      return;
  }

  if (ForAlias || shouldLink(New, *Old))
    linkGlobalValueBody(*New, *Old);
}

Metadata *IRLinker::mapTemporaryMetadata(Metadata *MD) {
  if (!ValIDToTempMDMap)
    return nullptr;
  // If this temporary metadata has a value id recorded during function
  // parsing, record that in the ValIDToTempMDMap if one was provided.
  if (MetadataToIDs.count(MD)) {
    unsigned Idx = MetadataToIDs[MD];
    // Check if we created a temp MD when importing a different function from
    // this module. If so, reuse it the same temporary metadata, otherwise
    // add this temporary metadata to the map.
    if (!ValIDToTempMDMap->count(Idx)) {
      MDNode *Node = cast<MDNode>(MD);
      assert(Node->isTemporary());
      (*ValIDToTempMDMap)[Idx] = Node;
    }
    return (*ValIDToTempMDMap)[Idx];
  }
  return nullptr;
}

void IRLinker::replaceTemporaryMetadata(const Metadata *OrigMD,
                                        Metadata *NewMD) {
  if (!ValIDToTempMDMap)
    return;
#ifndef NDEBUG
  auto *N = dyn_cast_or_null<MDNode>(NewMD);
  assert(!N || !N->isTemporary());
#endif
  // If a mapping between metadata value ids and temporary metadata
  // created during function importing was provided, and the source
  // metadata has a value id recorded during metadata parsing, replace
  // the temporary metadata with the final mapped metadata now.
  if (MetadataToIDs.count(OrigMD)) {
    unsigned Idx = MetadataToIDs[OrigMD];
    // Nothing to do if we didn't need to create a temporary metadata during
    // function importing.
    if (!ValIDToTempMDMap->count(Idx))
      return;
    MDNode *TempMD = (*ValIDToTempMDMap)[Idx];
    TempMD->replaceAllUsesWith(NewMD);
    MDNode::deleteTemporary(TempMD);
    ValIDToTempMDMap->erase(Idx);
  }
}

bool IRLinker::isMetadataNeeded(Metadata *MD) {
  // Currently only DISubprogram metadata is marked as being unneeded.
  if (UnneededSubprograms.empty())
    return true;
  MDNode *Node = dyn_cast<MDNode>(MD);
  if (!Node)
    return true;
  DISubprogram *SP = getDISubprogram(Node);
  if (!SP)
    return true;
  return !UnneededSubprograms.count(SP);
}

/// Loop through the global variables in the src module and merge them into the
/// dest module.
GlobalVariable *IRLinker::copyGlobalVariableProto(const GlobalVariable *SGVar) {
  // No linking to be performed or linking from the source: simply create an
  // identical version of the symbol over in the dest module... the
  // initializer will be filled in later by LinkGlobalInits.
  GlobalVariable *NewDGV =
      new GlobalVariable(DstM, TypeMap.get(SGVar->getValueType()),
                         SGVar->isConstant(), GlobalValue::ExternalLinkage,
                         /*init*/ nullptr, SGVar->getName(),
                         /*insertbefore*/ nullptr, SGVar->getThreadLocalMode(),
                         SGVar->getType()->getAddressSpace());
  NewDGV->setAlignment(SGVar->getAlignment());
  return NewDGV;
}

/// Link the function in the source module into the destination module if
/// needed, setting up mapping information.
Function *IRLinker::copyFunctionProto(const Function *SF) {
  // If there is no linkage to be performed or we are linking from the source,
  // bring SF over.
  return Function::Create(TypeMap.get(SF->getFunctionType()),
                          GlobalValue::ExternalLinkage, SF->getName(), &DstM);
}

/// Set up prototypes for any aliases that come over from the source module.
GlobalValue *IRLinker::copyGlobalAliasProto(const GlobalAlias *SGA) {
  // If there is no linkage to be performed or we're linking from the source,
  // bring over SGA.
  auto *Ty = TypeMap.get(SGA->getValueType());
  return GlobalAlias::create(Ty, SGA->getType()->getPointerAddressSpace(),
                             GlobalValue::ExternalLinkage, SGA->getName(),
                             &DstM);
}

GlobalValue *IRLinker::copyGlobalValueProto(const GlobalValue *SGV,
                                            bool ForDefinition) {
  GlobalValue *NewGV;
  if (auto *SGVar = dyn_cast<GlobalVariable>(SGV)) {
    NewGV = copyGlobalVariableProto(SGVar);
  } else if (auto *SF = dyn_cast<Function>(SGV)) {
    NewGV = copyFunctionProto(SF);
  } else {
    if (ForDefinition)
      NewGV = copyGlobalAliasProto(cast<GlobalAlias>(SGV));
    else
      NewGV = new GlobalVariable(
          DstM, TypeMap.get(SGV->getValueType()),
          /*isConstant*/ false, GlobalValue::ExternalLinkage,
          /*init*/ nullptr, SGV->getName(),
          /*insertbefore*/ nullptr, SGV->getThreadLocalMode(),
          SGV->getType()->getAddressSpace());
  }

  if (ForDefinition)
    NewGV->setLinkage(SGV->getLinkage());
  else if (SGV->hasExternalWeakLinkage() || SGV->hasWeakLinkage() ||
           SGV->hasLinkOnceLinkage())
    NewGV->setLinkage(GlobalValue::ExternalWeakLinkage);

  NewGV->copyAttributesFrom(SGV);

  // Remove these copied constants in case this stays a declaration, since
  // they point to the source module. If the def is linked the values will
  // be mapped in during linkFunctionBody.
  if (auto *NewF = dyn_cast<Function>(NewGV)) {
    NewF->setPersonalityFn(nullptr);
    NewF->setPrefixData(nullptr);
    NewF->setPrologueData(nullptr);
  }

  return NewGV;
}

/// Loop over all of the linked values to compute type mappings.  For example,
/// if we link "extern Foo *x" and "Foo *x = NULL", then we have two struct
/// types 'Foo' but one got renamed when the module was loaded into the same
/// LLVMContext.
void IRLinker::computeTypeMapping() {
  for (GlobalValue &SGV : SrcM.globals()) {
    GlobalValue *DGV = getLinkedToGlobal(&SGV);
    if (!DGV)
      continue;

    if (!DGV->hasAppendingLinkage() || !SGV.hasAppendingLinkage()) {
      TypeMap.addTypeMapping(DGV->getType(), SGV.getType());
      continue;
    }

    // Unify the element type of appending arrays.
    ArrayType *DAT = cast<ArrayType>(DGV->getValueType());
    ArrayType *SAT = cast<ArrayType>(SGV.getValueType());
    TypeMap.addTypeMapping(DAT->getElementType(), SAT->getElementType());
  }

  for (GlobalValue &SGV : SrcM)
    if (GlobalValue *DGV = getLinkedToGlobal(&SGV))
      TypeMap.addTypeMapping(DGV->getType(), SGV.getType());

  for (GlobalValue &SGV : SrcM.aliases())
    if (GlobalValue *DGV = getLinkedToGlobal(&SGV))
      TypeMap.addTypeMapping(DGV->getType(), SGV.getType());

  // Incorporate types by name, scanning all the types in the source module.
  // At this point, the destination module may have a type "%foo = { i32 }" for
  // example.  When the source module got loaded into the same LLVMContext, if
  // it had the same type, it would have been renamed to "%foo.42 = { i32 }".
  std::vector<StructType *> Types = SrcM.getIdentifiedStructTypes();
  for (StructType *ST : Types) {
    if (!ST->hasName())
      continue;

    // Check to see if there is a dot in the name followed by a digit.
    size_t DotPos = ST->getName().rfind('.');
    if (DotPos == 0 || DotPos == StringRef::npos ||
        ST->getName().back() == '.' ||
        !isdigit(static_cast<unsigned char>(ST->getName()[DotPos + 1])))
      continue;

    // Check to see if the destination module has a struct with the prefix name.
    StructType *DST = DstM.getTypeByName(ST->getName().substr(0, DotPos));
    if (!DST)
      continue;

    // Don't use it if this actually came from the source module. They're in
    // the same LLVMContext after all. Also don't use it unless the type is
    // actually used in the destination module. This can happen in situations
    // like this:
    //
    //      Module A                         Module B
    //      --------                         --------
    //   %Z = type { %A }                %B = type { %C.1 }
    //   %A = type { %B.1, [7 x i8] }    %C.1 = type { i8* }
    //   %B.1 = type { %C }              %A.2 = type { %B.3, [5 x i8] }
    //   %C = type { i8* }               %B.3 = type { %C.1 }
    //
    // When we link Module B with Module A, the '%B' in Module B is
    // used. However, that would then use '%C.1'. But when we process '%C.1',
    // we prefer to take the '%C' version. So we are then left with both
    // '%C.1' and '%C' being used for the same types. This leads to some
    // variables using one type and some using the other.
    if (TypeMap.DstStructTypesSet.hasType(DST))
      TypeMap.addTypeMapping(DST, ST);
  }

  // Now that we have discovered all of the type equivalences, get a body for
  // any 'opaque' types in the dest module that are now resolved.
  TypeMap.linkDefinedTypeBodies();
}

static void getArrayElements(const Constant *C,
                             SmallVectorImpl<Constant *> &Dest) {
  unsigned NumElements = cast<ArrayType>(C->getType())->getNumElements();

  for (unsigned i = 0; i != NumElements; ++i)
    Dest.push_back(C->getAggregateElement(i));
}

/// If there were any appending global variables, link them together now.
/// Return true on error.
Constant *IRLinker::linkAppendingVarProto(GlobalVariable *DstGV,
                                          const GlobalVariable *SrcGV) {
  Type *EltTy = cast<ArrayType>(TypeMap.get(SrcGV->getValueType()))
                    ->getElementType();

  StringRef Name = SrcGV->getName();
  bool IsNewStructor = false;
  bool IsOldStructor = false;
  if (Name == "llvm.global_ctors" || Name == "llvm.global_dtors") {
    if (cast<StructType>(EltTy)->getNumElements() == 3)
      IsNewStructor = true;
    else
      IsOldStructor = true;
  }

  PointerType *VoidPtrTy = Type::getInt8Ty(SrcGV->getContext())->getPointerTo();
  if (IsOldStructor) {
    auto &ST = *cast<StructType>(EltTy);
    Type *Tys[3] = {ST.getElementType(0), ST.getElementType(1), VoidPtrTy};
    EltTy = StructType::get(SrcGV->getContext(), Tys, false);
  }

  if (DstGV) {
    ArrayType *DstTy = cast<ArrayType>(DstGV->getValueType());

    if (!SrcGV->hasAppendingLinkage() || !DstGV->hasAppendingLinkage()) {
      emitError(
          "Linking globals named '" + SrcGV->getName() +
          "': can only link appending global with another appending global!");
      return nullptr;
    }

    // Check to see that they two arrays agree on type.
    if (EltTy != DstTy->getElementType()) {
      emitError("Appending variables with different element types!");
      return nullptr;
    }
    if (DstGV->isConstant() != SrcGV->isConstant()) {
      emitError("Appending variables linked with different const'ness!");
      return nullptr;
    }

    if (DstGV->getAlignment() != SrcGV->getAlignment()) {
      emitError(
          "Appending variables with different alignment need to be linked!");
      return nullptr;
    }

    if (DstGV->getVisibility() != SrcGV->getVisibility()) {
      emitError(
          "Appending variables with different visibility need to be linked!");
      return nullptr;
    }

    if (DstGV->hasUnnamedAddr() != SrcGV->hasUnnamedAddr()) {
      emitError(
          "Appending variables with different unnamed_addr need to be linked!");
      return nullptr;
    }

    if (StringRef(DstGV->getSection()) != SrcGV->getSection()) {
      emitError(
          "Appending variables with different section name need to be linked!");
      return nullptr;
    }
  }

  SmallVector<Constant *, 16> DstElements;
  if (DstGV)
    getArrayElements(DstGV->getInitializer(), DstElements);

  SmallVector<Constant *, 16> SrcElements;
  getArrayElements(SrcGV->getInitializer(), SrcElements);

  if (IsNewStructor)
    SrcElements.erase(
        std::remove_if(SrcElements.begin(), SrcElements.end(),
                       [this](Constant *E) {
                         auto *Key = dyn_cast<GlobalValue>(
                             E->getAggregateElement(2)->stripPointerCasts());
                         if (!Key)
                           return false;
                         GlobalValue *DGV = getLinkedToGlobal(Key);
                         return !shouldLink(DGV, *Key);
                       }),
        SrcElements.end());
  uint64_t NewSize = DstElements.size() + SrcElements.size();
  ArrayType *NewType = ArrayType::get(EltTy, NewSize);

  // Create the new global variable.
  GlobalVariable *NG = new GlobalVariable(
      DstM, NewType, SrcGV->isConstant(), SrcGV->getLinkage(),
      /*init*/ nullptr, /*name*/ "", DstGV, SrcGV->getThreadLocalMode(),
      SrcGV->getType()->getAddressSpace());

  NG->copyAttributesFrom(SrcGV);
  forceRenaming(NG, SrcGV->getName());

  Constant *Ret = ConstantExpr::getBitCast(NG, TypeMap.get(SrcGV->getType()));

  // Stop recursion.
  ValueMap[SrcGV] = Ret;

  for (auto *V : SrcElements) {
    Constant *NewV;
    if (IsOldStructor) {
      auto *S = cast<ConstantStruct>(V);
      auto *E1 = MapValue(S->getOperand(0), ValueMap, ValueMapperFlags,
                          &TypeMap, &GValMaterializer);
      auto *E2 = MapValue(S->getOperand(1), ValueMap, ValueMapperFlags,
                          &TypeMap, &GValMaterializer);
      Value *Null = Constant::getNullValue(VoidPtrTy);
      NewV =
          ConstantStruct::get(cast<StructType>(EltTy), E1, E2, Null, nullptr);
    } else {
      NewV =
          MapValue(V, ValueMap, ValueMapperFlags, &TypeMap, &GValMaterializer);
    }
    DstElements.push_back(NewV);
  }

  NG->setInitializer(ConstantArray::get(NewType, DstElements));

  // Replace any uses of the two global variables with uses of the new
  // global.
  if (DstGV) {
    DstGV->replaceAllUsesWith(ConstantExpr::getBitCast(NG, DstGV->getType()));
    DstGV->eraseFromParent();
  }

  return Ret;
}

bool IRLinker::shouldLink(GlobalValue *DGV, GlobalValue &SGV) {
  // Already imported all the values. Just map to the Dest value
  // in case it is referenced in the metadata.
  if (IsMetadataLinkingPostpass) {
    assert(!ValuesToLink.count(&SGV) &&
           "Source value unexpectedly requested for link during metadata link");
    return false;
  }

  if (ValuesToLink.count(&SGV))
    return true;

  if (SGV.hasLocalLinkage())
    return true;

  if (DGV && !DGV->isDeclarationForLinker())
    return false;

  if (SGV.hasAvailableExternallyLinkage())
    return true;

  if (DoneLinkingBodies)
    return false;

  AddLazyFor(SGV, [this](GlobalValue &GV) { maybeAdd(&GV); });
  return ValuesToLink.count(&SGV);
}

Constant *IRLinker::linkGlobalValueProto(GlobalValue *SGV, bool ForAlias) {
  GlobalValue *DGV = getLinkedToGlobal(SGV);

  bool ShouldLink = shouldLink(DGV, *SGV);

  // just missing from map
  if (ShouldLink) {
    auto I = ValueMap.find(SGV);
    if (I != ValueMap.end())
      return cast<Constant>(I->second);

    I = AliasValueMap.find(SGV);
    if (I != AliasValueMap.end())
      return cast<Constant>(I->second);
  }

  DGV = nullptr;
  if (ShouldLink || !ForAlias)
    DGV = getLinkedToGlobal(SGV);

  // Handle the ultra special appending linkage case first.
  assert(!DGV || SGV->hasAppendingLinkage() == DGV->hasAppendingLinkage());
  if (SGV->hasAppendingLinkage())
    return linkAppendingVarProto(cast_or_null<GlobalVariable>(DGV),
                                 cast<GlobalVariable>(SGV));

  GlobalValue *NewGV;
  if (DGV && !ShouldLink) {
    NewGV = DGV;
  } else {
    // If we are done linking global value bodies (i.e. we are performing
    // metadata linking), don't link in the global value due to this
    // reference, simply map it to null.
    if (DoneLinkingBodies)
      return nullptr;

    NewGV = copyGlobalValueProto(SGV, ShouldLink);
    if (ShouldLink || !ForAlias)
      forceRenaming(NewGV, SGV->getName());
  }
  if (ShouldLink || ForAlias) {
    if (const Comdat *SC = SGV->getComdat()) {
      if (auto *GO = dyn_cast<GlobalObject>(NewGV)) {
        Comdat *DC = DstM.getOrInsertComdat(SC->getName());
        DC->setSelectionKind(SC->getSelectionKind());
        GO->setComdat(DC);
      }
    }
  }

  if (!ShouldLink && ForAlias)
    NewGV->setLinkage(GlobalValue::InternalLinkage);

  Constant *C = NewGV;
  if (DGV)
    C = ConstantExpr::getBitCast(NewGV, TypeMap.get(SGV->getType()));

  if (DGV && NewGV != DGV) {
    DGV->replaceAllUsesWith(ConstantExpr::getBitCast(NewGV, DGV->getType()));
    DGV->eraseFromParent();
  }

  return C;
}

/// Update the initializers in the Dest module now that all globals that may be
/// referenced are in Dest.
void IRLinker::linkGlobalInit(GlobalVariable &Dst, GlobalVariable &Src) {
  // Figure out what the initializer looks like in the dest module.
  Dst.setInitializer(MapValue(Src.getInitializer(), ValueMap, ValueMapperFlags,
                              &TypeMap, &GValMaterializer));
}

/// Copy the source function over into the dest function and fix up references
/// to values. At this point we know that Dest is an external function, and
/// that Src is not.
bool IRLinker::linkFunctionBody(Function &Dst, Function &Src) {
  assert(Dst.isDeclaration() && !Src.isDeclaration());

  // Materialize if needed.
  if (std::error_code EC = Src.materialize())
    return emitError(EC.message());

  if (!shouldLinkMetadata())
    // This is only supported for lazy links. Do after materialization of
    // a function and before remapping metadata on instructions below
    // in RemapInstruction, as the saved mapping is used to handle
    // the temporary metadata hanging off instructions.
    SrcM.getMaterializer()->saveMetadataList(MetadataToIDs,
                                             /* OnlyTempMD = */ true);

  // Link in the prefix data.
  if (Src.hasPrefixData())
    Dst.setPrefixData(MapValue(Src.getPrefixData(), ValueMap, ValueMapperFlags,
                               &TypeMap, &GValMaterializer));

  // Link in the prologue data.
  if (Src.hasPrologueData())
    Dst.setPrologueData(MapValue(Src.getPrologueData(), ValueMap,
                                 ValueMapperFlags, &TypeMap,
                                 &GValMaterializer));

  // Link in the personality function.
  if (Src.hasPersonalityFn())
    Dst.setPersonalityFn(MapValue(Src.getPersonalityFn(), ValueMap,
                                  ValueMapperFlags, &TypeMap,
                                  &GValMaterializer));

  // Go through and convert function arguments over, remembering the mapping.
  Function::arg_iterator DI = Dst.arg_begin();
  for (Argument &Arg : Src.args()) {
    DI->setName(Arg.getName()); // Copy the name over.

    // Add a mapping to our mapping.
    ValueMap[&Arg] = &*DI;
    ++DI;
  }

  // Copy over the metadata attachments.
  SmallVector<std::pair<unsigned, MDNode *>, 8> MDs;
  Src.getAllMetadata(MDs);
  for (const auto &I : MDs)
    Dst.setMetadata(I.first, MapMetadata(I.second, ValueMap, ValueMapperFlags,
                                         &TypeMap, &GValMaterializer));

  // Splice the body of the source function into the dest function.
  Dst.getBasicBlockList().splice(Dst.end(), Src.getBasicBlockList());

  // At this point, all of the instructions and values of the function are now
  // copied over.  The only problem is that they are still referencing values in
  // the Source function as operands.  Loop through all of the operands of the
  // functions and patch them up to point to the local versions.
  for (BasicBlock &BB : Dst)
    for (Instruction &I : BB)
      RemapInstruction(&I, ValueMap, RF_IgnoreMissingEntries | ValueMapperFlags,
                       &TypeMap, &GValMaterializer);

  // There is no need to map the arguments anymore.
  for (Argument &Arg : Src.args())
    ValueMap.erase(&Arg);

  return false;
}

void IRLinker::linkAliasBody(GlobalAlias &Dst, GlobalAlias &Src) {
  Constant *Aliasee = Src.getAliasee();
  Constant *Val = MapValue(Aliasee, AliasValueMap, ValueMapperFlags, &TypeMap,
                           &LValMaterializer);
  Dst.setAliasee(Val);
}

bool IRLinker::linkGlobalValueBody(GlobalValue &Dst, GlobalValue &Src) {
  if (auto *F = dyn_cast<Function>(&Src))
    return linkFunctionBody(cast<Function>(Dst), *F);
  if (auto *GVar = dyn_cast<GlobalVariable>(&Src)) {
    linkGlobalInit(cast<GlobalVariable>(Dst), *GVar);
    return false;
  }
  linkAliasBody(cast<GlobalAlias>(Dst), cast<GlobalAlias>(Src));
  return false;
}

void IRLinker::findNeededSubprograms() {
  // Track unneeded nodes to make it simpler to handle the case
  // where we are checking if an already-mapped SP is needed.
  NamedMDNode *CompileUnits = SrcM.getNamedMetadata("llvm.dbg.cu");
  if (!CompileUnits)
    return;
  for (unsigned I = 0, E = CompileUnits->getNumOperands(); I != E; ++I) {
    auto *CU = cast<DICompileUnit>(CompileUnits->getOperand(I));
    assert(CU && "Expected valid compile unit");
    // Ensure that we don't remove subprograms referenced by DIImportedEntity.
    // It is not legal to have a DIImportedEntity with a null entity or scope.
    // FIXME: The DISubprogram for functions not linked in but kept due to
    // being referenced by a DIImportedEntity should also get their
    // IsDefinition flag is unset.
    SmallPtrSet<DISubprogram *, 8> ImportedEntitySPs;
    for (auto *IE : CU->getImportedEntities()) {
      if (auto *SP = dyn_cast<DISubprogram>(IE->getEntity()))
        ImportedEntitySPs.insert(SP);
      if (auto *SP = dyn_cast<DISubprogram>(IE->getScope()))
        ImportedEntitySPs.insert(SP);
    }
    for (auto *Op : CU->getSubprograms()) {
      // Unless we were doing function importing and deferred metadata linking,
      // any needed SPs should have been mapped as they would be reached
      // from the function linked in (either on the function itself for linked
      // function bodies, or from DILocation on inlined instructions).
      assert(!(ValueMap.MD()[Op] && IsMetadataLinkingPostpass) &&
             "DISubprogram shouldn't be mapped yet");
      if (!ValueMap.MD()[Op] && !ImportedEntitySPs.count(Op))
        UnneededSubprograms.insert(Op);
    }
  }
  if (!IsMetadataLinkingPostpass)
    return;
  // In the case of metadata linking as a postpass (e.g. for function
  // importing), see which DISubprogram MD from the source has an associated
  // temporary metadata node, which means the SP was needed by an imported
  // function.
  for (auto MDI : MetadataToIDs) {
    const MDNode *Node = dyn_cast<MDNode>(MDI.first);
    if (!Node)
      continue;
    DISubprogram *SP = getDISubprogram(Node);
    if (!SP || !ValIDToTempMDMap->count(MDI.second))
      continue;
    UnneededSubprograms.erase(SP);
  }
}

// Squash null subprograms from compile unit subprogram lists.
void IRLinker::stripNullSubprograms() {
  NamedMDNode *CompileUnits = DstM.getNamedMetadata("llvm.dbg.cu");
  if (!CompileUnits)
    return;
  for (unsigned I = 0, E = CompileUnits->getNumOperands(); I != E; ++I) {
    auto *CU = cast<DICompileUnit>(CompileUnits->getOperand(I));
    assert(CU && "Expected valid compile unit");

    SmallVector<Metadata *, 16> NewSPs;
    NewSPs.reserve(CU->getSubprograms().size());
    bool FoundNull = false;
    for (DISubprogram *SP : CU->getSubprograms()) {
      if (!SP) {
        FoundNull = true;
        continue;
      }
      NewSPs.push_back(SP);
    }
    if (FoundNull)
      CU->replaceSubprograms(MDTuple::get(CU->getContext(), NewSPs));
  }
}

/// Insert all of the named MDNodes in Src into the Dest module.
void IRLinker::linkNamedMDNodes() {
  findNeededSubprograms();
  const NamedMDNode *SrcModFlags = SrcM.getModuleFlagsMetadata();
  for (const NamedMDNode &NMD : SrcM.named_metadata()) {
    // Don't link module flags here. Do them separately.
    if (&NMD == SrcModFlags)
      continue;
    NamedMDNode *DestNMD = DstM.getOrInsertNamedMetadata(NMD.getName());
    // Add Src elements into Dest node.
    for (const MDNode *op : NMD.operands())
      DestNMD->addOperand(MapMetadata(
          op, ValueMap, ValueMapperFlags | RF_NullMapMissingGlobalValues,
          &TypeMap, &GValMaterializer));
  }
  stripNullSubprograms();
}

/// Merge the linker flags in Src into the Dest module.
bool IRLinker::linkModuleFlagsMetadata() {
  // If the source module has no module flags, we are done.
  const NamedMDNode *SrcModFlags = SrcM.getModuleFlagsMetadata();
  if (!SrcModFlags)
    return false;

  // If the destination module doesn't have module flags yet, then just copy
  // over the source module's flags.
  NamedMDNode *DstModFlags = DstM.getOrInsertModuleFlagsMetadata();
  if (DstModFlags->getNumOperands() == 0) {
    for (unsigned I = 0, E = SrcModFlags->getNumOperands(); I != E; ++I)
      DstModFlags->addOperand(SrcModFlags->getOperand(I));

    return false;
  }

  // First build a map of the existing module flags and requirements.
  DenseMap<MDString *, std::pair<MDNode *, unsigned>> Flags;
  SmallSetVector<MDNode *, 16> Requirements;
  for (unsigned I = 0, E = DstModFlags->getNumOperands(); I != E; ++I) {
    MDNode *Op = DstModFlags->getOperand(I);
    ConstantInt *Behavior = mdconst::extract<ConstantInt>(Op->getOperand(0));
    MDString *ID = cast<MDString>(Op->getOperand(1));

    if (Behavior->getZExtValue() == Module::Require) {
      Requirements.insert(cast<MDNode>(Op->getOperand(2)));
    } else {
      Flags[ID] = std::make_pair(Op, I);
    }
  }

  // Merge in the flags from the source module, and also collect its set of
  // requirements.
  for (unsigned I = 0, E = SrcModFlags->getNumOperands(); I != E; ++I) {
    MDNode *SrcOp = SrcModFlags->getOperand(I);
    ConstantInt *SrcBehavior =
        mdconst::extract<ConstantInt>(SrcOp->getOperand(0));
    MDString *ID = cast<MDString>(SrcOp->getOperand(1));
    MDNode *DstOp;
    unsigned DstIndex;
    std::tie(DstOp, DstIndex) = Flags.lookup(ID);
    unsigned SrcBehaviorValue = SrcBehavior->getZExtValue();

    // If this is a requirement, add it and continue.
    if (SrcBehaviorValue == Module::Require) {
      // If the destination module does not already have this requirement, add
      // it.
      if (Requirements.insert(cast<MDNode>(SrcOp->getOperand(2)))) {
        DstModFlags->addOperand(SrcOp);
      }
      continue;
    }

    // If there is no existing flag with this ID, just add it.
    if (!DstOp) {
      Flags[ID] = std::make_pair(SrcOp, DstModFlags->getNumOperands());
      DstModFlags->addOperand(SrcOp);
      continue;
    }

    // Otherwise, perform a merge.
    ConstantInt *DstBehavior =
        mdconst::extract<ConstantInt>(DstOp->getOperand(0));
    unsigned DstBehaviorValue = DstBehavior->getZExtValue();

    // If either flag has override behavior, handle it first.
    if (DstBehaviorValue == Module::Override) {
      // Diagnose inconsistent flags which both have override behavior.
      if (SrcBehaviorValue == Module::Override &&
          SrcOp->getOperand(2) != DstOp->getOperand(2)) {
        emitError("linking module flags '" + ID->getString() +
                  "': IDs have conflicting override values");
      }
      continue;
    } else if (SrcBehaviorValue == Module::Override) {
      // Update the destination flag to that of the source.
      DstModFlags->setOperand(DstIndex, SrcOp);
      Flags[ID].first = SrcOp;
      continue;
    }

    // Diagnose inconsistent merge behavior types.
    if (SrcBehaviorValue != DstBehaviorValue) {
      emitError("linking module flags '" + ID->getString() +
                "': IDs have conflicting behaviors");
      continue;
    }

    auto replaceDstValue = [&](MDNode *New) {
      Metadata *FlagOps[] = {DstOp->getOperand(0), ID, New};
      MDNode *Flag = MDNode::get(DstM.getContext(), FlagOps);
      DstModFlags->setOperand(DstIndex, Flag);
      Flags[ID].first = Flag;
    };

    // Perform the merge for standard behavior types.
    switch (SrcBehaviorValue) {
    case Module::Require:
    case Module::Override:
      llvm_unreachable("not possible");
    case Module::Error: {
      // Emit an error if the values differ.
      if (SrcOp->getOperand(2) != DstOp->getOperand(2)) {
        emitError("linking module flags '" + ID->getString() +
                  "': IDs have conflicting values");
      }
      continue;
    }
    case Module::Warning: {
      // Emit a warning if the values differ.
      if (SrcOp->getOperand(2) != DstOp->getOperand(2)) {
        emitWarning("linking module flags '" + ID->getString() +
                    "': IDs have conflicting values");
      }
      continue;
    }
    case Module::Append: {
      MDNode *DstValue = cast<MDNode>(DstOp->getOperand(2));
      MDNode *SrcValue = cast<MDNode>(SrcOp->getOperand(2));
      SmallVector<Metadata *, 8> MDs;
      MDs.reserve(DstValue->getNumOperands() + SrcValue->getNumOperands());
      MDs.append(DstValue->op_begin(), DstValue->op_end());
      MDs.append(SrcValue->op_begin(), SrcValue->op_end());

      replaceDstValue(MDNode::get(DstM.getContext(), MDs));
      break;
    }
    case Module::AppendUnique: {
      SmallSetVector<Metadata *, 16> Elts;
      MDNode *DstValue = cast<MDNode>(DstOp->getOperand(2));
      MDNode *SrcValue = cast<MDNode>(SrcOp->getOperand(2));
      Elts.insert(DstValue->op_begin(), DstValue->op_end());
      Elts.insert(SrcValue->op_begin(), SrcValue->op_end());

      replaceDstValue(MDNode::get(DstM.getContext(),
                                  makeArrayRef(Elts.begin(), Elts.end())));
      break;
    }
    }
  }

  // Check all of the requirements.
  for (unsigned I = 0, E = Requirements.size(); I != E; ++I) {
    MDNode *Requirement = Requirements[I];
    MDString *Flag = cast<MDString>(Requirement->getOperand(0));
    Metadata *ReqValue = Requirement->getOperand(1);

    MDNode *Op = Flags[Flag].first;
    if (!Op || Op->getOperand(2) != ReqValue) {
      emitError("linking module flags '" + Flag->getString() +
                "': does not have the required value");
      continue;
    }
  }

  return HasError;
}

// This function returns true if the triples match.
static bool triplesMatch(const Triple &T0, const Triple &T1) {
  // If vendor is apple, ignore the version number.
  if (T0.getVendor() == Triple::Apple)
    return T0.getArch() == T1.getArch() && T0.getSubArch() == T1.getSubArch() &&
           T0.getVendor() == T1.getVendor() && T0.getOS() == T1.getOS();

  return T0 == T1;
}

// This function returns the merged triple.
static std::string mergeTriples(const Triple &SrcTriple,
                                const Triple &DstTriple) {
  // If vendor is apple, pick the triple with the larger version number.
  if (SrcTriple.getVendor() == Triple::Apple)
    if (DstTriple.isOSVersionLT(SrcTriple))
      return SrcTriple.str();

  return DstTriple.str();
}

bool IRLinker::run() {
  // Inherit the target data from the source module if the destination module
  // doesn't have one already.
  if (DstM.getDataLayout().isDefault())
    DstM.setDataLayout(SrcM.getDataLayout());

  if (SrcM.getDataLayout() != DstM.getDataLayout()) {
    emitWarning("Linking two modules of different data layouts: '" +
                SrcM.getModuleIdentifier() + "' is '" +
                SrcM.getDataLayoutStr() + "' whereas '" +
                DstM.getModuleIdentifier() + "' is '" +
                DstM.getDataLayoutStr() + "'\n");
  }

  // Copy the target triple from the source to dest if the dest's is empty.
  if (DstM.getTargetTriple().empty() && !SrcM.getTargetTriple().empty())
    DstM.setTargetTriple(SrcM.getTargetTriple());

  Triple SrcTriple(SrcM.getTargetTriple()), DstTriple(DstM.getTargetTriple());

  if (!SrcM.getTargetTriple().empty() && !triplesMatch(SrcTriple, DstTriple))
    emitWarning("Linking two modules of different target triples: " +
                SrcM.getModuleIdentifier() + "' is '" + SrcM.getTargetTriple() +
                "' whereas '" + DstM.getModuleIdentifier() + "' is '" +
                DstM.getTargetTriple() + "'\n");

  DstM.setTargetTriple(mergeTriples(SrcTriple, DstTriple));

  // Append the module inline asm string.
  if (!SrcM.getModuleInlineAsm().empty()) {
    if (DstM.getModuleInlineAsm().empty())
      DstM.setModuleInlineAsm(SrcM.getModuleInlineAsm());
    else
      DstM.setModuleInlineAsm(DstM.getModuleInlineAsm() + "\n" +
                              SrcM.getModuleInlineAsm());
  }

  // Loop over all of the linked values to compute type mappings.
  computeTypeMapping();

  std::reverse(Worklist.begin(), Worklist.end());
  while (!Worklist.empty()) {
    GlobalValue *GV = Worklist.back();
    Worklist.pop_back();

    // Already mapped.
    if (ValueMap.find(GV) != ValueMap.end() ||
        AliasValueMap.find(GV) != AliasValueMap.end())
      continue;

    assert(!GV->isDeclaration());
    MapValue(GV, ValueMap, ValueMapperFlags, &TypeMap, &GValMaterializer);
    if (HasError)
      return true;
  }

  // Note that we are done linking global value bodies. This prevents
  // metadata linking from creating new references.
  DoneLinkingBodies = true;

  // Remap all of the named MDNodes in Src into the DstM module. We do this
  // after linking GlobalValues so that MDNodes that reference GlobalValues
  // are properly remapped.
  if (shouldLinkMetadata()) {
    // Even if just linking metadata we should link decls above in case
    // any are referenced by metadata. IRLinker::shouldLink ensures that
    // we don't actually link anything from source.
    if (IsMetadataLinkingPostpass) {
      // Ensure metadata materialized
      if (SrcM.getMaterializer()->materializeMetadata())
        return true;
      SrcM.getMaterializer()->saveMetadataList(MetadataToIDs,
                                               /* OnlyTempMD = */ false);
    }

    linkNamedMDNodes();

    if (IsMetadataLinkingPostpass) {
      // Handle anything left in the ValIDToTempMDMap, such as metadata nodes
      // not reached by the dbg.cu NamedMD (i.e. only reached from
      // instructions).
      // Walk the MetadataToIDs once to find the set of new (imported) MD
      // that still has corresponding temporary metadata, and invoke metadata
      // mapping on each one.
      for (auto MDI : MetadataToIDs) {
        if (!ValIDToTempMDMap->count(MDI.second))
          continue;
        MapMetadata(MDI.first, ValueMap, ValueMapperFlags, &TypeMap,
                    &GValMaterializer);
      }
      assert(ValIDToTempMDMap->empty());
    }

    // Merge the module flags into the DstM module.
    if (linkModuleFlagsMetadata())
      return true;
  }

  return false;
}

IRMover::StructTypeKeyInfo::KeyTy::KeyTy(ArrayRef<Type *> E, bool P)
    : ETypes(E), IsPacked(P) {}

IRMover::StructTypeKeyInfo::KeyTy::KeyTy(const StructType *ST)
    : ETypes(ST->elements()), IsPacked(ST->isPacked()) {}

bool IRMover::StructTypeKeyInfo::KeyTy::operator==(const KeyTy &That) const {
  if (IsPacked != That.IsPacked)
    return false;
  if (ETypes != That.ETypes)
    return false;
  return true;
}

bool IRMover::StructTypeKeyInfo::KeyTy::operator!=(const KeyTy &That) const {
  return !this->operator==(That);
}

StructType *IRMover::StructTypeKeyInfo::getEmptyKey() {
  return DenseMapInfo<StructType *>::getEmptyKey();
}

StructType *IRMover::StructTypeKeyInfo::getTombstoneKey() {
  return DenseMapInfo<StructType *>::getTombstoneKey();
}

unsigned IRMover::StructTypeKeyInfo::getHashValue(const KeyTy &Key) {
  return hash_combine(hash_combine_range(Key.ETypes.begin(), Key.ETypes.end()),
                      Key.IsPacked);
}

unsigned IRMover::StructTypeKeyInfo::getHashValue(const StructType *ST) {
  return getHashValue(KeyTy(ST));
}

bool IRMover::StructTypeKeyInfo::isEqual(const KeyTy &LHS,
                                         const StructType *RHS) {
  if (RHS == getEmptyKey() || RHS == getTombstoneKey())
    return false;
  return LHS == KeyTy(RHS);
}

bool IRMover::StructTypeKeyInfo::isEqual(const StructType *LHS,
                                         const StructType *RHS) {
  if (RHS == getEmptyKey())
    return LHS == getEmptyKey();

  if (RHS == getTombstoneKey())
    return LHS == getTombstoneKey();

  return KeyTy(LHS) == KeyTy(RHS);
}

void IRMover::IdentifiedStructTypeSet::addNonOpaque(StructType *Ty) {
  assert(!Ty->isOpaque());
  NonOpaqueStructTypes.insert(Ty);
}

void IRMover::IdentifiedStructTypeSet::switchToNonOpaque(StructType *Ty) {
  assert(!Ty->isOpaque());
  NonOpaqueStructTypes.insert(Ty);
  bool Removed = OpaqueStructTypes.erase(Ty);
  (void)Removed;
  assert(Removed);
}

void IRMover::IdentifiedStructTypeSet::addOpaque(StructType *Ty) {
  assert(Ty->isOpaque());
  OpaqueStructTypes.insert(Ty);
}

StructType *
IRMover::IdentifiedStructTypeSet::findNonOpaque(ArrayRef<Type *> ETypes,
                                                bool IsPacked) {
  IRMover::StructTypeKeyInfo::KeyTy Key(ETypes, IsPacked);
  auto I = NonOpaqueStructTypes.find_as(Key);
  if (I == NonOpaqueStructTypes.end())
    return nullptr;
  return *I;
}

bool IRMover::IdentifiedStructTypeSet::hasType(StructType *Ty) {
  if (Ty->isOpaque())
    return OpaqueStructTypes.count(Ty);
  auto I = NonOpaqueStructTypes.find(Ty);
  if (I == NonOpaqueStructTypes.end())
    return false;
  return *I == Ty;
}

IRMover::IRMover(Module &M) : Composite(M) {
  TypeFinder StructTypes;
  StructTypes.run(M, true);
  for (StructType *Ty : StructTypes) {
    if (Ty->isOpaque())
      IdentifiedStructTypes.addOpaque(Ty);
    else
      IdentifiedStructTypes.addNonOpaque(Ty);
  }
}

bool IRMover::move(
    Module &Src, ArrayRef<GlobalValue *> ValuesToLink,
    std::function<void(GlobalValue &, ValueAdder Add)> AddLazyFor,
    DenseMap<unsigned, MDNode *> *ValIDToTempMDMap,
    bool IsMetadataLinkingPostpass) {
  IRLinker TheIRLinker(Composite, IdentifiedStructTypes, Src, ValuesToLink,
                       AddLazyFor, ValIDToTempMDMap, IsMetadataLinkingPostpass);
  bool RetCode = TheIRLinker.run();
  Composite.dropTriviallyDeadConstantArrays();
  return RetCode;
}
