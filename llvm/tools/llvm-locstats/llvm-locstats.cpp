//===-- llvm-locstats.cpp - Debug location coverage utility for llvm ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that works like debug location coverage calculator.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "locstats"
using namespace llvm;
using namespace object;

/// This represents the largest category of debug location coverage being
/// calculated. The first category is 0% location coverage, but the last
/// category is 100% location coverage.
static const int largest_cov_category = 12;

/// @}
/// Command line options.
/// @{

namespace {
using namespace cl;

OptionCategory LocStatsCategory("Specific Options");
static opt<bool> Help("h", desc("Alias for -help"), Hidden,
                      cat(LocStatsCategory));
static opt<std::string>
    InputFilename(Positional, desc("<input object file>"),
                  cat(LocStatsCategory));
static opt<std::string>
    OutputFilename("out-file", cl::init("-"),
                   cl::desc("Redirect output to the specified file."),
                   cl::value_desc("filename"),
                   cat(LocStatsCategory));
static alias OutputFilenameAlias("o", desc("Alias for -out-file."),
                                 aliasopt(OutputFilename),
                                 cat(LocStatsCategory));
static opt<bool>
    OnlyFormalParameters("only-formal-parameters",
         desc("Calculate the location statistics only for formal parameters."),
         cat(LocStatsCategory));
static opt<bool>
    OnlyVariables("only-variables",
         desc("Calculate the location statistics only for local variables."),
         cat(LocStatsCategory));
static opt<bool>
    IgnoreInlined("ignore-inlined",
         desc("Ignore the location statistics on inlined instances."),
         cat(LocStatsCategory));
static opt<bool>
    IgnoreEntryValues("ignore-entry-values",
         desc("Ignore the location statistics on locations with entry values."),
         cat(LocStatsCategory));
} // namespace
/// @}
//===----------------------------------------------------------------------===//

using HandlerFn = std::function<void(ObjectFile &, DWARFContext &DICtx, Twine,
                                     raw_ostream &)>;

/// Extract the low pc from a Die.
static uint64_t getLowPC(DWARFDie Die) {
  auto RangesOrError = Die.getAddressRanges();
  DWARFAddressRangesVector Ranges;
  if (RangesOrError)
    Ranges = RangesOrError.get();
  else
    llvm::consumeError(RangesOrError.takeError());
  if (Ranges.size())
    return Ranges[0].LowPC;
  return dwarf::toAddress(Die.find(dwarf::DW_AT_low_pc), 0);
}

