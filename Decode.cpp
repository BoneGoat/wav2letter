/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <vector>

#include <flashlight/flashlight.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "common/Defines.h"
#include "common/Dictionary.h"
#include "common/Transforms.h"
#include "common/Utils.h"
#include "criterion/criterion.h"
#include "data/Featurize.h"
#include "decoder/KenLM.h"
#include "decoder/Trie.h"
#include "module/module.h"
#include "runtime/Data.h"
#include "runtime/Logger.h"
#include "runtime/Serial.h"

#include "decoder/LexiconFreeDecoder.h"
#include "decoder/TokenLMDecoder.h"
#include "decoder/WordLMDecoder.h"

using namespace w2l;

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  std::string exec(argv[0]);
  std::vector<std::string> argvs;
  for (int i = 0; i < argc; i++) {
    argvs.emplace_back(argv[i]);
  }
  gflags::SetUsageMessage(
      "Usage: \n " + exec + " [data_path] [dataset_name] [flags]");
  if (argc <= 1) {
    LOG(FATAL) << gflags::ProgramUsage();
  }

  /* ===================== Parse Options ===================== */
  LOG(INFO) << "Parsing command line flags";
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  auto flagsfile = FLAGS_flagsfile;
  if (!flagsfile.empty()) {
    LOG(INFO) << "Reading flags from file " << flagsfile;
    gflags::ReadFromFlagsFile(flagsfile, argv[0], true);
  }

  /* ===================== Create Network ===================== */
  if (!(FLAGS_am.empty() ^ FLAGS_emission_dir.empty())) {
    LOG(FATAL)
        << "One and only one of flag -am and -emission_dir should be set.";
  }
  EmissionSet emissionSet;

  /* Using acoustic model */
  std::shared_ptr<fl::Module> network;
  std::shared_ptr<SequenceCriterion> criterion;
  if (!FLAGS_am.empty()) {
    std::unordered_map<std::string, std::string> cfg;
    LOG(INFO) << "[Network] Reading acoustic model from " << FLAGS_am;

    W2lSerializer::load(FLAGS_am, cfg, network, criterion);
    network->eval();
    LOG(INFO) << "[Network] " << network->prettyString();
    if (criterion) {
      criterion->eval();
      LOG(INFO) << "[Network] " << criterion->prettyString();
    }
    LOG(INFO) << "[Network] Number of params: " << numTotalParams(network);

    auto flags = cfg.find(kGflags);
    if (flags == cfg.end()) {
      LOG(FATAL) << "[Network] Invalid config loaded from " << FLAGS_am;
    }
    LOG(INFO) << "[Network] Updating flags from config file: " << FLAGS_am;
    gflags::ReadFlagsFromString(flags->second, gflags::GetArgv0(), true);
  }
  /* Using existing emissions */
  else {
    std::string cleanedTestPath = cleanFilepath(FLAGS_test);
    std::string loadPath =
        pathsConcat(FLAGS_emission_dir, cleanedTestPath + ".bin");
    LOG(INFO) << "[Serialization] Loading file: " << loadPath;
    W2lSerializer::load(loadPath, emissionSet);
    gflags::ReadFlagsFromString(emissionSet.gflags, gflags::GetArgv0(), true);
  }

  // override with user-specified flags
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  if (!flagsfile.empty()) {
    gflags::ReadFromFlagsFile(flagsfile, argv[0], true);
  }

  LOG(INFO) << "Gflags after parsing \n" << serializeGflags("; ");

  /* ===================== Create Dictionary ===================== */

  auto tokenDict = createTokenDict(pathsConcat(FLAGS_tokensdir, FLAGS_tokens));
  int numClasses = tokenDict.indexSize();
  LOG(INFO) << "Number of classes (network): " << numClasses;

  Dictionary wordDict;
  LexiconMap lexicon;
  if (!FLAGS_lexicon.empty()) {
    lexicon = loadWords(FLAGS_lexicon, FLAGS_maxword);
    wordDict = createWordDict(lexicon);
    LOG(INFO) << "Number of words: " << wordDict.indexSize();
  }

  DictionaryMap dicts = {{kTargetIdx, tokenDict}, {kWordIdx, wordDict}};

  /* ===================== Create Dataset ===================== */
  if (FLAGS_emission_dir.empty()) {
    // Load dataset
    int worldRank = 0;
    int worldSize = 1;
    auto ds =
        createDataset(FLAGS_test, dicts, lexicon, 1, worldRank, worldSize);

    ds->shuffle(3);
    LOG(INFO) << "[Serialization] Running forward pass ...";

    int cnt = 0;
    for (auto& sample : *ds) {
      auto rawEmission =
          network->forward({fl::input(sample[kInputIdx])}).front();
      int N = rawEmission.dims(0);
      int T = rawEmission.dims(1);

      auto emission = afToVector<float>(rawEmission);
      auto tokenTarget = afToVector<int>(sample[kTargetIdx]);
      auto wordTarget = afToVector<int>(sample[kWordIdx]);

      // TODO: we will reform the w2l dataset so that the loaded word targets
      // are strings already
      std::vector<std::string> wordTargetStr;
      if (!FLAGS_lexicon.empty() && FLAGS_criterion != kSeq2SeqCriterion) {
        wordTargetStr = wrdTensor2Words(wordTarget, wordDict);
      } else {
        auto letterTarget = tkn2Ltr(tokenTarget, tokenDict);
        wordTargetStr = tknTensor2Words(letterTarget, tokenDict);
      }

      emissionSet.emissions.emplace_back(emission);
      emissionSet.wordTargets.emplace_back(wordTargetStr);
      emissionSet.tokenTargets.emplace_back(tokenTarget);
      emissionSet.emissionT.emplace_back(T);
      emissionSet.emissionN = N;

      // while decoding we use batchsize 1 and hence ds only has 1 sampleid
      emissionSet.sampleIds.emplace_back(
          afToVector<std::string>(sample[kSampleIdx]).front());

      ++cnt;
      if (cnt == FLAGS_maxload) {
        break;
      }
    }
    if (FLAGS_criterion == kAsgCriterion) {
      emissionSet.transition = afToVector<float>(criterion->param(0).array());
    }
  }

  int nSample = emissionSet.emissions.size();
  nSample = FLAGS_maxload > 0 ? std::min(nSample, FLAGS_maxload) : nSample;
  int nSamplePerThread =
      std::ceil(nSample / static_cast<float>(FLAGS_nthread_decoder));
  LOG(INFO) << "[Dataset] Number of samples per thread: " << nSamplePerThread;

  /* ===================== Decode ===================== */
  // Prepare counters
  std::vector<double> sliceWer(FLAGS_nthread_decoder);
  std::vector<double> sliceLer(FLAGS_nthread_decoder);
  std::vector<int> sliceNumWords(FLAGS_nthread_decoder, 0);
  std::vector<int> sliceNumTokens(FLAGS_nthread_decoder, 0);
  std::vector<int> sliceNumSamples(FLAGS_nthread_decoder, 0);
  std::vector<double> sliceTime(FLAGS_nthread_decoder, 0);

  // Prepare criterion
  CriterionType criterionType = CriterionType::ASG;
  if (FLAGS_criterion == kCtcCriterion) {
    criterionType = CriterionType::CTC;
  } else if (FLAGS_criterion != kAsgCriterion) {
    LOG(FATAL) << "[Decoder] Invalid model type: " << FLAGS_criterion;
  }

  const auto& transition = emissionSet.transition;

  // Prepare decoder options
  DecoderOptions decoderOpt(
      FLAGS_beamsize,
      static_cast<float>(FLAGS_beamthreshold),
      static_cast<float>(FLAGS_lmweight),
      static_cast<float>(FLAGS_wordscore),
      static_cast<float>(FLAGS_unkweight),
      FLAGS_logadd,
      static_cast<float>(FLAGS_silweight),
      criterionType);

  // Prepare log writer
  std::mutex hypMutex, refMutex, logMutex;
  std::ofstream hypStream, refStream, logStream;
  if (!FLAGS_sclite.empty()) {
    auto fileName = cleanFilepath(FLAGS_test);
    auto hypPath = pathsConcat(FLAGS_sclite, fileName + ".hyp");
    auto refPath = pathsConcat(FLAGS_sclite, fileName + ".ref");
    auto logPath = pathsConcat(FLAGS_sclite, fileName + ".log");
    hypStream.open(hypPath);
    refStream.open(refPath);
    logStream.open(logPath);
    if (!hypStream.is_open() || !hypStream.good()) {
      LOG(FATAL) << "Error opening hypothesis file: " << hypPath;
    }
    if (!refStream.is_open() || !refStream.good()) {
      LOG(FATAL) << "Error opening reference file: " << refPath;
    }
    if (!logStream.is_open() || !logStream.good()) {
      LOG(FATAL) << "Error opening log file: " << logPath;
    }
  }

  auto writeHyp = [&](const std::string& hypStr) {
    std::lock_guard<std::mutex> lock(hypMutex);
    hypStream << hypStr;
  };
  auto writeRef = [&](const std::string& refStr) {
    std::lock_guard<std::mutex> lock(refMutex);
    refStream << refStr;
  };
  auto writeLog = [&](const std::string& logStr) {
    std::lock_guard<std::mutex> lock(logMutex);
    logStream << logStr;
  };

  // Build Language Model
  std::shared_ptr<LM> lm;
  if (FLAGS_lmtype == "kenlm") {
    lm = std::make_shared<KenLM>(FLAGS_lm);
    if (!lm) {
      LOG(FATAL) << "[LM constructing] Failed to load LM: " << FLAGS_lm;
    }
  } else {
    LOG(FATAL) << "[LM constructing] Invalid LM Type: " << FLAGS_lmtype;
  }
  LOG(INFO) << "[Decoder] LM constructed.\n";

  // Build Trie
  if (std::strlen(kSilToken) != 1) {
    LOG(FATAL) << "[Decoder] Invalid unknown_symbol: " << kSilToken;
  }
  if (std::strlen(kBlankToken) != 1) {
    LOG(FATAL) << "[Decoder] Invalid unknown_symbol: " << kBlankToken;
  }
  int silIdx = tokenDict.getIndex(kSilToken);
  int blankIdx =
      FLAGS_criterion == kCtcCriterion ? tokenDict.getIndex(kBlankToken) : -1;
  int unkIdx = lm->index(kUnkToken);
  std::shared_ptr<TrieLabel> unk = nullptr;

  std::shared_ptr<Trie> trie = nullptr;
  if (!FLAGS_lexicon.empty()) {
    trie = std::make_shared<Trie>(tokenDict.indexSize(), silIdx);
    auto start_state = lm->start(false);

    for (auto& it : lexicon) {
      const std::string& word = it.first;
      int lmIdx = -1;
      float score = -1;
      if (FLAGS_decodertype == "wrd") {
        lmIdx = lm->index(word);
        auto dummyState = lm->score(start_state, lmIdx, score);
      }
      for (auto& tokens : it.second) {
        auto tokensTensor = tokens2Tensor(tokens, tokenDict);
        trie->insert(
            tokensTensor,
            std::make_shared<TrieLabel>(lmIdx, wordDict.getIndex(word)),
            score);
      }
    }
    unk = std::make_shared<TrieLabel>(unkIdx, wordDict.getIndex(kUnkToken));
    LOG(INFO) << "[Decoder] Trie planted.\n";

    // Smearing
    SmearingMode smear_mode = SmearingMode::NONE;
    if (FLAGS_smearing == "logadd") {
      smear_mode = SmearingMode::LOGADD;
    } else if (FLAGS_smearing == "max") {
      smear_mode = SmearingMode::MAX;
    } else if (FLAGS_smearing != "none") {
      LOG(FATAL) << "[Decoder] Invalid smearing mode: " << FLAGS_smearing;
    }
    trie->smear(smear_mode);
    LOG(INFO) << "[Decoder] Trie smeared.\n";
  }

  // Decoding
  auto runDecoder = [&](int tid, int start, int end) {
    try {
      // Build Decoder
      std::unique_ptr<Decoder> decoder;

      if (FLAGS_decodertype == "wrd") {
        decoder = std::make_unique<WordLMDecoder>(
            decoderOpt, trie, lm, silIdx, blankIdx, unk, transition);
        LOG(INFO) << "[Decoder] Decoder with word-LM loaded in thread: " << tid;
      } else if (FLAGS_decodertype == "tkn") {
        std::unordered_map<int, int> lmIndMap;
        for (int i = 0; i < tokenDict.indexSize(); i++) {
          const std::string& token = tokenDict.getToken(i);
          int lmIdx = lm->index(token);
          lmIndMap[i] = lmIdx;
        }
        if (!FLAGS_lexicon.empty()) {
          decoder = std::make_unique<TokenLMDecoder>(
              decoderOpt,
              trie,
              lm,
              silIdx,
              blankIdx,
              unk,
              transition,
              lmIndMap);
          LOG(INFO) << "[Decoder] Decoder with token-LM loaded in thread: "
                    << tid;
        } else {
          decoder = std::make_unique<LexiconFreeDecoder>(
              decoderOpt, lm, silIdx, blankIdx, transition, lmIndMap);
          LOG(INFO) << "[Decoder] Decoder with token-LM loaded in thread: "
                    << tid;
        }
      } else {
        LOG(FATAL) << "Unsupported decoder type: " << FLAGS_decodertype;
      }

      // Get data and run decoder
      TestMeters meters;
      int sliceSize = end - start;
      meters.timer.resume();
      for (int s = start; s < end; s++) {
        auto emission = emissionSet.emissions[s];
        auto wordTarget = emissionSet.wordTargets[s];
        auto tokenTarget = emissionSet.tokenTargets[s];
        auto sampleId = emissionSet.sampleIds[s];
        auto T = emissionSet.emissionT[s];
        auto N = emissionSet.emissionN;

        // DecodeResult
        auto results = decoder->decode(emission.data(), T, N);

        // Cleanup predictions
        auto& rawWordPrediction = results[0].words_;
        auto& rawTokenPrediction = results[0].tokens_;

        auto letterTarget = tkn2Ltr(tokenTarget, tokenDict);
        auto letterPrediction = tkn2Ltr(rawTokenPrediction, tokenDict);
        std::vector<std::string> wordPrediction;
        if (!FLAGS_lexicon.empty() && FLAGS_criterion != kSeq2SeqCriterion) {
          rawWordPrediction =
              validateTensor(rawWordPrediction, wordDict.getIndex(kUnkToken));
          wordPrediction = wrdTensor2Words(rawWordPrediction, wordDict);
        } else {
          wordPrediction = tknTensor2Words(letterPrediction, tokenDict);
        }

        // Update meters & print out predictions
        meters.werSlice.add(wordPrediction, wordTarget);
        meters.lerSlice.add(letterPrediction, letterTarget);

        if (FLAGS_show) {
          meters.wer.reset();
          meters.ler.reset();
          meters.wer.add(wordPrediction, wordTarget);
          meters.ler.add(letterPrediction, letterTarget);

          auto wordTargetStr = join(" ", wordTarget);
          auto wordPredictionStr = join(" ", wordPrediction);

          std::stringstream buffer;
          buffer << "|T|: " << wordTargetStr << std::endl;
          buffer << "|P|: " << wordPredictionStr << std::endl;
          if (FLAGS_showletters) {
            buffer << "|t|: " << tensor2String(letterTarget, tokenDict)
                   << std::endl;
            buffer << "|p|: " << tensor2String(letterPrediction, tokenDict)
                   << std::endl;
          }
          buffer << "[sample: " << sampleId
                 << ", WER: " << meters.wer.value()[0]
                 << "\%, LER: " << meters.ler.value()[0]
                 << "\%, slice WER: " << meters.werSlice.value()[0]
                 << "\%, slice LER: " << meters.lerSlice.value()[0]
                 << "\%, progress: "
                 << static_cast<float>(s - start + 1) / sliceSize * 100 << "\%]"
                 << std::endl;

          std::cout << buffer.str();
          if (!FLAGS_sclite.empty()) {
            std::string suffix = "(" + sampleId + ")\n";
            writeHyp(wordPredictionStr + suffix);
            writeRef(wordTargetStr + suffix);
            writeLog(buffer.str());
          }
        }

        // Update conters
        sliceNumWords[tid] += wordTarget.size();
        sliceNumTokens[tid] += tokenTarget.size();
      }
      meters.timer.stop();
      sliceWer[tid] = meters.werSlice.value()[0];
      sliceLer[tid] = meters.lerSlice.value()[0];
      sliceNumSamples[tid] = sliceSize;
      sliceTime[tid] = meters.timer.value();
    } catch (const std::exception& exc) {
      LOG(FATAL) << "Exception in thread " << tid << "\n" << exc.what();
    }
  };

  /* Spread threades */
  auto startThreads = [&]() {
    if (FLAGS_nthread_decoder == 1) {
      runDecoder(0, 0, nSample);
    } else if (FLAGS_nthread_decoder > 1) {
      fl::ThreadPool threadPool(FLAGS_nthread_decoder);
      for (int i = 0; i < FLAGS_nthread_decoder; i++) {
        int start = i * nSamplePerThread;
        if (start >= nSample) {
          break;
        }
        int end = std::min((i + 1) * nSamplePerThread, nSample);
        threadPool.enqueue(runDecoder, i, start, end);
      }
    } else {
      LOG(FATAL) << "Invalid nthread_decoder";
    }
  };
  auto timer = fl::TimeMeter();
  timer.resume();
  startThreads();
  timer.stop();

  /* Compute statistics */
  int totalTokens = 0, totalWords = 0, totalSamples = 0;
  for (int i = 0; i < FLAGS_nthread_decoder; i++) {
    totalTokens += sliceNumTokens[i];
    totalWords += sliceNumWords[i];
    totalSamples += sliceNumSamples[i];
  }
  double totalWer = 0, totalLer = 0, totalTime = 0;
  for (int i = 0; i < FLAGS_nthread_decoder; i++) {
    totalWer += sliceWer[i] * sliceNumWords[i] / totalWords;
    totalLer += sliceLer[i] * sliceNumTokens[i] / totalTokens;
    totalTime += sliceTime[i];
  }

  std::stringstream buffer;
  buffer << "------\n";
  buffer << "[Decode " << FLAGS_test << " (" << totalSamples << " samples) in "
         << timer.value() << "s (actual decoding time " << std::setprecision(3)
         << totalTime / totalSamples
         << "s/sample) -- WER: " << std::setprecision(6) << totalWer
         << ", LER: " << totalLer << "]" << std::endl;
  LOG(INFO) << buffer.str();
  if (!FLAGS_sclite.empty()) {
    writeLog(buffer.str());
    hypStream.close();
    refStream.close();
    logStream.close();
  }
  return 0;
}
