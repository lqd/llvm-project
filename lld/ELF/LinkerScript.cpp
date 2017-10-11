//===- LinkerScript.cpp ---------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the parser/evaluator of the linker script.
//
//===----------------------------------------------------------------------===//

#include "LinkerScript.h"
#include "Config.h"
#include "InputSection.h"
#include "Memory.h"
#include "OutputSections.h"
#include "Strings.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "Threads.h"
#include "Writer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace lld;
using namespace lld::elf;

LinkerScript *elf::Script;

static uint64_t getOutputSectionVA(SectionBase *InputSec, StringRef Loc) {
  if (OutputSection *OS = InputSec->getOutputSection())
    return OS->Addr;
  error(Loc + ": unable to evaluate expression: input section " +
        InputSec->Name + " has no output section assigned");
  return 0;
}

uint64_t ExprValue::getValue() const {
  if (Sec)
    return alignTo(Sec->getOffset(Val) + getOutputSectionVA(Sec, Loc),
                   Alignment);
  return alignTo(Val, Alignment);
}

uint64_t ExprValue::getSecAddr() const {
  if (Sec)
    return Sec->getOffset(0) + getOutputSectionVA(Sec, Loc);
  return 0;
}

uint64_t ExprValue::getSectionOffset() const {
  // If the alignment is trivial, we don't have to compute the full
  // value to know the offset. This allows this function to succeed in
  // cases where the output section is not yet known.
  if (Alignment == 1)
    return Val;
  return getValue() - getSecAddr();
}

OutputSection *LinkerScript::createOutputSection(StringRef Name,
                                                 StringRef Location) {
  OutputSection *&SecRef = NameToOutputSection[Name];
  OutputSection *Sec;
  if (SecRef && SecRef->Location.empty()) {
    // There was a forward reference.
    Sec = SecRef;
  } else {
    Sec = make<OutputSection>(Name, SHT_PROGBITS, 0);
    if (!SecRef)
      SecRef = Sec;
  }
  Sec->Location = Location;
  return Sec;
}

OutputSection *LinkerScript::getOrCreateOutputSection(StringRef Name) {
  OutputSection *&CmdRef = NameToOutputSection[Name];
  if (!CmdRef)
    CmdRef = make<OutputSection>(Name, SHT_PROGBITS, 0);
  return CmdRef;
}

void LinkerScript::setDot(Expr E, const Twine &Loc, bool InSec) {
  uint64_t Val = E().getValue();
  if (Val < Dot && InSec)
    error(Loc + ": unable to move location counter backward for: " +
          CurAddressState->OutSec->Name);
  Dot = Val;

  // Update to location counter means update to section size.
  if (InSec)
    CurAddressState->OutSec->Size = Dot - CurAddressState->OutSec->Addr;
}

// This function is called from processSectionCommands,
// while we are fixing the output section layout.
void LinkerScript::addSymbol(SymbolAssignment *Cmd) {
  if (Cmd->Name == ".")
    return;

  // If a symbol was in PROVIDE(), we need to define it only when
  // it is a referenced undefined symbol.
  SymbolBody *B = Symtab->find(Cmd->Name);
  if (Cmd->Provide && (!B || B->isDefined()))
    return;

  // Define a symbol.
  Symbol *Sym;
  uint8_t Visibility = Cmd->Hidden ? STV_HIDDEN : STV_DEFAULT;
  std::tie(Sym, std::ignore) = Symtab->insert(Cmd->Name, /*Type*/ 0, Visibility,
                                              /*CanOmitFromDynSym*/ false,
                                              /*File*/ nullptr);
  Sym->Binding = STB_GLOBAL;
  ExprValue Value = Cmd->Expression();
  SectionBase *Sec = Value.isAbsolute() ? nullptr : Value.Sec;

  // When this function is called, section addresses have not been
  // fixed yet. So, we may or may not know the value of the RHS
  // expression.
  //
  // For example, if an expression is `x = 42`, we know x is always 42.
  // However, if an expression is `x = .`, there's no way to know its
  // value at the moment.
  //
  // We want to set symbol values early if we can. This allows us to
  // use symbols as variables in linker scripts. Doing so allows us to
  // write expressions like this: `alignment = 16; . = ALIGN(., alignment)`.
  uint64_t SymValue = Value.Sec ? 0 : Value.getValue();

  replaceBody<DefinedRegular>(Sym, nullptr, Cmd->Name, /*IsLocal=*/false,
                              Visibility, STT_NOTYPE, SymValue, 0, Sec);
  Cmd->Sym = cast<DefinedRegular>(Sym->body());
}

