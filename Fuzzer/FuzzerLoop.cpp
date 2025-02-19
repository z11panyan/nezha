//===- FuzzerLoop.cpp - Fuzzer's main loop --------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Fuzzer's main loop.
//===----------------------------------------------------------------------===//

#include "FuzzerCorpus.h"
#include "FuzzerIO.h"
#include "FuzzerInternal.h"
#include "FuzzerMutate.h"
#include "FuzzerRandom.h"
#include "FuzzerShmem.h"
#include "FuzzerTracePC.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>

#if defined(__has_include)
#if __has_include(<sanitizer / lsan_interface.h>)
#include <sanitizer/lsan_interface.h>
#endif
#endif

#define NO_SANITIZE_MEMORY
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#undef NO_SANITIZE_MEMORY
#define NO_SANITIZE_MEMORY __attribute__((no_sanitize_memory))
#endif
#endif


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <iostream>
#include <fstream>
#include <ios>
#include <string>
#include <sstream>
#include <netdb.h>
#include <map>
#include <utility>
#include <vector>
#include <cmath>
#include "errno.h"
#include <time.h>

void log_save(const char *file, char *message)
{
  FILE* fp = NULL;
  fp = fopen(file, "a");
  fprintf(fp, "%s\n", message);
  fclose(fp);
}

void uint_to_uint8(uintptr_t* source, uint8_t* dest,size_t size)
{
	
	for (int i = 0; i < size; i++) {
	    dest[i * 8] = (uint8_t)source[i];
	    dest[i * 8 + 1] = (uint8_t)(source[i] >> 8);
	    dest[i * 8 + 2] = (uint8_t)(source[i] >> 16);
	    dest[i * 8 + 3] = (uint8_t)(source[i] >> 24);
	    dest[i * 8 + 4] = (uint8_t)(source[i] >> 32);
	    dest[i * 8 + 5] = (uint8_t)(source[i] >> 40);
	    dest[i * 8 + 6] = (uint8_t)(source[i] >> 48);
	    dest[i * 8 + 7] = (uint8_t)(source[i] >> 56);
	}
}


