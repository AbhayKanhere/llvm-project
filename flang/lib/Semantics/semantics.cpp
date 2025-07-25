//===-- lib/Semantics/semantics.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "flang/Semantics/semantics.h"
#include "assignment.h"
#include "canonicalize-acc.h"
#include "canonicalize-directives.h"
#include "canonicalize-do.h"
#include "canonicalize-omp.h"
#include "check-acc-structure.h"
#include "check-allocate.h"
#include "check-arithmeticif.h"
#include "check-case.h"
#include "check-coarray.h"
#include "check-cuda.h"
#include "check-data.h"
#include "check-deallocate.h"
#include "check-declarations.h"
#include "check-do-forall.h"
#include "check-if-stmt.h"
#include "check-io.h"
#include "check-namelist.h"
#include "check-nullify.h"
#include "check-omp-structure.h"
#include "check-purity.h"
#include "check-return.h"
#include "check-select-rank.h"
#include "check-select-type.h"
#include "check-stop.h"
#include "compute-offsets.h"
#include "mod-file.h"
#include "resolve-labels.h"
#include "resolve-names.h"
#include "rewrite-parse-tree.h"
#include "flang/Parser/parse-tree-visitor.h"
#include "flang/Parser/tools.h"
#include "flang/Semantics/expression.h"
#include "flang/Semantics/scope.h"
#include "flang/Semantics/symbol.h"
#include "flang/Support/default-kinds.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace Fortran::semantics {

using NameToSymbolMap = std::multimap<parser::CharBlock, SymbolRef>;
static void DoDumpSymbols(llvm::raw_ostream &, const Scope &, int indent = 0);
static void PutIndent(llvm::raw_ostream &, int indent);

static void GetSymbolNames(const Scope &scope, NameToSymbolMap &symbols) {
  // Finds all symbol names in the scope without collecting duplicates.
  for (const auto &pair : scope) {
    symbols.emplace(pair.second->name(), *pair.second);
  }
  for (const auto &pair : scope.commonBlocks()) {
    symbols.emplace(pair.second->name(), *pair.second);
  }
  for (const auto &child : scope.children()) {
    GetSymbolNames(child, symbols);
  }
}

// A parse tree visitor that calls Enter/Leave functions from each checker
// class C supplied as template parameters. Enter is called before the node's
// children are visited, Leave is called after. No two checkers may have the
// same Enter or Leave function. Each checker must be constructible from
// SemanticsContext and have BaseChecker as a virtual base class.
template <typename... C>
class SemanticsVisitor : public virtual BaseChecker, public virtual C... {
public:
  using BaseChecker::Enter;
  using BaseChecker::Leave;
  using C::Enter...;
  using C::Leave...;
  SemanticsVisitor(SemanticsContext &context)
      : C{context}..., context_{context} {}

  template <typename N> bool Pre(const N &node) {
    if constexpr (common::HasMember<const N *, ConstructNode>) {
      context_.PushConstruct(node);
    }
    Enter(node);
    return true;
  }
  template <typename N> void Post(const N &node) {
    Leave(node);
    if constexpr (common::HasMember<const N *, ConstructNode>) {
      context_.PopConstruct();
    }
  }

  template <typename T> bool Pre(const parser::Statement<T> &node) {
    context_.set_location(node.source);
    Enter(node);
    return true;
  }
  template <typename T> bool Pre(const parser::UnlabeledStatement<T> &node) {
    context_.set_location(node.source);
    Enter(node);
    return true;
  }
  template <typename T> void Post(const parser::Statement<T> &node) {
    Leave(node);
    context_.set_location(std::nullopt);
  }
  template <typename T> void Post(const parser::UnlabeledStatement<T> &node) {
    Leave(node);
    context_.set_location(std::nullopt);
  }

  bool Walk(const parser::Program &program) {
    parser::Walk(program, *this);
    return !context_.AnyFatalError();
  }

private:
  SemanticsContext &context_;
};