// This function is called from assignAddresses, while we are
// fixing the output section addresses. This function is supposed
// to set the final value for a given symbol assignment.
void LinkerScript::assignSymbol(SymbolAssignment *Cmd, bool InSec) {
  if (Cmd->Name == ".") {
    setDot(Cmd->Expression, Cmd->Location, InSec);
    return;
  }

  if (!Cmd->Sym)
    return;

  ExprValue V = Cmd->Expression();
  if (V.isAbsolute()) {
    Cmd->Sym->Section = nullptr;
    Cmd->Sym->Value = V.getValue();
  } else {
    Cmd->Sym->Section = V.Sec;
    Cmd->Sym->Value = V.getSectionOffset();
  }
}

static std::string filename(InputFile *File) {
  if (!File)
    return "";
  if (File->ArchiveName.empty())
    return File->getName();
  return (File->ArchiveName + "(" + File->getName() + ")").str();
}

bool LinkerScript::shouldKeep(InputSectionBase *S) {
  std::string Filename = filename(S->File);
  for (InputSectionDescription *ID : KeptSections)
    if (ID->FilePat.match(Filename))
      for (SectionPattern &P : ID->SectionPatterns)
        if (P.SectionPat.match(S->Name))
          return true;
  return false;
}

// A helper function for the SORT() command.
static std::function<bool(InputSectionBase *, InputSectionBase *)>
getComparator(SortSectionPolicy K) {
  switch (K) {
  case SortSectionPolicy::Alignment:
    return [](InputSectionBase *A, InputSectionBase *B) {
      // ">" is not a mistake. Sections with larger alignments are placed
      // before sections with smaller alignments in order to reduce the
      // amount of padding necessary. This is compatible with GNU.
      return A->Alignment > B->Alignment;
    };
  case SortSectionPolicy::Name:
    return [](InputSectionBase *A, InputSectionBase *B) {
      return A->Name < B->Name;
    };
  case SortSectionPolicy::Priority:
    return [](InputSectionBase *A, InputSectionBase *B) {
      return getPriority(A->Name) < getPriority(B->Name);
    };
  default:
    llvm_unreachable("unknown sort policy");
  }
}

// A helper function for the SORT() command.
static bool matchConstraints(ArrayRef<InputSectionBase *> Sections,
                             ConstraintKind Kind) {
  if (Kind == ConstraintKind::NoConstraint)
    return true;

  bool IsRW = llvm::any_of(Sections, [](InputSectionBase *Sec) {
    return static_cast<InputSectionBase *>(Sec)->Flags & SHF_WRITE;
  });

  return (IsRW && Kind == ConstraintKind::ReadWrite) ||
         (!IsRW && Kind == ConstraintKind::ReadOnly);
}

static void sortSections(InputSection **Begin, InputSection **End,
                         SortSectionPolicy K) {
  if (K != SortSectionPolicy::Default && K != SortSectionPolicy::None)
    std::stable_sort(Begin, End, getComparator(K));
}

static void sortBySymbolOrder(InputSection **Begin, InputSection **End) {
  if (Config->SymbolOrderingFile.empty())
    return;
  static llvm::DenseMap<SectionBase *, int> Order = buildSectionOrder();
  MutableArrayRef<InputSection *> In(Begin, End - Begin);
  sortByOrder(In, [&](InputSectionBase *S) { return Order.lookup(S); });
}