static void collectLocStatsForDie(DWARFDie Die, uint64_t ScopeLowPC,
                                  uint64_t BytesInScope,
                                  std::map<int, unsigned long> &LocStatistics,
                                  unsigned &CumulNumOfVars,
                                  double &TotalAverage,
                                  DWARFContext &DICtx) {
  if (Die.getTag() == dwarf::DW_TAG_variable && OnlyFormalParameters)
    return;
  if (Die.getTag() == dwarf::DW_TAG_formal_parameter && OnlyVariables)
    return;

  // Ignore declarations and artificial variables.
  if (Die.find(dwarf::DW_AT_declaration) || Die.find(dwarf::DW_AT_artificial))
    return;

  // Also ignore extern globals with no DW_AT_location.
  if (Die.find(dwarf::DW_AT_external) && !Die.find(dwarf::DW_AT_location))
    return;

  // Ignore subroutine types.
  if (Die.getTag() == dwarf::DW_TAG_formal_parameter &&
      Die.getParent().getTag() == dwarf::DW_TAG_subroutine_type)
    return;

  if (auto name = Die.getName(DINameKind::ShortName))
    LLVM_DEBUG(llvm::dbgs() << "    -var (or formal param): " << name << "\n");

  double Coverage = 0;

  auto IsEntryValue = [&] (StringRef D) -> bool {
    DWARFUnit *U = Die.getDwarfUnit();
    DataExtractor Data(D, DICtx.isLittleEndian(), 0);
    DWARFExpression Expression(Data, U->getVersion(),
                               U->getAddressByteSize());
    bool IsEntryVal = llvm::any_of(Expression,
                                  [](DWARFExpression::Operation &Op) {
      return Op.getCode() == dwarf::DW_OP_entry_value ||
             Op.getCode() == dwarf::DW_OP_GNU_entry_value;
    });
    return IsEntryVal;
  };

  if (Die.find(dwarf::DW_AT_const_value))
    // This catches constant members *and* variables.
    Coverage = 100;
  else {
    // Handle variables and function arguments location.
    auto FormValue = Die.find(dwarf::DW_AT_location);
    if (FormValue.hasValue()) {
      uint64_t Covered = 0;
      // Get PC coverage.
      if (auto DebugLocOffset = FormValue->getAsSectionOffset()) {
        auto *DebugLoc = Die.getDwarfUnit()->getContext().getDebugLoc();
        if (auto List = DebugLoc->getLocationListAtOffset(*DebugLocOffset)) {
          for (auto Entry : List->Entries) {
            if (IgnoreEntryValues &&
                IsEntryValue({Entry.Loc.data(), Entry.Loc.size()}))
              continue;
            Covered += Entry.End - Entry.Begin;
          }
        }

        // This is wrong location list.
        if (Covered > BytesInScope) {
          Covered = BytesInScope;
          LLVM_DEBUG(llvm::dbgs() << "      -WRONG LOCATION LIST!!!\n");
        }

        Coverage = 100 * (double)Covered / BytesInScope;
      } else {
        // Assume the entire range is covered by a single location.
        Coverage = 100;
      }
    } else {
      // No at_location attribute.
      Coverage = 0;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "      -coverage is " << (int)Coverage << "%\n");

  int CoverageRounded = (int)Coverage;
  TotalAverage += CoverageRounded;
  int PercentageKey;
  if (CoverageRounded == 0)
    PercentageKey = 0;
  else if (CoverageRounded == 100)
    PercentageKey = largest_cov_category - 1;
  else
    PercentageKey = CoverageRounded / 10 + 1;

  LocStatistics[PercentageKey]++;
  CumulNumOfVars++;
}

static void collectStatsRecursive(DWARFDie Die, uint64_t ScopeLowPC,
                                  uint64_t BytesInScope,
                                  std::map<int, unsigned long> &LocStatistics,
                                  unsigned &CumulNumOfVars,
                                  double &TotalAverage,
                                  DWARFContext &DICtx) {
  const dwarf::Tag Tag = Die.getTag();
  const bool IsFunction = Tag == dwarf::DW_TAG_subprogram;
  const bool IsBlock = Tag == dwarf::DW_TAG_lexical_block;
  // TODO: Add a separate option to track inlined functions.
  const bool IsInlinedFunction = Tag == dwarf::DW_TAG_inlined_subroutine;
  if (IsFunction || IsInlinedFunction || IsBlock) {
    if (auto name = Die.getName(DINameKind::ShortName))
    LLVM_DEBUG(llvm::dbgs() << "The function beeing processed is: "
                            << name<< "\n");

    // Ignore forward declarations.
    if (Die.find(dwarf::DW_AT_declaration)) {
      LLVM_DEBUG(llvm::dbgs() << "  -declaration ignored\n");
      return;
    }

    // Ignore inlined subprograms.
    if (Die.find(dwarf::DW_AT_inline)) {
      LLVM_DEBUG(llvm::dbgs() << "  -inlined subprogram ignored\n");
      return;
    }

    if (IgnoreInlined && IsInlinedFunction) {
      LLVM_DEBUG(llvm::dbgs() << "  -an inlined instance ignored\n");
      return;
    }

    // PC Ranges.
    auto RangesOrError = Die.getAddressRanges();
    if (!RangesOrError) {
      llvm::consumeError(RangesOrError.takeError());
      return;
    }

    auto Ranges = RangesOrError.get();
    uint64_t BytesInThisScope = 0;
    for (auto Range : Ranges)
      BytesInThisScope += Range.HighPC - Range.LowPC;
    ScopeLowPC = getLowPC(Die);

    LLVM_DEBUG(llvm::dbgs() << "  -the coverage: " << BytesInThisScope
                            << " (bytes)\n");
    BytesInScope = BytesInThisScope;
  } else if (Die.getTag() == dwarf::DW_TAG_variable ||
             Die.getTag() == dwarf::DW_TAG_formal_parameter) {
    collectLocStatsForDie(Die, ScopeLowPC, BytesInScope, LocStatistics,
                          CumulNumOfVars, TotalAverage, DICtx);
  }

  // Traverse children.
  DWARFDie Child = Die.getFirstChild();
  while (Child) {
    collectStatsRecursive(Child, ScopeLowPC, BytesInScope, LocStatistics,
                          CumulNumOfVars, TotalAverage, DICtx);
    Child = Child.getSibling();
  }
}

static void outputLocStats(std::map<int, unsigned long> &LocStatistics,
                           unsigned &CumulNumOfVars, double &TotalAverage,
                           raw_ostream &OS) {
  if (CumulNumOfVars == 0) {
    OS << "No coverage recorded.\n";
    return;
  }

  OS << "=================================================\n";
  OS << "           Debug Location Statistics\n";
  OS << "=================================================\n";
  OS << "    cov%        samples        percentage\n";
  OS << "-------------------------------------------------\n";
  OS << "    0"
     << "         " << format_decimal(LocStatistics[0], 8) << "        "
     << format_decimal((int)(LocStatistics[0] / (double)CumulNumOfVars * 100),
                       8)
     << "%\n";
  OS << "    1..9"
     << "      " << format_decimal(LocStatistics[1], 8) << "        "
     << format_decimal((int)(LocStatistics[1] / (double)CumulNumOfVars * 100),
                       8)
     << "%\n";
  for (unsigned i = 2; i < 11; ++i)
    OS << "    " << (i - 1) * 10 + 1 << ".." << i * 10 - 1 << "    "
       << format_decimal(LocStatistics[i], 8) << "        "
       << format_decimal((int)(LocStatistics[i] / (double)CumulNumOfVars * 100),
                         8)
       << "%\n";
  OS << "    100"
     << "       " << format_decimal(LocStatistics[11], 8) << "        "
     << format_decimal((int)(LocStatistics[11] / (double)CumulNumOfVars * 100),
                       8)
     << "%\n";
  OS << "=================================================\n";
  OS << "-the number of debug variables processed: " << CumulNumOfVars << "\n";
  OS << "-the average coverage per var: ~ "
     << (int)std::round((TotalAverage/CumulNumOfVars * 100) / 100) << "%\n";
  OS << "=================================================\n";
}

static void collectLocstats(ObjectFile &Obj, DWARFContext &DICtx,
                            Twine Filename, raw_ostream &OS) {
  // Map percentage->occurrences.
  std::map<int, unsigned long> LocStatistics;
  for (int i = 0; i < largest_cov_category; ++i)
    LocStatistics[i] = 0;

  unsigned CumulNumOfVars = 0;
  double TotalAverage = 0.0;
  
  for (const auto &CU : static_cast<DWARFContext *>(&DICtx)->compile_units())
    if (DWARFDie CUDie = CU->getNonSkeletonUnitDIE(false))
      collectStatsRecursive(CUDie, 0, 0, LocStatistics, CumulNumOfVars,
                            TotalAverage, DICtx);

  // Output the results.
  outputLocStats(LocStatistics, CumulNumOfVars, TotalAverage, OS);
}

static void error(StringRef Prefix, std::error_code EC) {
  if (!EC)
    return;
  WithColor::error() << Prefix << ": " << EC.message() << "\n";
  exit(1);
}

static void handleBuffer(StringRef Filename, MemoryBufferRef Buffer,
                         HandlerFn HandleObj, raw_ostream &OS) {
  Expected<std::unique_ptr<Binary>> BinOrErr = object::createBinary(Buffer);
  error(Filename, errorToErrorCode(BinOrErr.takeError()));

  if (auto *Obj = dyn_cast<ObjectFile>(BinOrErr->get())) {
    std::unique_ptr<DWARFContext> DICtx = DWARFContext::create(*Obj);
    HandleObj(*Obj, *DICtx, Filename, OS);
  }
}

static void handleFile(StringRef Filename, HandlerFn HandleObj,
                       raw_ostream &OS) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> BuffOrErr =
  MemoryBuffer::getFileOrSTDIN(Filename);
  error(Filename, BuffOrErr.getError());
  std::unique_ptr<MemoryBuffer> Buffer = std::move(BuffOrErr.get());
  handleBuffer(Filename, *Buffer, HandleObj, OS);
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();

  HideUnrelatedOptions({&LocStatsCategory});
  cl::ParseCommandLineOptions(
      argc, argv,
      "calculate debug location coverage on an input file.\n");

  if (Help) {
    PrintHelpMessage(false, true);
    return 0;
  }

  if (InputFilename == "") {
    WithColor::error(errs()) << "no input file\n";
    exit(1);
  }

  if (OnlyFormalParameters && OnlyVariables) {
    WithColor::error() << "incompatible arguments: specifying both "
                          "-only-formal-parameters and -only-variables is "
                          "not allowed.";
    return 0;
  }

  std::error_code EC;
  ToolOutputFile OutputFile(OutputFilename, EC, sys::fs::OF_None);
  error("Unable to open output file" + OutputFilename, EC);
  // Don't remove output file if we exit with an error.
  OutputFile.keep();

  handleFile(InputFilename, collectLocstats, OutputFile.os());
 
  return EXIT_SUCCESS;
}