class MiscChecker : public virtual BaseChecker {
public:
  explicit MiscChecker(SemanticsContext &context) : context_{context} {}
  void Leave(const parser::EntryStmt &) {
    if (!context_.constructStack().empty()) { // C1571
      context_.Say("ENTRY may not appear in an executable construct"_err_en_US);
    }
  }
  void Leave(const parser::AssignStmt &stmt) {
    CheckAssignGotoName(std::get<parser::Name>(stmt.t));
  }
  void Leave(const parser::AssignedGotoStmt &stmt) {
    CheckAssignGotoName(std::get<parser::Name>(stmt.t));
  }

private:
  void CheckAssignGotoName(const parser::Name &name) {
    if (context_.HasError(name.symbol)) {
      return;
    }
    const Symbol &symbol{DEREF(name.symbol)};
    auto type{evaluate::DynamicType::From(symbol)};
    if (!IsVariableName(symbol) || symbol.Rank() != 0 || !type ||
        type->category() != TypeCategory::Integer ||
        type->kind() !=
            context_.defaultKinds().GetDefaultKind(TypeCategory::Integer)) {
      context_
          .Say(name.source,
              "'%s' must be a default integer scalar variable"_err_en_US,
              name.source)
          .Attach(symbol.name(), "Declaration of '%s'"_en_US, symbol.name());
    }
  }

  SemanticsContext &context_;
};

static void WarnUndefinedFunctionResult(
    SemanticsContext &context, const Scope &scope) {
  auto WasDefined{[&context](const Symbol &symbol) {
    return context.IsSymbolDefined(symbol) ||
        IsInitialized(symbol, /*ignoreDataStatements=*/true,
            /*ignoreAllocatable=*/true, /*ignorePointer=*/true);
  }};
  if (const Symbol * symbol{scope.symbol()}) {
    if (const auto *subp{symbol->detailsIf<SubprogramDetails>()}) {
      if (subp->isFunction() && !subp->isInterface() && !subp->stmtFunction()) {
        bool wasDefined{WasDefined(subp->result())};
        if (!wasDefined) {
          // Definitions of ENTRY result variables also count.
          for (const auto &pair : scope) {
            const Symbol &local{*pair.second};
            if (IsFunctionResult(local) && WasDefined(local)) {
              wasDefined = true;
              break;
            }
          }
          if (!wasDefined) {
            context.Warn(common::UsageWarning::UndefinedFunctionResult,
                symbol->name(), "Function result is never defined"_warn_en_US);
          }
        }
      }
    }
  }
  if (!scope.IsModuleFile()) {
    for (const Scope &child : scope.children()) {
      WarnUndefinedFunctionResult(context, child);
    }
  }
}

using StatementSemanticsPass1 = ExprChecker;
using StatementSemanticsPass2 = SemanticsVisitor<AllocateChecker,
    ArithmeticIfStmtChecker, AssignmentChecker, CaseChecker, CoarrayChecker,
    DataChecker, DeallocateChecker, DoForallChecker, IfStmtChecker, IoChecker,
    MiscChecker, NamelistChecker, NullifyChecker, PurityChecker,
    ReturnStmtChecker, SelectRankConstructChecker, SelectTypeChecker,
    StopChecker>;

static bool PerformStatementSemantics(
    SemanticsContext &context, parser::Program &program) {
  ResolveNames(context, program, context.globalScope());
  RewriteParseTree(context, program);
  ComputeOffsets(context, context.globalScope());
  CheckDeclarations(context);
  StatementSemanticsPass1{context}.Walk(program);
  StatementSemanticsPass2 pass2{context};
  pass2.Walk(program);
  if (context.languageFeatures().IsEnabled(common::LanguageFeature::OpenACC)) {
    SemanticsVisitor<AccStructureChecker>{context}.Walk(program);
  }
  if (context.languageFeatures().IsEnabled(common::LanguageFeature::OpenMP)) {
    SemanticsVisitor<OmpStructureChecker>{context}.Walk(program);
  }
  if (context.languageFeatures().IsEnabled(common::LanguageFeature::CUDA)) {
    SemanticsVisitor<CUDAChecker>{context}.Walk(program);
  }
  if (!context.messages().AnyFatalError()) {
    WarnUndefinedFunctionResult(context, context.globalScope());
  }
  if (!context.AnyFatalError()) {
    pass2.CompileDataInitializationsIntoInitializers();
  }
  return !context.AnyFatalError();
}