// Compute and remember which sections the InputSectionDescription matches.
std::vector<InputSection *>
LinkerScript::computeInputSections(const InputSectionDescription *Cmd) {
  std::vector<InputSection *> Ret;

  // Collects all sections that satisfy constraints of Cmd.
  for (const SectionPattern &Pat : Cmd->SectionPatterns) {
    size_t SizeBefore = Ret.size();

    for (InputSectionBase *Sec : InputSections) {
      if (Sec->Assigned)
        continue;

      if (!Sec->Live) {
        reportDiscarded(Sec);
        continue;
      }

      // For -emit-relocs we have to ignore entries like
      //   .rela.dyn : { *(.rela.data) }
      // which are common because they are in the default bfd script.
      if (Sec->Type == SHT_REL || Sec->Type == SHT_RELA)
        continue;

      std::string Filename = filename(Sec->File);
      if (!Cmd->FilePat.match(Filename) ||
          Pat.ExcludedFilePat.match(Filename) ||
          !Pat.SectionPat.match(Sec->Name))
        continue;

      Ret.push_back(cast<InputSection>(Sec));
      Sec->Assigned = true;
    }

    // Sort sections as instructed by SORT-family commands and --sort-section
    // option. Because SORT-family commands can be nested at most two depth
    // (e.g. SORT_BY_NAME(SORT_BY_ALIGNMENT(.text.*))) and because the command
    // line option is respected even if a SORT command is given, the exact
    // behavior we have here is a bit complicated. Here are the rules.
    //
    // 1. If two SORT commands are given, --sort-section is ignored.
    // 2. If one SORT command is given, and if it is not SORT_NONE,
    //    --sort-section is handled as an inner SORT command.
    // 3. If one SORT command is given, and if it is SORT_NONE, don't sort.
    // 4. If no SORT command is given, sort according to --sort-section.
    // 5. If no SORT commands are given and --sort-section is not specified,
    //    apply sorting provided by --symbol-ordering-file if any exist.
    InputSection **Begin = Ret.data() + SizeBefore;
    InputSection **End = Ret.data() + Ret.size();
    if (Pat.SortOuter == SortSectionPolicy::Default &&
        Config->SortSection == SortSectionPolicy::Default) {
      sortBySymbolOrder(Begin, End);
      continue;
    }
    if (Pat.SortOuter != SortSectionPolicy::None) {
      if (Pat.SortInner == SortSectionPolicy::Default)
        sortSections(Begin, End, Config->SortSection);
      else
        sortSections(Begin, End, Pat.SortInner);
      sortSections(Begin, End, Pat.SortOuter);
    }
  }
  return Ret;
}

void LinkerScript::discard(ArrayRef<InputSectionBase *> V) {
  for (InputSectionBase *S : V) {
    S->Live = false;
    if (S == InX::ShStrTab || S == InX::Dynamic || S == InX::DynSymTab ||
        S == InX::DynStrTab)
      error("discarding " + S->Name + " section is not allowed");
    discard(S->DependentSections);
  }
}

std::vector<InputSectionBase *>
LinkerScript::createInputSectionList(OutputSection &OutCmd) {
  std::vector<InputSectionBase *> Ret;

  for (BaseCommand *Base : OutCmd.SectionCommands) {
    auto *Cmd = dyn_cast<InputSectionDescription>(Base);
    if (!Cmd)
      continue;

    Cmd->Sections = computeInputSections(Cmd);
    Ret.insert(Ret.end(), Cmd->Sections.begin(), Cmd->Sections.end());
  }

  return Ret;
}