namespace fuzzer {
static const size_t kMaxUnitSizeToPrint = 256;

thread_local bool Fuzzer::IsMyThread;
thread_local bool Fuzzer::UnitHadOutputDiff;

SharedMemoryRegion SMR;

// Only one Fuzzer per process.
static Fuzzer *F;

// Leak detection is expensive, so we first check if there were more mallocs
// than frees (using the sanitizer malloc hooks) and only then try to call lsan.
struct MallocFreeTracer {
  void Start(int TraceLevel) {
    this->TraceLevel = TraceLevel;
    if (TraceLevel)
      Printf("MallocFreeTracer: START\n");
    Mallocs = 0;
    Frees = 0;
  }
  // Returns true if there were more mallocs than frees.
  bool Stop() {
    if (TraceLevel)
      Printf("MallocFreeTracer: STOP %zd %zd (%s)\n", Mallocs.load(),
             Frees.load(), Mallocs == Frees ? "same" : "DIFFERENT");
    bool Result = Mallocs > Frees;
    Mallocs = 0;
    Frees = 0;
    TraceLevel = 0;
    return Result;
  }
  std::atomic<size_t> Mallocs;
  std::atomic<size_t> Frees;
  int TraceLevel = 0;
};

static MallocFreeTracer AllocTracer;

ATTRIBUTE_NO_SANITIZE_MEMORY
void MallocHook(const volatile void *ptr, size_t size) {
  size_t N = AllocTracer.Mallocs++;
  F->HandleMalloc(size);
  if (int TraceLevel = AllocTracer.TraceLevel) {
    Printf("MALLOC[%zd] %p %zd\n", N, ptr, size);
    if (TraceLevel >= 2 && EF)
      EF->__sanitizer_print_stack_trace();
  }
}

ATTRIBUTE_NO_SANITIZE_MEMORY
void FreeHook(const volatile void *ptr) {
  size_t N = AllocTracer.Frees++;
  if (int TraceLevel = AllocTracer.TraceLevel) {
    Printf("FREE[%zd]   %p\n", N, ptr);
    if (TraceLevel >= 2 && EF)
      EF->__sanitizer_print_stack_trace();
  }
}

// Crash on a single malloc that exceeds the rss limit.
void Fuzzer::HandleMalloc(size_t Size) {
  if (!Options.RssLimitMb || (Size >> 20) < (size_t)Options.RssLimitMb)
    return;
  Printf("==%d== ERROR: libFuzzer: out-of-memory (malloc(%zd))\n", GetPid(),
         Size);
  Printf("   To change the out-of-memory limit use -rss_limit_mb=<N>\n\n");
  if (EF->__sanitizer_print_stack_trace)
    EF->__sanitizer_print_stack_trace();
  DumpCurrentUnit("oom-");
  Printf("SUMMARY: libFuzzer: out-of-memory\n");
  PrintFinalStats();
  _Exit(Options.ErrorExitCode); // Stop right now.
}

Fuzzer::Fuzzer(UserCallback CB, InputCorpus &Corpus, MutationDispatcher &MD,
               FuzzingOptions Options)
    : CB(CB), Corpus(Corpus), MD(MD), Options(Options) {
  if (EF->__sanitizer_set_death_callback)
    EF->__sanitizer_set_death_callback(StaticDeathCallback);
  assert(!F);
  F = this;
  TPC.ResetMaps();
  if (Options.DifferentialMode) TPC.InitializeDiffCallbacks(EF);
  IsMyThread = true;
  if (Options.DetectLeaks && EF->__sanitizer_install_malloc_and_free_hooks)
    EF->__sanitizer_install_malloc_and_free_hooks(MallocHook, FreeHook);
  TPC.SetUseCounters(Options.UseCounters);
  TPC.SetUseValueProfile(Options.UseValueProfile);
  TPC.SetPrintNewPCs(Options.PrintNewCovPcs);

  if (Options.Verbosity)
    TPC.PrintModuleInfo();
  if (!Options.OutputCorpus.empty() && Options.ReloadIntervalSec)
    EpochOfLastReadOfOutputCorpus = GetEpoch(Options.OutputCorpus);
  MaxInputLen = MaxMutationLen = Options.MaxLen;
  AllocateCurrentUnitData();
  CurrentUnitSize = 0;
  memset(BaseSha1, 0, sizeof(BaseSha1));
}

Fuzzer::~Fuzzer() { }

void Fuzzer::AllocateCurrentUnitData() {
  if (CurrentUnitData || MaxInputLen == 0) return;
  CurrentUnitData = new uint8_t[MaxInputLen];
}

void Fuzzer::StaticDeathCallback() {
  assert(F);
  F->DeathCallback();
}

void Fuzzer::DumpCurrentUnit(const char *Prefix) {
  if (!CurrentUnitData) return;  // Happens when running individual inputs.
  MD.PrintMutationSequence();
  Printf("; base unit: %s\n", Sha1ToString(BaseSha1).c_str());
  size_t UnitSize = CurrentUnitSize;
  if (UnitSize <= kMaxUnitSizeToPrint) {
    PrintHexArray(CurrentUnitData, UnitSize, "\n");
    PrintASCII(CurrentUnitData, UnitSize, "\n");
  }
  WriteUnitToFileWithPrefix({CurrentUnitData, CurrentUnitData + UnitSize},
                            Prefix);
}

void Fuzzer::DumpUnitIfDiff(const uint8_t *Data, size_t Size) {
  bool has_zero = false;
  bool has_nonzero = false;
  std::stringstream SS;
  for (size_t i = 0; i < TPC.OutputDiffVec.size(); ++i) {
    if (TPC.OutputDiffVec[i] == 0)
      has_zero = true;
    else
      has_nonzero = true;
    SS << TPC.OutputDiffVec[i] << "_";
  }

  if ( has_zero && has_nonzero) {
    int size = TPC.UC->size;
    int seq=0; 
    int index = 0;
    int pt = 0;
    uint8_t Mid[TPC.GetNumPCs()*8];
    uint8_t Coverage[TPC.GetNumPCs()*8];
    size_t CovSize=0;
    uint_to_uint8(TPC.PCs(),Mid,TPC.GetNumPCs());
    memset(Coverage,0,TPC.GetNumPCs()*8);
    for(int j = 0; j < size; j++)
    {
	index = index + 8*TPC.ModuleNum[j];
        pt = CovSize;
	if(TPC.OutputDiffVec[j]!=0)
	{
		memcpy(Coverage+pt,Mid+index,8*TPC.ModuleNum[j+1]);
		CovSize += 8*TPC.ModuleNum[j+1];
	}
    }
    
    
    uint8_t Hash[kSHA1NumBytes];
    ComputeSHA1(Coverage,CovSize,Hash);
    std::string tmp = Sha1ToString(Hash);
    if(CoverageHash.count(tmp)>0)
    {
	Duplicate++;
    }
    else
    {
	    CoverageHash[tmp] = true;
	    UnitHadOutputDiff = true;
	    NumberOfDiffUnitsAdded++;
	    WriteUnitToFileWithPrefix({Data, Data + Size},
		                      ("diff_" + SS.str()).c_str());
    }
  }
}

NO_SANITIZE_MEMORY
void Fuzzer::DeathCallback() {
  DumpCurrentUnit("crash-");
  PrintFinalStats();
}

void Fuzzer::StaticAlarmCallback() {
  assert(F);
  F->AlarmCallback();
}

void Fuzzer::StaticCrashSignalCallback() {
  assert(F);
  F->CrashCallback();
}

void Fuzzer::StaticInterruptCallback() {
  assert(F);
  F->InterruptCallback();
}

void Fuzzer::StaticFileSizeExceedCallback() {
  Printf("==%lu== ERROR: libFuzzer: file size exceeded\n", GetPid());
  exit(1);
}

void Fuzzer::CrashCallback() {
  Printf("==%lu== ERROR: libFuzzer: deadly signal\n", GetPid());
  if (EF->__sanitizer_print_stack_trace)
    EF->__sanitizer_print_stack_trace();
  Printf("NOTE: libFuzzer has rudimentary signal handlers.\n"
         "      Combine libFuzzer with AddressSanitizer or similar for better "
         "crash reports.\n");
  Printf("SUMMARY: libFuzzer: deadly signal\n");
  DumpCurrentUnit("crash-");
  PrintFinalStats();
  _Exit(Options.ErrorExitCode);  // Stop right now.
}

void Fuzzer::InterruptCallback() {
  Printf("==%lu== libFuzzer: run interrupted; exiting\n", GetPid());
  PrintFinalStats();
  _Exit(0);  // Stop right now, don't perform any at-exit actions.
}

NO_SANITIZE_MEMORY
void Fuzzer::AlarmCallback() {
  assert(Options.UnitTimeoutSec > 0);
  // In Windows Alarm callback is executed by a different thread.
#if !LIBFUZZER_WINDOWS
  if (!InFuzzingThread()) return;
#endif
  if (!RunningCB)
    return; // We have not started running units yet.
  size_t Seconds =
      duration_cast<seconds>(system_clock::now() - UnitStartTime).count();
  if (Seconds == 0)
    return;
  if (Options.Verbosity >= 2)
    Printf("AlarmCallback %zd\n", Seconds);
  if (Seconds >= (size_t)Options.UnitTimeoutSec) {
    Printf("ALARM: working on the last Unit for %zd seconds\n", Seconds);
    Printf("       and the timeout value is %d (use -timeout=N to change)\n",
           Options.UnitTimeoutSec);
    DumpCurrentUnit("timeout-");
    Printf("==%lu== ERROR: libFuzzer: timeout after %d seconds\n", GetPid(),
           Seconds);
    if (EF->__sanitizer_print_stack_trace)
      EF->__sanitizer_print_stack_trace();
    Printf("SUMMARY: libFuzzer: timeout\n");
    PrintFinalStats();
    _Exit(Options.TimeoutExitCode); // Stop right now.
  }
}

void Fuzzer::RssLimitCallback() {
  Printf(
      "==%lu== ERROR: libFuzzer: out-of-memory (used: %zdMb; limit: %zdMb)\n",
      GetPid(), GetPeakRSSMb(), Options.RssLimitMb);
  Printf("   To change the out-of-memory limit use -rss_limit_mb=<N>\n\n");
  if (EF->__sanitizer_print_memory_profile)
    EF->__sanitizer_print_memory_profile(95, 8);
  DumpCurrentUnit("oom-");
  Printf("SUMMARY: libFuzzer: out-of-memory\n");
  PrintFinalStats();
  _Exit(Options.ErrorExitCode); // Stop right now.
}

void Fuzzer::PrintStats(const char *Where, const char *End, size_t Units) {
  size_t ExecPerSec = execPerSec();
  if (!Options.Verbosity)
    return;
  Printf("#%zd\t%s", TotalNumberOfRuns, Where);
  char message[100];
  
  if (size_t N = TPC.GetTotalPCCoverage())
  {
    Printf(" cov: %zd", N);
  } 
  if (size_t N = Corpus.NumFeatures())
    Printf( " ft: %zd", N);
  if (!Corpus.empty()) {
    Printf(" corp: %zd", Corpus.NumActiveUnits());
    if (size_t N = Corpus.SizeInBytes()) {
      if (N < (1<<14))
        Printf("/%zdb", N);
      else if (N < (1 << 24))
        Printf("/%zdKb", N >> 10);
      else
        Printf("/%zdMb", N >> 20);
    }
  }
  if (Units)
    Printf(" units: %zd", Units);

  Printf(" exec/s: %zd", ExecPerSec);
  Printf(" rss: %zdMb", GetPeakRSSMb());
  Printf("%s", End);
}

void Fuzzer::PrintFinalStats() {
  if (Options.PrintCoverage)
    TPC.PrintCoverage();
  if (Options.DumpCoverage)
    TPC.DumpCoverage();
  if (Options.PrintCorpusStats)
    Corpus.PrintStats();
  if (!Options.PrintFinalStats) return;
  size_t ExecPerSec = execPerSec();
  Printf("stat::number_of_executed_units: %zd\n", TotalNumberOfRuns);
  Printf("stat::average_exec_per_sec:     %zd\n", ExecPerSec);
  Printf("stat::new_units_added:          %zd\n", NumberOfNewUnitsAdded);
  if  (Options.DifferentialMode)
    Printf("stat::number_of_diffs:          %zd\n", NumberOfDiffUnitsAdded);
  Printf("stat::slowest_unit_time_sec:    %zd\n", TimeOfLongestUnitInSeconds);
  Printf("stat::peak_rss_mb:              %zd\n", GetPeakRSSMb());
  Printf("stat::number_of_duplicates:	%zd\n", NumberOfDuplicate);
  Printf("stat::coverage:	%zd\n", TPC.GetTotalPCCoverage());
  Printf("stat::Duplicate:	%zd\n", Duplicate);
}

void Fuzzer::SetMaxInputLen(size_t MaxInputLen) {
  assert(this->MaxInputLen == 0); // Can only reset MaxInputLen from 0 to non-0.
  assert(MaxInputLen);
  this->MaxInputLen = MaxInputLen;
  this->MaxMutationLen = MaxInputLen;
  AllocateCurrentUnitData();
  Printf("INFO: -max_len is not provided; "
         "libFuzzer will not generate inputs larger than %zd bytes\n",
         MaxInputLen);
}

void Fuzzer::SetMaxMutationLen(size_t MaxMutationLen) {
  assert(MaxMutationLen && MaxMutationLen <= MaxInputLen);
  this->MaxMutationLen = MaxMutationLen;
}

void Fuzzer::CheckExitOnSrcPosOrItem() {
  if (!Options.ExitOnSrcPos.empty()) {
    static auto *PCsSet = new std::set<uintptr_t>;
    for (size_t i = 1, N = TPC.GetNumPCs(); i < N; i++) {
      uintptr_t PC = TPC.GetPC(i);
      if (!PC) continue;
      if (!PCsSet->insert(PC).second) continue;
      std::string Descr = DescribePC("%L", PC);
      if (Descr.find(Options.ExitOnSrcPos) != std::string::npos) {
        Printf("INFO: found line matching '%s', exiting.\n",
               Options.ExitOnSrcPos.c_str());
        _Exit(0);
      }
    }
  }
  if (!Options.ExitOnItem.empty()) {
    if (Corpus.HasUnit(Options.ExitOnItem)) {
      Printf("INFO: found item with checksum '%s', exiting.\n",
             Options.ExitOnItem.c_str());
      _Exit(0);
    }
  }
}

void Fuzzer::RereadOutputCorpus(size_t MaxSize) {
  if (Options.OutputCorpus.empty() || !Options.ReloadIntervalSec) return;
  std::vector<Unit> AdditionalCorpus;
  ReadDirToVectorOfUnits(Options.OutputCorpus.c_str(), &AdditionalCorpus,
                         &EpochOfLastReadOfOutputCorpus, MaxSize,
                         /*ExitOnError*/ false);
  if (Options.Verbosity >= 2)
    Printf("Reload: read %zd new units.\n", AdditionalCorpus.size());
  bool Reloaded = false;
  for (auto &U : AdditionalCorpus) {
    if (U.size() > MaxSize)
      U.resize(MaxSize);
    if (!Corpus.HasUnit(U)) {
      if (RunOne(U.data(), U.size()))
        Reloaded = true;
    }
  }
  if (Reloaded)
    PrintStats("RELOAD");
}

void Fuzzer::ShuffleCorpus(UnitVector *V) {
  std::shuffle(V->begin(), V->end(), MD.GetRand());
  if (Options.PreferSmall)
    std::stable_sort(V->begin(), V->end(), [](const Unit &A, const Unit &B) {
      return A.size() < B.size();
    });
}

void Fuzzer::ShuffleAndMinimize(UnitVector *InitialCorpus) {
  Printf("#0\tREAD units: %zd\n", InitialCorpus->size());
  if (Options.ShuffleAtStartUp)
    ShuffleCorpus(InitialCorpus);

  // Test the callback with empty input and never try it again.
  uint8_t dummy;
  ExecuteCallback(&dummy, 0);
  int temp = 0;
  for (const auto &U : *InitialCorpus) {
    if(RunOne(U.data(), U.size()))
    {
      MD.RecordSuccessfulMutationSequence();
      PrintStatusForNewUnit(U);
      //WriteToOutputCorpus(U);
      NumberOfNewUnitsAdded++;
      TPC.PrintNewPCs();
    }  
    if (TotalNumberOfRuns >= Options.MaxNumberOfRuns)
      break;
    TryDetectingAMemoryLeak(U.data(), U.size(),/*DuringInitialCorpusExecution*/ true);
  }
  Printf("%d \n",temp);
  PrintStats("INITED");
  if (Corpus.empty()) {
    Printf("ERROR: no interesting inputs were found. "
           "Is the code instrumented for coverage? Exiting.\n");
    exit(1);
  }
}

void Fuzzer::PrintPulseAndReportSlowInput(const uint8_t *Data, size_t Size) {
  auto TimeOfUnit =
      duration_cast<seconds>(UnitStopTime - UnitStartTime).count();
  if (!(TotalNumberOfRuns & (TotalNumberOfRuns - 1)) &&
      secondsSinceProcessStartUp() >= 2)
    PrintStats("pulse ");
  if (TimeOfUnit > TimeOfLongestUnitInSeconds * 1.1 &&
      TimeOfUnit >= Options.ReportSlowUnits) {
    TimeOfLongestUnitInSeconds = TimeOfUnit;
    Printf("Slowest unit: %zd s:\n", TimeOfLongestUnitInSeconds);
    WriteUnitToFileWithPrefix({Data, Data + Size}, "slow-unit-");
  }
}

bool Fuzzer::RunOneCallback(const uint8_t *Data, size_t Size, size_t idx,
                            bool MayDeleteFile, InputInfo *II) {
  if (!Size) return false;

  int ret = ExecuteCallback(Data, Size);
  if (Options.DifferentialMode) TPC.OutputDiffVec[idx] = ret;
  FeatureSetTmp.clear();
  size_t NumUpdatesBefore = Corpus.NumFeatureUpdates();
  TPC.CollectFeatures([&](size_t Feature) {
    Corpus.AddFeature(Feature, Size, Options.Shrink);
    if (Options.ReduceInputs)
      FeatureSetTmp.push_back(Feature);
  });
  PrintPulseAndReportSlowInput(Data, Size);
  size_t NumNewFeatures = Corpus.NumFeatureUpdates() - NumUpdatesBefore;
  if (NumNewFeatures) {
	Corpus.AddToCorpus({Data, Data + Size}, NumNewFeatures, MayDeleteFile,
                       FeatureSetTmp);	
    CheckExitOnSrcPosOrItem();
    return true;
  }
  if (II && Corpus.TryToReplace(II, Data, Size, FeatureSetTmp)) {
    CheckExitOnSrcPosOrItem();
    return true;
  }
  return false;
}

bool Fuzzer::RunOne(const uint8_t *Data, size_t Size, bool MayDeleteFile,
                    InputInfo *II) {
  if (Options.DifferentialMode) {      
    TPC.ResetCoverage();
    size_t ret = 0, cb_ret = 0, features = 0;
    UnitHadOutputDiff = false;
    std::vector<int> feature_vec;
    size_t CoverageBefore = TPC.GetTotalPCCoverage();
    
    //EF->__sanitizer_update_counter_bitset_and_clear_counters(0);
    for (int i = 0; i < TPC.UC->size; ++i) {
      CB = TPC.UC->callbacks[i];
      cb_ret = RunOneCallback(Data, Size, i, MayDeleteFile, II);
      features += cb_ret;
      feature_vec.push_back(cb_ret);
    }
    size_t NumCoverage = TPC.GetTotalPCCoverage() - CoverageBefore;
    //bool new_diff = TPC.NewOutputDiff() | (NumCoverage > 0);
    //bool new_diff = TPC.NewOutputDiff() | TPC.NewTraceDiff(feature_vec);
    bool new_diff = TPC.NewOutputDiff_change();
    if(TPC.NewTraceDiff(feature_vec))
    {
        NumberofValidCases++;
    }
    if (new_diff)
    {
      FeatureSetTmp.clear();
      DumpUnitIfDiff(Data, Size);
      if(UnitHadOutputDiff)
      {
		Corpus.AddToCorpus({Data, Data + Size}, NumCoverage, MayDeleteFile,
                       FeatureSetTmp);      
      }
      else
      {}
    }
    //TPC.ResetCoverage(); 
    TotalNumberOfRuns++;
    //log 
    if ((int)TotalNumberOfRuns % 20 == 0)
    {
	char message[100];
	sprintf(message,"%zd\t%zd\t%zd\t%zd",TotalNumberOfRuns,Duplicate,NumberOfDiffUnitsAdded,NumberofValidCases);
	log_save("./log",message);
    }
    
    return features > 0 ? features : new_diff;
  }

  return RunOneCallback(Data, Size, 0, MayDeleteFile, II);
}

size_t Fuzzer::GetCurrentUnitInFuzzingThead(const uint8_t **Data) const {
  assert(InFuzzingThread());
  *Data = CurrentUnitData;
  return CurrentUnitSize;
}

void Fuzzer::CrashOnOverwrittenData() {
  Printf("==%d== ERROR: libFuzzer: fuzz target overwrites it's const input\n",
         GetPid());
  DumpCurrentUnit("crash-");
  Printf("SUMMARY: libFuzzer: out-of-memory\n");
  _Exit(Options.ErrorExitCode); // Stop right now.
}

// Compare two arrays, but not all bytes if the arrays are large.
static bool LooseMemeq(const uint8_t *A, const uint8_t *B, size_t Size) {
  const size_t Limit = 64;
  if (Size <= 64)
    return !memcmp(A, B, Size);
  // Compare first and last Limit/2 bytes.
  return !memcmp(A, B, Limit / 2) &&
         !memcmp(A + Size - Limit / 2, B + Size - Limit / 2, Limit / 2);
}

int Fuzzer::ExecuteCallback(const uint8_t *Data, size_t Size) {
 
  assert(InFuzzingThread());
  if (SMR.IsClient())
    SMR.WriteByteArray(Data, Size);
  // We copy the contents of Unit into a separate heap buffer
  // so that we reliably find buffer overflows in it.
  uint8_t *DataCopy = new uint8_t[Size];
  memcpy(DataCopy, Data, Size);
  if (CurrentUnitData && CurrentUnitData != Data)
    memcpy(CurrentUnitData, Data, Size);
  CurrentUnitSize = Size;
  AllocTracer.Start(Options.TraceMalloc);
  UnitStartTime = system_clock::now();
  TPC.ResetMaps();
  RunningCB = true;
  int Res = CB(DataCopy, Size);
  RunningCB = false;
  UnitStopTime = system_clock::now();
  if (!Options.DifferentialMode) {
    (void)Res;
    assert(Res == 0);
  }
  HasMoreMallocsThanFrees = AllocTracer.Stop();
  if (!LooseMemeq(DataCopy, Data, Size))
    CrashOnOverwrittenData();
  CurrentUnitSize = 0;
  delete[] DataCopy;
  return Res;
}

void Fuzzer::WriteToOutputCorpus(const Unit &U) {
  if (Options.OnlyASCII)
    assert(IsASCII(U));
  if (Options.OutputCorpus.empty())
    return;
  std::string Path = DirPlusFile(Options.OutputCorpus, Hash(U));
  WriteToFile(U, Path);
  if (Options.Verbosity >= 2)
    Printf("Written to %s\n", Path.c_str());
}

void Fuzzer::WriteUnitToFileWithPrefix(const Unit &U, const char *Prefix) {
  if (!Options.SaveArtifacts)
    return;
  std::string Path = Options.ArtifactPrefix + Prefix + Hash(U);
  if (!Options.ExactArtifactPath.empty())
    Path = Options.ExactArtifactPath; // Overrides ArtifactPrefix.
  WriteToFile(U, Path);
  Printf("artifact_prefix='%s'; Test unit written to %s\n",
         Options.ArtifactPrefix.c_str(), Path.c_str());
  if (U.size() <= kMaxUnitSizeToPrint)
    Printf("Base64: %s\n", Base64(U).c_str());
}

void Fuzzer::PrintStatusForNewUnit(const Unit &U) {
  if (!Options.PrintNEW)
    return;
  PrintStats("NEW   ", "");
  if (Options.Verbosity) {
    Printf(" L: %zd ", U.size());
    MD.PrintMutationSequence();
    Printf("\n");
  }
}

void Fuzzer::ReportNewCoverage(InputInfo *II, const Unit &U) {
  II->NumSuccessfullMutations++;
  MD.RecordSuccessfulMutationSequence();
  PrintStatusForNewUnit(U);
  WriteToOutputCorpus(U);
  NumberOfNewUnitsAdded++;
  TPC.PrintNewPCs();
}

// Tries detecting a memory leak on the particular input that we have just
// executed before calling this function.
void Fuzzer::TryDetectingAMemoryLeak(const uint8_t *Data, size_t Size,
                                     bool DuringInitialCorpusExecution) {
  if (!HasMoreMallocsThanFrees) return;  // mallocs==frees, a leak is unlikely.
  if (!Options.DetectLeaks) return;
  if (!&(EF->__lsan_enable) || !&(EF->__lsan_disable) ||
      !(EF->__lsan_do_recoverable_leak_check))
    return;  // No lsan.
  // Run the target once again, but with lsan disabled so that if there is
  // a real leak we do not report it twice.
  EF->__lsan_disable();
  ExecuteCallback(Data, Size);
  EF->__lsan_enable();
  if (!HasMoreMallocsThanFrees) return;  // a leak is unlikely.
  if (NumberOfLeakDetectionAttempts++ > 1000) {
    Options.DetectLeaks = false;
    Printf("INFO: libFuzzer disabled leak detection after every mutation.\n"
           "      Most likely the target function accumulates allocated\n"
           "      memory in a global state w/o actually leaking it.\n"
           "      You may try running this binary with -trace_malloc=[12]"
           "      to get a trace of mallocs and frees.\n"
           "      If LeakSanitizer is enabled in this process it will still\n"
           "      run on the process shutdown.\n");
    return;
  }
  // Now perform the actual lsan pass. This is expensive and we must ensure
  // we don't call it too often.
  if (EF->__lsan_do_recoverable_leak_check()) { // Leak is found, report it.
    if (DuringInitialCorpusExecution)
      Printf("\nINFO: a leak has been found in the initial corpus.\n\n");
    Printf("INFO: to ignore leaks on libFuzzer side use -detect_leaks=0.\n\n");
    CurrentUnitSize = Size;
    DumpCurrentUnit("leak-");
    PrintFinalStats();
    _Exit(Options.ErrorExitCode);  // not exit() to disable lsan further on.
  }
}

static size_t ComputeMutationLen(size_t MaxInputSize, size_t MaxMutationLen,
                                 Random &Rand) {
  assert(MaxInputSize <= MaxMutationLen);
  if (MaxInputSize == MaxMutationLen) return MaxMutationLen;
  size_t Result = MaxInputSize;
  size_t R = Rand.Rand();
  if ((R % (1U << 7)) == 0)
    Result++;
  if ((R % (1U << 15)) == 0)
    Result += 10 + Result / 2;
  return Min(Result, MaxMutationLen);
}

void Fuzzer::MutateAndTestOne() {
  MD.StartMutationSequence();

  auto &II = Corpus.ChooseUnitToMutate(MD.GetRand());
  const auto &U = II.U;
  memcpy(BaseSha1, II.Sha1, sizeof(BaseSha1));
  assert(CurrentUnitData);
  size_t Size = U.size();
  assert(Size <= MaxInputLen && "Oversized Unit");
  memcpy(CurrentUnitData, U.data(), Size);

  assert(MaxMutationLen > 0);
  uint8_t *PreviousUnit = new uint8_t[MaxInputLen];
  size_t PreviousSize = 0;

  size_t CurrentMaxMutationLen =
      Options.ExperimentalLenControl
          ? ComputeMutationLen(Corpus.MaxInputSize(), MaxMutationLen,
                               MD.GetRand())
          : MaxMutationLen;

  for (int i = 0; i < Options.MutateDepth; i++) {
    if (TotalNumberOfRuns >= Options.MaxNumberOfRuns)
      break;
    
      
    size_t NewSize = 0;
    
    do{
	
	memcpy(PreviousUnit, CurrentUnitData, Size);
    	PreviousSize = Size;  

	NewSize = MD.Mutate(CurrentUnitData, Size, CurrentMaxMutationLen);
	uint8_t Hash[kSHA1NumBytes];
        ComputeSHA1((uint8_t *)(CurrentUnitData), NewSize, Hash);
	std::string tmp(Hash,Hash+kSHA1NumBytes);
	if(hashMap.count(tmp) > 0)
	{	
		NumberOfDuplicate++;
		continue;
	}
	hashMap[tmp] = true;
	
    }while(NewSize > CurrentMaxMutationLen);
    
    assert(NewSize > 0 && "Mutator returned empty unit");
    assert(NewSize <= CurrentMaxMutationLen && "Mutator return overisized unit");
    Size = NewSize;
    II.NumExecutedMutations++;
    if (RunOne(CurrentUnitData, Size, /*MayDeleteFile=*/true, &II)) {
	ReportNewCoverage(&II, {CurrentUnitData, CurrentUnitData + Size});
      if (UnitHadOutputDiff) {	
        
        uint8_t Hash[kSHA1NumBytes];
        ComputeSHA1((uint8_t *)(CurrentUnitData), Size, Hash);
        std::string s = Sha1ToString(Hash) + "_BeforeMutationWas_";
        WriteUnitToFileWithPrefix({PreviousUnit, PreviousUnit + PreviousSize},
                                  s.c_str());
      }
    }

    TryDetectingAMemoryLeak(CurrentUnitData, Size,
                            /*DuringInitialCorpusExecution*/ false);
  }
  delete [] PreviousUnit;
}

void Fuzzer::Loop() {
  TPC.InitializePrintNewPCs();
  system_clock::time_point LastCorpusReload = system_clock::now();
  if (Options.DoCrossOver)
    MD.SetCorpus(&Corpus);
  
  srand((int)time(0));
  while (true) {
    auto Now = system_clock::now();
    if (duration_cast<seconds>(Now - LastCorpusReload).count() >=
        Options.ReloadIntervalSec) {
      RereadOutputCorpus(MaxInputLen);
      LastCorpusReload = system_clock::now();
    }
    if (TotalNumberOfRuns >= Options.MaxNumberOfRuns)
      break;
    if (TimedOut()) break;
    // Perform several mutations and runs.
    MutateAndTestOne();
  }

  PrintStats("DONE  ", "\n");
  MD.PrintRecommendedDictionary();
}

void Fuzzer::MinimizeCrashLoop(const Unit &U) {
  if (U.size() <= 1) return;
  while (!TimedOut() && TotalNumberOfRuns < Options.MaxNumberOfRuns) {
    MD.StartMutationSequence();
    memcpy(CurrentUnitData, U.data(), U.size());
    for (int i = 0; i < Options.MutateDepth; i++) {
      size_t NewSize = MD.Mutate(CurrentUnitData, U.size(), MaxMutationLen);
      assert(NewSize > 0 && NewSize <= MaxMutationLen);
      ExecuteCallback(CurrentUnitData, NewSize);
      PrintPulseAndReportSlowInput(CurrentUnitData, NewSize);
      TryDetectingAMemoryLeak(CurrentUnitData, NewSize,
                              /*DuringInitialCorpusExecution*/ false);
    }
  }
}

void Fuzzer::AnnounceOutput(const uint8_t *Data, size_t Size) {
  if (SMR.IsServer()) {
    SMR.WriteByteArray(Data, Size);
  } else if (SMR.IsClient()) {
    SMR.PostClient();
    SMR.WaitServer();
    size_t OtherSize = SMR.ReadByteArraySize();
    uint8_t *OtherData = SMR.GetByteArray();
    if (Size != OtherSize || memcmp(Data, OtherData, Size) != 0) {
      size_t i = 0;
      for (i = 0; i < Min(Size, OtherSize); i++)
        if (Data[i] != OtherData[i])
          break;
      Printf("==%lu== ERROR: libFuzzer: equivalence-mismatch. Sizes: %zd %zd; "
             "offset %zd\n", GetPid(), Size, OtherSize, i);
      DumpCurrentUnit("mismatch-");
      Printf("SUMMARY: libFuzzer: equivalence-mismatch\n");
      PrintFinalStats();
      _Exit(Options.ErrorExitCode);
    }
  }
}

} // namespace fuzzer

extern "C" {

size_t LLVMFuzzerMutate(uint8_t *Data, size_t Size, size_t MaxSize) {
  assert(fuzzer::F);
  return fuzzer::F->GetMD().DefaultMutate(Data, Size, MaxSize);
}

// Experimental
void LLVMFuzzerAnnounceOutput(const uint8_t *Data, size_t Size) {
  assert(fuzzer::F);
  fuzzer::F->AnnounceOutput(Data, Size);
}
}  // extern "C"