/// This class keeps track of the common block appearances with the biggest size
/// and with an initial value (if any) in a program. This allows reporting
/// conflicting initialization and warning about appearances of a same
/// named common block with different sizes. The biggest common block size and
/// initialization (if any) can later be provided so that lowering can generate
/// the correct symbol size and initial values, even when named common blocks
/// appears with different sizes and are initialized outside of block data.
class CommonBlockMap {
private:
  struct CommonBlockInfo {
    // Common block symbol for the appearance with the biggest size.
    SymbolRef biggestSize;
    // Common block symbol for the appearance with the initialized members (if
    // any).
    std::optional<SymbolRef> initialization;
  };

public:
  void MapCommonBlockAndCheckConflicts(
      SemanticsContext &context, const Symbol &common) {
    const Symbol *isInitialized{CommonBlockIsInitialized(common)};
    // Merge common according to the name they will have in the object files.
    // This allows merging BIND(C) and non BIND(C) common block instead of
    // later crashing. This "merge" matches what ifort/gfortran/nvfortran are
    // doing and what a linker would do if the definition were in distinct
    // files.
    std::string commonName{
        GetCommonBlockObjectName(common, context.underscoring())};
    auto [it, firstAppearance] = commonBlocks_.insert({commonName,
        isInitialized ? CommonBlockInfo{common, common}
                      : CommonBlockInfo{common, std::nullopt}});
    if (!firstAppearance) {
      CommonBlockInfo &info{it->second};
      if (isInitialized) {
        if (info.initialization.has_value() &&
            &**info.initialization != &common) {
          // Use the location of the initialization in the error message because
          // common block symbols may have no location if they are blank
          // commons.
          const Symbol &previousInit{
              DEREF(CommonBlockIsInitialized(**info.initialization))};
          context
              .Say(isInitialized->name(),
                  "Multiple initialization of COMMON block /%s/"_err_en_US,
                  common.name())
              .Attach(previousInit.name(),
                  "Previous initialization of COMMON block /%s/"_en_US,
                  common.name());
        } else {
          info.initialization = common;
        }
      }
      if (common.size() != info.biggestSize->size() && !common.name().empty()) {
        if (auto *msg{context.Warn(common::LanguageFeature::DistinctCommonSizes,
                common.name(),
                "A named COMMON block should have the same size everywhere it appears (%zd bytes here)"_port_en_US,
                common.size())}) {
          msg->Attach(info.biggestSize->name(),
              "Previously defined with a size of %zd bytes"_en_US,
              info.biggestSize->size());
        }
      }
      if (common.size() > info.biggestSize->size()) {
        info.biggestSize = common;
      }
    }
  }

  CommonBlockList GetCommonBlocks() const {
    CommonBlockList result;
    for (const auto &[_, blockInfo] : commonBlocks_) {
      result.emplace_back(
          std::make_pair(blockInfo.initialization ? *blockInfo.initialization
                                                  : blockInfo.biggestSize,
              blockInfo.biggestSize->size()));
    }
    return result;
  }

private:
  /// Return the symbol of an initialized member if a COMMON block
  /// is initalized. Otherwise, return nullptr.
  static Symbol *CommonBlockIsInitialized(const Symbol &common) {
    const auto &commonDetails =
        common.get<Fortran::semantics::CommonBlockDetails>();

    for (const auto &member : commonDetails.objects()) {
      if (IsInitialized(*member)) {
        return &*member;
      }
    }

    // Common block may be initialized via initialized variables that are in an
    // equivalence with the common block members.
    for (const Fortran::semantics::EquivalenceSet &set :
        common.owner().equivalenceSets()) {
      for (const Fortran::semantics::EquivalenceObject &obj : set) {
        if (!obj.symbol.test(
                Fortran::semantics::Symbol::Flag::CompilerCreated)) {
          if (FindCommonBlockContaining(obj.symbol) == &common &&
              IsInitialized(obj.symbol)) {
            return &obj.symbol;
          }
        }
      }
    }
    return nullptr;
  }