void LinkerScript::processSectionCommands(OutputSectionFactory &Factory) {
  // A symbol can be assigned before any section is mentioned in the linker
  // script. In an DSO, the symbol values are addresses, so the only important
  // section values are:
  // * SHN_UNDEF
  // * SHN_ABS
  // * Any value meaning a regular section.
  // To handle that, create a dummy aether section that fills the void before
  // the linker scripts switches to another section. It has an index of one
  // which will map to whatever the first actual section is.
  Aether = make<OutputSection>("", 0, SHF_ALLOC);
  Aether->SectionIndex = 1;
  auto State = make_unique<AddressState>();

  // CurAddressState captures the local AddressState and makes it accessible
  // deliberately. This is needed as there are some cases where we cannot just
  // thread the current state through to a lambda function created by the
  // script parser.
  CurAddressState = State.get();
  CurAddressState->OutSec = Aether;

  for (size_t I = 0; I < SectionCommands.size(); ++I) {
    // Handle symbol assignments outside of any output section.
    if (auto *Cmd = dyn_cast<SymbolAssignment>(SectionCommands[I])) {
      addSymbol(Cmd);
      continue;
    }

    if (auto *Sec = dyn_cast<OutputSection>(SectionCommands[I])) {
      std::vector<InputSectionBase *> V = createInputSectionList(*Sec);

      // The output section name `/DISCARD/' is special.
      // Any input section assigned to it is discarded.
      if (Sec->Name == "/DISCARD/") {
        discard(V);
        continue;
      }

      // This is for ONLY_IF_RO and ONLY_IF_RW. An output section directive
      // ".foo : ONLY_IF_R[OW] { ... }" is handled only if all member input
      // sections satisfy a given constraint. If not, a directive is handled
      // as if it wasn't present from the beginning.
      //
      // Because we'll iterate over SectionCommands many more times, the easiest
      // way to "make it as if it wasn't present" is to just remove it.
      if (!matchConstraints(V, Sec->Constraint)) {
        for (InputSectionBase *S : V)
          S->Assigned = false;
        SectionCommands.erase(SectionCommands.begin() + I);
        --I;
        continue;
      }

      // A directive may contain symbol definitions like this:
      // ".foo : { ...; bar = .; }". Handle them.
      for (BaseCommand *Base : Sec->SectionCommands)
        if (auto *OutCmd = dyn_cast<SymbolAssignment>(Base))
          addSymbol(OutCmd);

      // Handle subalign (e.g. ".foo : SUBALIGN(32) { ... }"). If subalign
      // is given, input sections are aligned to that value, whether the
      // given value is larger or smaller than the original section alignment.
      if (Sec->SubalignExpr) {
        uint32_t Subalign = Sec->SubalignExpr().getValue();
        for (InputSectionBase *S : V)
          S->Alignment = Subalign;
      }

      // Add input sections to an output section.
      for (InputSectionBase *S : V)
        Sec->addSection(cast<InputSection>(S));

      assert(Sec->SectionIndex == INT_MAX);
      Sec->SectionIndex = I;
      if (Sec->Noload)
        Sec->Type = SHT_NOBITS;
    }
  }
  CurAddressState = nullptr;
}

void LinkerScript::fabricateDefaultCommands() {
  // Define start address
  uint64_t StartAddr = UINT64_MAX;

  // The Sections with -T<section> have been sorted in order of ascending
  // address. We must lower StartAddr if the lowest -T<section address> as
  // calls to setDot() must be monotonically increasing.
  for (auto &KV : Config->SectionStartMap)
    StartAddr = std::min(StartAddr, KV.second);

  auto Expr = [=] {
    return std::min(StartAddr, Target->getImageBase() + elf::getHeaderSize());
  };
  SectionCommands.insert(SectionCommands.begin(),
                         make<SymbolAssignment>(".", Expr, ""));
}

static OutputSection *findByName(ArrayRef<BaseCommand *> Vec,
                                 StringRef Name) {
  for (BaseCommand *Base : Vec)
    if (auto *Sec = dyn_cast<OutputSection>(Base))
      if (Sec->Name == Name)
        return Sec;
  return nullptr;
}

// Add sections that didn't match any sections command.
void LinkerScript::addOrphanSections(OutputSectionFactory &Factory) {
  unsigned End = SectionCommands.size();

  for (InputSectionBase *S : InputSections) {
    if (!S->Live || S->Parent)
      continue;

    StringRef Name = getOutputSectionName(S->Name);
    log(toString(S) + " is being placed in '" + Name + "'");

    if (OutputSection *Sec =
            findByName(makeArrayRef(SectionCommands).slice(0, End), Name)) {
      Sec->addSection(cast<InputSection>(S));
      continue;
    }

    if (OutputSection *OS = Factory.addInputSec(S, Name))
      SectionCommands.push_back(OS);
    assert(S->getOutputSection()->SectionIndex == INT_MAX);
  }
}