  std::map<std::string, CommonBlockInfo> commonBlocks_;
};

SemanticsContext::SemanticsContext(
    const common::IntrinsicTypeDefaultKinds &defaultKinds,
    const common::LanguageFeatureControl &languageFeatures,
    const common::LangOptions &langOpts,
    parser::AllCookedSources &allCookedSources)
    : defaultKinds_{defaultKinds}, languageFeatures_{languageFeatures},
      langOpts_{langOpts}, allCookedSources_{allCookedSources},
      intrinsics_{evaluate::IntrinsicProcTable::Configure(defaultKinds_)},
      globalScope_{*this}, intrinsicModulesScope_{globalScope_.MakeScope(
                               Scope::Kind::IntrinsicModules, nullptr)},
      foldingContext_{parser::ContextualMessages{&messages_}, defaultKinds_,
          intrinsics_, targetCharacteristics_, languageFeatures_, tempNames_} {}

SemanticsContext::~SemanticsContext() {}

int SemanticsContext::GetDefaultKind(TypeCategory category) const {
  return defaultKinds_.GetDefaultKind(category);
}

const DeclTypeSpec &SemanticsContext::MakeNumericType(
    TypeCategory category, int kind) {
  if (kind == 0) {
    kind = GetDefaultKind(category);
  }
  return globalScope_.MakeNumericType(category, KindExpr{kind});
}
const DeclTypeSpec &SemanticsContext::MakeLogicalType(int kind) {
  if (kind == 0) {
    kind = GetDefaultKind(TypeCategory::Logical);
  }
  return globalScope_.MakeLogicalType(KindExpr{kind});
}

bool SemanticsContext::AnyFatalError() const {
  return messages_.AnyFatalError(warningsAreErrors_);
}
bool SemanticsContext::HasError(const Symbol &symbol) {
  return errorSymbols_.count(symbol) > 0;
}
bool SemanticsContext::HasError(const Symbol *symbol) {
  return !symbol || HasError(*symbol);
}
bool SemanticsContext::HasError(const parser::Name &name) {
  return HasError(name.symbol);
}
void SemanticsContext::SetError(const Symbol &symbol, bool value) {
  if (value) {
    CheckError(symbol);
    errorSymbols_.emplace(symbol);
  }
}
void SemanticsContext::CheckError(const Symbol &symbol) {
  if (!AnyFatalError()) {
    std::string buf;
    llvm::raw_string_ostream ss{buf};
    ss << symbol;
    common::die(
        "No error was reported but setting error on: %s", ss.str().c_str());
  }
}

bool SemanticsContext::ScopeIndexComparator::operator()(
    parser::CharBlock x, parser::CharBlock y) const {
  return x.begin() < y.begin() ||
      (x.begin() == y.begin() && x.size() > y.size());
}

auto SemanticsContext::SearchScopeIndex(parser::CharBlock source)
    -> ScopeIndex::iterator {
  if (!scopeIndex_.empty()) {
    auto iter{scopeIndex_.upper_bound(source)};
    auto begin{scopeIndex_.begin()};
    do {
      --iter;
      if (iter->first.Contains(source)) {
        return iter;
      }
    } while (iter != begin);
  }
  return scopeIndex_.end();
}

const Scope &SemanticsContext::FindScope(parser::CharBlock source) const {
  return const_cast<SemanticsContext *>(this)->FindScope(source);
}

Scope &SemanticsContext::FindScope(parser::CharBlock source) {
  if (auto iter{SearchScopeIndex(source)}; iter != scopeIndex_.end()) {
    return iter->second;
  } else {
    common::die(
        "SemanticsContext::FindScope(): invalid source location for '%s'",
        source.ToString().c_str());
  }
}

void SemanticsContext::UpdateScopeIndex(
    Scope &scope, parser::CharBlock newSource) {
  if (scope.sourceRange().empty()) {
    scopeIndex_.emplace(newSource, scope);
  } else if (!scope.sourceRange().Contains(newSource)) {
    auto iter{SearchScopeIndex(scope.sourceRange())};
    CHECK(iter != scopeIndex_.end());
    while (&iter->second != &scope) {
      CHECK(iter != scopeIndex_.begin());
      --iter;
    }
    scopeIndex_.erase(iter);
    scopeIndex_.emplace(newSource, scope);
  }
}

bool SemanticsContext::IsInModuleFile(parser::CharBlock source) const {
  for (const Scope *scope{&FindScope(source)}; !scope->IsGlobal();
       scope = &scope->parent()) {
    if (scope->IsModuleFile()) {
      return true;
    }
  }
  return false;
}

void SemanticsContext::PopConstruct() {
  CHECK(!constructStack_.empty());
  constructStack_.pop_back();
}

parser::Message *SemanticsContext::CheckIndexVarRedefine(
    const parser::CharBlock &location, const Symbol &variable,
    parser::MessageFixedText &&message) {
  const Symbol &symbol{ResolveAssociations(variable)};
  auto it{activeIndexVars_.find(symbol)};
  if (it != activeIndexVars_.end()) {
    std::string kind{EnumToString(it->second.kind)};
    return &Say(location, std::move(message), kind, symbol.name())
                .Attach(
                    it->second.location, "Enclosing %s construct"_en_US, kind);
  } else {
    return nullptr;
  }
}

void SemanticsContext::WarnIndexVarRedefine(
    const parser::CharBlock &location, const Symbol &variable) {
  if (ShouldWarn(common::UsageWarning::IndexVarRedefinition)) {
    if (auto *msg{CheckIndexVarRedefine(location, variable,
            "Possible redefinition of %s variable '%s'"_warn_en_US)}) {
      msg->set_usageWarning(common::UsageWarning::IndexVarRedefinition);
    }
  }
}

void SemanticsContext::CheckIndexVarRedefine(
    const parser::CharBlock &location, const Symbol &variable) {
  CheckIndexVarRedefine(
      location, variable, "Cannot redefine %s variable '%s'"_err_en_US);
}

void SemanticsContext::CheckIndexVarRedefine(const parser::Variable &variable) {
  if (const Symbol * entity{GetLastName(variable).symbol}) {
    CheckIndexVarRedefine(variable.GetSource(), *entity);
  }
}

void SemanticsContext::CheckIndexVarRedefine(const parser::Name &name) {
  if (const Symbol * entity{name.symbol}) {
    CheckIndexVarRedefine(name.source, *entity);
  }
}

void SemanticsContext::ActivateIndexVar(
    const parser::Name &name, IndexVarKind kind) {
  CheckIndexVarRedefine(name);
  if (const Symbol * indexVar{name.symbol}) {
    activeIndexVars_.emplace(
        ResolveAssociations(*indexVar), IndexVarInfo{name.source, kind});
  }
}

void SemanticsContext::DeactivateIndexVar(const parser::Name &name) {
  if (Symbol * indexVar{name.symbol}) {
    auto it{activeIndexVars_.find(ResolveAssociations(*indexVar))};
    if (it != activeIndexVars_.end() && it->second.location == name.source) {
      activeIndexVars_.erase(it);
    }
  }
}

SymbolVector SemanticsContext::GetIndexVars(IndexVarKind kind) {
  SymbolVector result;
  for (const auto &[symbol, info] : activeIndexVars_) {
    if (info.kind == kind) {
      result.push_back(symbol);
    }
  }
  return result;
}

SourceName SemanticsContext::SaveTempName(std::string &&name) {
  return {*tempNames_.emplace(std::move(name)).first};
}

SourceName SemanticsContext::GetTempName(const Scope &scope) {
  for (const auto &str : tempNames_) {
    if (IsTempName(str)) {
      SourceName name{str};
      if (scope.find(name) == scope.end()) {
        return name;
      }
    }
  }
  return SaveTempName(".F18."s + std::to_string(tempNames_.size()));
}

bool SemanticsContext::IsTempName(const std::string &name) {
  return name.size() > 5 && name.substr(0, 5) == ".F18.";
}

Scope *SemanticsContext::GetBuiltinModule(const char *name) {
  return ModFileReader{*this}.Read(SourceName{name, std::strlen(name)},
      true /*intrinsic*/, nullptr, /*silent=*/true);
}