uint64_t LinkerScript::advance(uint64_t Size, unsigned Align) {
  bool IsTbss = (CurAddressState->OutSec->Flags & SHF_TLS) &&
                CurAddressState->OutSec->Type == SHT_NOBITS;
  uint64_t Start = IsTbss ? Dot + CurAddressState->ThreadBssOffset : Dot;
  Start = alignTo(Start, Align);
  uint64_t End = Start + Size;

  if (IsTbss)
    CurAddressState->ThreadBssOffset = End - Dot;
  else
    Dot = End;
  return End;
}

void LinkerScript::output(InputSection *S) {
  uint64_t Before = advance(0, 1);
  uint64_t Pos = advance(S->getSize(), S->Alignment);
  S->OutSecOff = Pos - S->getSize() - CurAddressState->OutSec->Addr;

  // Update output section size after adding each section. This is so that
  // SIZEOF works correctly in the case below:
  // .foo { *(.aaa) a = SIZEOF(.foo); *(.bbb) }
  CurAddressState->OutSec->Size = Pos - CurAddressState->OutSec->Addr;

  // If there is a memory region associated with this input section, then
  // place the section in that region and update the region index.
  if (CurAddressState->MemRegion) {
    uint64_t &CurOffset =
        CurAddressState->MemRegionOffset[CurAddressState->MemRegion];
    CurOffset += Pos - Before;
    uint64_t CurSize = CurOffset - CurAddressState->MemRegion->Origin;
    if (CurSize > CurAddressState->MemRegion->Length) {
      uint64_t OverflowAmt = CurSize - CurAddressState->MemRegion->Length;
      error("section '" + CurAddressState->OutSec->Name +
            "' will not fit in region '" + CurAddressState->MemRegion->Name +
            "': overflowed by " + Twine(OverflowAmt) + " bytes");
    }
  }
}

void LinkerScript::switchTo(OutputSection *Sec) {
  if (CurAddressState->OutSec == Sec)
    return;

  CurAddressState->OutSec = Sec;
  CurAddressState->OutSec->Addr =
      advance(0, CurAddressState->OutSec->Alignment);

  // If neither AT nor AT> is specified for an allocatable section, the linker
  // will set the LMA such that the difference between VMA and LMA for the
  // section is the same as the preceding output section in the same region
  // https://sourceware.org/binutils/docs-2.20/ld/Output-Section-LMA.html
  if (CurAddressState->LMAOffset)
    CurAddressState->OutSec->LMAOffset = CurAddressState->LMAOffset();
}

void LinkerScript::process(BaseCommand &Base) {
  // This handles the assignments to symbol or to the dot.
  if (auto *Cmd = dyn_cast<SymbolAssignment>(&Base)) {
    assignSymbol(Cmd, true);
    return;
  }

  // Handle BYTE(), SHORT(), LONG(), or QUAD().
  if (auto *Cmd = dyn_cast<BytesDataCommand>(&Base)) {
    Cmd->Offset = Dot - CurAddressState->OutSec->Addr;
    Dot += Cmd->Size;
    CurAddressState->OutSec->Size = Dot - CurAddressState->OutSec->Addr;
    return;
  }

  // Handle ASSERT().
  if (auto *Cmd = dyn_cast<AssertCommand>(&Base)) {
    Cmd->Expression();
    return;
  }

  // Handle a single input section description command.
  // It calculates and assigns the offsets for each section and also
  // updates the output section size.
  auto &Cmd = cast<InputSectionDescription>(Base);
  for (InputSection *Sec : Cmd.Sections) {
    // We tentatively added all synthetic sections at the beginning and removed
    // empty ones afterwards (because there is no way to know whether they were
    // going be empty or not other than actually running linker scripts.)
    // We need to ignore remains of empty sections.
    if (auto *S = dyn_cast<SyntheticSection>(Sec))
      if (S->empty())
        continue;

    if (!Sec->Live)
      continue;
    assert(CurAddressState->OutSec == Sec->getParent());
    output(Sec);
  }
}