void SemanticsContext::UseFortranBuiltinsModule() {
  if (builtinsScope_ == nullptr) {
    builtinsScope_ = GetBuiltinModule("__fortran_builtins");
    if (builtinsScope_) {
      intrinsics_.SupplyBuiltins(*builtinsScope_);
    }
  }
}

void SemanticsContext::UsePPCBuiltinTypesModule() {
  if (ppcBuiltinTypesScope_ == nullptr) {
    ppcBuiltinTypesScope_ = GetBuiltinModule("__ppc_types");
  }
}

const Scope &SemanticsContext::GetCUDABuiltinsScope() {
  if (!cudaBuiltinsScope_) {
    cudaBuiltinsScope_ = GetBuiltinModule("__cuda_builtins");
    CHECK(cudaBuiltinsScope_.value() != nullptr);
  }
  return **cudaBuiltinsScope_;
}

const Scope &SemanticsContext::GetCUDADeviceScope() {
  if (!cudaDeviceScope_) {
    cudaDeviceScope_ = GetBuiltinModule("cudadevice");
    CHECK(cudaDeviceScope_.value() != nullptr);
  }
  return **cudaDeviceScope_;
}

void SemanticsContext::UsePPCBuiltinsModule() {
  if (ppcBuiltinsScope_ == nullptr) {
    ppcBuiltinsScope_ = GetBuiltinModule("__ppc_intrinsics");
  }
}

parser::Program &SemanticsContext::SaveParseTree(parser::Program &&tree) {
  return modFileParseTrees_.emplace_back(std::move(tree));
}

bool Semantics::Perform() {
  // Implicitly USE the __Fortran_builtins module so that special types
  // (e.g., __builtin_team_type) are available to semantics, esp. for
  // intrinsic checking.
  if (!program_.v.empty()) {
    const auto *frontModule{std::get_if<common::Indirection<parser::Module>>(
        &program_.v.front().u)};
    if (frontModule &&
        (std::get<parser::Statement<parser::ModuleStmt>>(frontModule->value().t)
                    .statement.v.source == "__fortran_builtins" ||
            std::get<parser::Statement<parser::ModuleStmt>>(
                frontModule->value().t)
                    .statement.v.source == "__ppc_types")) {
      // Don't try to read the builtins module when we're actually building it.
    } else if (frontModule &&
        (std::get<parser::Statement<parser::ModuleStmt>>(frontModule->value().t)
                    .statement.v.source == "__ppc_intrinsics" ||
            std::get<parser::Statement<parser::ModuleStmt>>(
                frontModule->value().t)
                    .statement.v.source == "mma")) {
      // The derived type definition for the vectors is needed.
      context_.UsePPCBuiltinTypesModule();
    } else {
      context_.UseFortranBuiltinsModule();
      llvm::Triple targetTriple{llvm::Triple(
          llvm::Triple::normalize(llvm::sys::getDefaultTargetTriple()))};
      // Only use __ppc_intrinsics module when targetting PowerPC arch
      if (context_.targetCharacteristics().isPPC()) {
        context_.UsePPCBuiltinTypesModule();
        context_.UsePPCBuiltinsModule();
      }
    }
  }
  return ValidateLabels(context_, program_) &&
      parser::CanonicalizeDo(program_) && // force line break
      CanonicalizeAcc(context_.messages(), program_) &&
      CanonicalizeOmp(context_.messages(), program_) &&
      CanonicalizeCUDA(program_) &&
      PerformStatementSemantics(context_, program_) &&
      CanonicalizeDirectives(context_.messages(), program_) &&
      ModFileWriter{context_}
          .set_hermeticModuleFileOutput(hermeticModuleFileOutput_)
          .WriteAll();
}

void Semantics::EmitMessages(llvm::raw_ostream &os) {
  // Resolve the CharBlock locations of the Messages to ProvenanceRanges
  // so messages from parsing and semantics are intermixed in source order.
  context_.messages().ResolveProvenances(context_.allCookedSources());
  context_.messages().Emit(os, context_.allCookedSources(),
      /*echoSourceLine=*/true, &context_.languageFeatures(),
      context_.maxErrors(), context_.warningsAreErrors());
}