// This function searches for a memory region to place the given output
// section in. If found, a pointer to the appropriate memory region is
// returned. Otherwise, a nullptr is returned.
MemoryRegion *LinkerScript::findMemoryRegion(OutputSection *Sec) {
  // If a memory region name was specified in the output section command,
  // then try to find that region first.
  if (!Sec->MemoryRegionName.empty()) {
    auto It = MemoryRegions.find(Sec->MemoryRegionName);
    if (It != MemoryRegions.end())
      return It->second;
    error("memory region '" + Sec->MemoryRegionName + "' not declared");
    return nullptr;
  }

  // If at least one memory region is defined, all sections must
  // belong to some memory region. Otherwise, we don't need to do
  // anything for memory regions.
  if (MemoryRegions.empty())
    return nullptr;

  // See if a region can be found by matching section flags.
  for (auto &Pair : MemoryRegions) {
    MemoryRegion *M = Pair.second;
    if ((M->Flags & Sec->Flags) && (M->NegFlags & Sec->Flags) == 0)
      return M;
  }

  // Otherwise, no suitable region was found.
  if (Sec->Flags & SHF_ALLOC)
    error("no memory region specified for section '" + Sec->Name + "'");
  return nullptr;
}

// This function assigns offsets to input sections and an output section
// for a single sections command (e.g. ".text { *(.text); }").
void LinkerScript::assignOffsets(OutputSection *Sec) {
  if (!(Sec->Flags & SHF_ALLOC))
    Dot = 0;
  else if (Sec->AddrExpr)
    setDot(Sec->AddrExpr, Sec->Location, false);

  CurAddressState->MemRegion = Sec->MemRegion;
  if (CurAddressState->MemRegion)
    Dot = CurAddressState->MemRegionOffset[CurAddressState->MemRegion];

  if (Sec->LMAExpr) {
    uint64_t D = Dot;
    CurAddressState->LMAOffset = [=] { return Sec->LMAExpr().getValue() - D; };
  }

  switchTo(Sec);

  // We do not support custom layout for compressed debug sectons.
  // At this point we already know their size and have compressed content.
  if (CurAddressState->OutSec->Flags & SHF_COMPRESSED)
    return;

  for (BaseCommand *C : Sec->SectionCommands)
    process(*C);
}

void LinkerScript::removeEmptyCommands() {
  // It is common practice to use very generic linker scripts. So for any
  // given run some of the output sections in the script will be empty.
  // We could create corresponding empty output sections, but that would
  // clutter the output.
  // We instead remove trivially empty sections. The bfd linker seems even
  // more aggressive at removing them.
  llvm::erase_if(SectionCommands, [&](BaseCommand *Base) {
    if (auto *Sec = dyn_cast<OutputSection>(Base))
      return !Sec->Live;
    return false;
  });
}

static bool isAllSectionDescription(const OutputSection &Cmd) {
  for (BaseCommand *Base : Cmd.SectionCommands)
    if (!isa<InputSectionDescription>(*Base))
      return false;
  return true;
}

void LinkerScript::adjustSectionsBeforeSorting() {
  // If the output section contains only symbol assignments, create a
  // corresponding output section. The bfd linker seems to only create them if
  // '.' is assigned to, but creating these section should not have any bad
  // consequeces and gives us a section to put the symbol in.
  uint64_t Flags = SHF_ALLOC;

  for (BaseCommand *Cmd : SectionCommands) {
    auto *Sec = dyn_cast<OutputSection>(Cmd);
    if (!Sec)
      continue;
    if (Sec->Live) {
      Flags = Sec->Flags;
      continue;
    }

    if (isAllSectionDescription(*Sec))
      continue;

    Sec->Live = true;
    Sec->Flags = Flags;
  }
}

void LinkerScript::adjustSectionsAfterSorting() {
  // Try and find an appropriate memory region to assign offsets in.
  for (BaseCommand *Base : SectionCommands) {
    if (auto *Sec = dyn_cast<OutputSection>(Base)) {
      if (!Sec->Live)
        continue;
      Sec->MemRegion = findMemoryRegion(Sec);
      // Handle align (e.g. ".foo : ALIGN(16) { ... }").
      if (Sec->AlignExpr)
        Sec->Alignment =
            std::max<uint32_t>(Sec->Alignment, Sec->AlignExpr().getValue());
    }
  }

  // If output section command doesn't specify any segments,
  // and we haven't previously assigned any section to segment,
  // then we simply assign section to the very first load segment.
  // Below is an example of such linker script:
  // PHDRS { seg PT_LOAD; }
  // SECTIONS { .aaa : { *(.aaa) } }
  std::vector<StringRef> DefPhdrs;
  auto FirstPtLoad =
      std::find_if(PhdrsCommands.begin(), PhdrsCommands.end(),
                   [](const PhdrsCommand &Cmd) { return Cmd.Type == PT_LOAD; });
  if (FirstPtLoad != PhdrsCommands.end())
    DefPhdrs.push_back(FirstPtLoad->Name);

  // Walk the commands and propagate the program headers to commands that don't
  // explicitly specify them.
  for (BaseCommand *Base : SectionCommands) {
    auto *Sec = dyn_cast<OutputSection>(Base);
    if (!Sec)
      continue;

    if (Sec->Phdrs.empty()) {
      // To match the bfd linker script behaviour, only propagate program
      // headers to sections that are allocated.
      if (Sec->Flags & SHF_ALLOC)
        Sec->Phdrs = DefPhdrs;
    } else {
      DefPhdrs = Sec->Phdrs;
    }
  }
}

static OutputSection *findFirstSection(PhdrEntry *Load) {
  for (OutputSection *Sec : OutputSections)
    if (Sec->PtLoad == Load)
      return Sec;
  return nullptr;
}

// Try to find an address for the file and program headers output sections,
// which were unconditionally added to the first PT_LOAD segment earlier.
//
// When using the default layout, we check if the headers fit below the first
// allocated section. When using a linker script, we also check if the headers
// are covered by the output section. This allows omitting the headers by not
// leaving enough space for them in the linker script; this pattern is common
// in embedded systems.
//
// If there isn't enough space for these sections, we'll remove them from the
// PT_LOAD segment, and we'll also remove the PT_PHDR segment.
void LinkerScript::allocateHeaders(std::vector<PhdrEntry *> &Phdrs) {
  uint64_t Min = std::numeric_limits<uint64_t>::max();
  for (OutputSection *Sec : OutputSections)
    if (Sec->Flags & SHF_ALLOC)
      Min = std::min<uint64_t>(Min, Sec->Addr);

  auto It = llvm::find_if(
      Phdrs, [](const PhdrEntry *E) { return E->p_type == PT_LOAD; });
  if (It == Phdrs.end())
    return;
  PhdrEntry *FirstPTLoad = *It;

  uint64_t HeaderSize = getHeaderSize();
  // When linker script with SECTIONS is being used, don't output headers
  // unless there's a space for them.
  uint64_t Base = HasSectionsCommand ? alignDown(Min, Config->MaxPageSize) : 0;
  if (HeaderSize <= Min - Base || Script->hasPhdrsCommands()) {
    Min = alignDown(Min - HeaderSize, Config->MaxPageSize);
    Out::ElfHeader->Addr = Min;
    Out::ProgramHeaders->Addr = Min + Out::ElfHeader->Size;
    return;
  }

  Out::ElfHeader->PtLoad = nullptr;
  Out::ProgramHeaders->PtLoad = nullptr;
  FirstPTLoad->FirstSec = findFirstSection(FirstPTLoad);

  llvm::erase_if(Phdrs,
                 [](const PhdrEntry *E) { return E->p_type == PT_PHDR; });
}

LinkerScript::AddressState::AddressState() {
  for (auto &MRI : Script->MemoryRegions) {
    const MemoryRegion *MR = MRI.second;
    MemRegionOffset[MR] = MR->Origin;
  }
}