void SemanticsContext::DumpSymbols(llvm::raw_ostream &os) {
  DoDumpSymbols(os, globalScope());
}

ProgramTree &SemanticsContext::SaveProgramTree(ProgramTree &&tree) {
  return programTrees_.emplace_back(std::move(tree));
}

void Semantics::DumpSymbols(llvm::raw_ostream &os) { context_.DumpSymbols(os); }

void Semantics::DumpSymbolsSources(llvm::raw_ostream &os) const {
  NameToSymbolMap symbols;
  GetSymbolNames(context_.globalScope(), symbols);
  const parser::AllCookedSources &allCooked{context_.allCookedSources()};
  for (const auto &pair : symbols) {
    const Symbol &symbol{pair.second};
    if (auto sourceInfo{allCooked.GetSourcePositionRange(symbol.name())}) {
      os << symbol.name().ToString() << ": " << sourceInfo->first.path << ", "
         << sourceInfo->first.line << ", " << sourceInfo->first.column << "-"
         << sourceInfo->second.column << "\n";
    } else if (symbol.has<semantics::UseDetails>()) {
      os << symbol.name().ToString() << ": "
         << symbol.GetUltimate().owner().symbol()->name().ToString() << "\n";
    }
  }
}

void DoDumpSymbols(llvm::raw_ostream &os, const Scope &scope, int indent) {
  PutIndent(os, indent);
  os << Scope::EnumToString(scope.kind()) << " scope:";
  if (const auto *symbol{scope.symbol()}) {
    os << ' ' << symbol->name();
  }
  if (scope.alignment().has_value()) {
    os << " size=" << scope.size() << " alignment=" << *scope.alignment();
  }
  if (scope.derivedTypeSpec()) {
    os << " instantiation of " << *scope.derivedTypeSpec();
  }
  os << " sourceRange=" << scope.sourceRange().size() << " bytes\n";
  ++indent;
  for (const auto &pair : scope) {
    const auto &symbol{*pair.second};
    PutIndent(os, indent);
    os << symbol << '\n';
    if (const auto *details{symbol.detailsIf<GenericDetails>()}) {
      if (const auto &type{details->derivedType()}) {
        PutIndent(os, indent);
        os << *type << '\n';
      }
    }
  }
  if (!scope.equivalenceSets().empty()) {
    PutIndent(os, indent);
    os << "Equivalence Sets:";
    for (const auto &set : scope.equivalenceSets()) {
      os << ' ';
      char sep = '(';
      for (const auto &object : set) {
        os << sep << object.AsFortran();
        sep = ',';
      }
      os << ')';
    }
    os << '\n';
  }
  if (!scope.crayPointers().empty()) {
    PutIndent(os, indent);
    os << "Cray Pointers:";
    for (const auto &[pointee, pointer] : scope.crayPointers()) {
      os << " (" << pointer->name() << ',' << pointee << ')';
    }
    os << '\n';
  }
  for (const auto &pair : scope.commonBlocks()) {
    const auto &symbol{*pair.second};
    PutIndent(os, indent);
    os << symbol << '\n';
  }
  for (const auto &child : scope.children()) {
    DoDumpSymbols(os, child, indent);
  }
  --indent;
}

static void PutIndent(llvm::raw_ostream &os, int indent) {
  for (int i = 0; i < indent; ++i) {
    os << "  ";
  }
}

void SemanticsContext::MapCommonBlockAndCheckConflicts(const Symbol &common) {
  if (!commonBlockMap_) {
    commonBlockMap_ = std::make_unique<CommonBlockMap>();
  }
  commonBlockMap_->MapCommonBlockAndCheckConflicts(*this, common);
}

CommonBlockList SemanticsContext::GetCommonBlocks() const {
  if (commonBlockMap_) {
    return commonBlockMap_->GetCommonBlocks();
  }
  return {};
}

void SemanticsContext::NoteDefinedSymbol(const Symbol &symbol) {
  isDefined_.insert(symbol);
}

bool SemanticsContext::IsSymbolDefined(const Symbol &symbol) const {
  return isDefined_.find(symbol) != isDefined_.end();
}

} // namespace Fortran::semantics