// Assign addresses as instructed by linker script SECTIONS sub-commands.
void LinkerScript::assignAddresses() {
  // By default linker scripts use an initial value of 0 for '.', but prefer
  // -image-base if set.
  Dot = Config->ImageBase ? *Config->ImageBase : 0;
  auto State = make_unique<AddressState>();

  // CurAddressState captures the local AddressState and makes it accessible
  // deliberately. This is needed as there are some cases where we cannot just
  // thread the current state through to a lambda function created by the
  // script parser.
  CurAddressState = State.get();
  ErrorOnMissingSection = true;
  switchTo(Aether);

  for (BaseCommand *Base : SectionCommands) {
    if (auto *Cmd = dyn_cast<SymbolAssignment>(Base)) {
      assignSymbol(Cmd, false);
      continue;
    }

    if (auto *Cmd = dyn_cast<AssertCommand>(Base)) {
      Cmd->Expression();
      continue;
    }

    assignOffsets(cast<OutputSection>(Base));
  }
  CurAddressState = nullptr;
}

// Creates program headers as instructed by PHDRS linker script command.
std::vector<PhdrEntry *> LinkerScript::createPhdrs() {
  std::vector<PhdrEntry *> Ret;

  // Process PHDRS and FILEHDR keywords because they are not
  // real output sections and cannot be added in the following loop.
  for (const PhdrsCommand &Cmd : PhdrsCommands) {
    PhdrEntry *Phdr = make<PhdrEntry>(Cmd.Type, Cmd.Flags ? *Cmd.Flags : PF_R);

    if (Cmd.HasFilehdr)
      Phdr->add(Out::ElfHeader);
    if (Cmd.HasPhdrs)
      Phdr->add(Out::ProgramHeaders);

    if (Cmd.LMAExpr) {
      Phdr->p_paddr = Cmd.LMAExpr().getValue();
      Phdr->HasLMA = true;
    }
    Ret.push_back(Phdr);
  }

  // Add output sections to program headers.
  for (OutputSection *Sec : OutputSections) {
    // Assign headers specified by linker script
    for (size_t Id : getPhdrIndices(Sec)) {
      Ret[Id]->add(Sec);
      if (!PhdrsCommands[Id].Flags.hasValue())
        Ret[Id]->p_flags |= Sec->getPhdrFlags();
    }
  }
  return Ret;
}

// Returns true if we should emit an .interp section.
//
// We usually do. But if PHDRS commands are given, and
// no PT_INTERP is there, there's no place to emit an
// .interp, so we don't do that in that case.
bool LinkerScript::needsInterpSection() {
  if (PhdrsCommands.empty())
    return true;
  for (PhdrsCommand &Cmd : PhdrsCommands)
    if (Cmd.Type == PT_INTERP)
      return true;
  return false;
}

ExprValue LinkerScript::getSymbolValue(const Twine &Loc, StringRef S) {
  if (S == ".") {
    if (CurAddressState)
      return {CurAddressState->OutSec, false,
              Dot - CurAddressState->OutSec->Addr, Loc};
    error(Loc + ": unable to get location counter value");
    return 0;
  }
  if (SymbolBody *B = Symtab->find(S)) {
    if (auto *D = dyn_cast<DefinedRegular>(B))
      return {D->Section, false, D->Value, Loc};
    if (auto *C = dyn_cast<DefinedCommon>(B))
      return {C->Section, false, 0, Loc};
  }
  error(Loc + ": symbol not found: " + S);
  return 0;
}

// Returns the index of the segment named Name.
static Optional<size_t> getPhdrIndex(ArrayRef<PhdrsCommand> Vec,
                                     StringRef Name) {
  for (size_t I = 0; I < Vec.size(); ++I)
    if (Vec[I].Name == Name)
      return I;
  return None;
}

// Returns indices of ELF headers containing specific section. Each index is a
// zero based number of ELF header listed within PHDRS {} script block.
std::vector<size_t> LinkerScript::getPhdrIndices(OutputSection *Cmd) {
  std::vector<size_t> Ret;

  for (StringRef S : Cmd->Phdrs) {
    if (Optional<size_t> Idx = getPhdrIndex(PhdrsCommands, S))
      Ret.push_back(*Idx);
    else if (S != "NONE")
      error(Cmd->Location + ": section header '" + S +
            "' is not listed in PHDRS");
  }
  return Ret;
}
