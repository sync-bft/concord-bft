// Concord
//
// Copyright (c) 2018 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").  You may not use this product except in
// compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright notices and license terms. Your use of
// these subcomponents is subject to the terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#pragma once

#include <type_traits>
#include <unordered_map>
#include <set>

#include "PrimitiveTypes.hpp"
#include "Digest.hpp"
#include "Crypto.hpp"
#include "SimpleThreadPool.hpp"
#include "IncomingMsgsStorage.hpp"
#include "assertUtils.hpp"
#include "messages/SignedShareMsgs.hpp"
#include "InternalReplicaApi.hpp"
#include "Logger.hpp"

namespace bftEngine {
namespace impl {

template <typename PART, typename FULL, typename ExternalFunc>
// TODO(GG): consider to enforce the requirements from PART, FULL and ExternalFunc
// TODO(GG): add Assert(ExternalFunc::numberOfRequiredSignatures(context) > 1);
class CollectorOfThresholdSignatures {
 public:
  CollectorOfThresholdSignatures(void* cnt) { this->context = cnt; }

  ~CollectorOfThresholdSignatures() { resetAndFree(); }

  bool addMsgWithPartialSignature(PART* partialSigMsg, ReplicaId repId) {
    Assert(partialSigMsg != nullptr);

    if ((combinedValidSignatureMsg != nullptr) || (replicasInfo.count(repId) > 0)) return false;

    // add partialSigMsg to replicasInfo
    RepInfo info = {partialSigMsg, SigState::Unknown};
    replicasInfo[repId] = info;

    numberOfUnknownSignatures++;

    trySendToBkThread();

    return true;
  }

  bool votesCollected() const {
    InternalReplicaApi* r = (InternalReplicaApi*)context;
    const ReplicasInfo& info = r->getReplicasInfo();
    return (numberOfVoteSignatures > info.fVal());
  }

  bool addMsgWithVoteSignature(PART* voteSigMsg, ReplicaId repId) {
    //LOG_DEBUG(CNSUS, "on addMsgWithVoteSignature()");
    Assert(voteSigMsg != nullptr);
    //LOG_DEBUG(CNSUS, "combinedValidSignatureMsg != nullptr : " << (combinedValidSignatureMsg != nullptr));
    //LOG_DEBUG(CNSUS, "replicasInfo.count(repId) > 0 : " << (replicasInfo.count(repId) > 0));
    if ((combinedValidSignatureMsg != nullptr) || (replicasInfo.count(repId) > 0)) return false;
    //LOG_DEBUG(CNSUS, "going to increase sig count");
    // add voteSigMsg to replicasInfo
    RepInfo info = {voteSigMsg, SigState::Unknown};
    replicasInfo[repId] = info;
    numberOfUnknownSignatures++;
    trySendToBkThread();
    //numberOfVoteSignatures++;
    return true;
  }
  
  bool addMsgWithCombinedSignature(FULL* combinedSigMsg) {
    if (combinedValidSignatureMsg != nullptr || candidateCombinedSignatureMsg != nullptr) return false;

    candidateCombinedSignatureMsg = combinedSigMsg;

    trySendToBkThread();

    return true;
  }

  void setExpected(SeqNum seqNumber, ViewNum view, Digest& digest) {
    Assert(seqNumber != 0);
    Assert(expectedSeqNumber == 0);
    Assert(!processingSignaturesInTheBackground);

    expectedSeqNumber = seqNumber;
    expectedView = view;
    expectedDigest = digest;

    trySendToBkThread();
  }

  void resetAndFree() {
    if ((replicasInfo.size() == 0) && (combinedValidSignatureMsg == nullptr) &&
        (candidateCombinedSignatureMsg == nullptr) && (expectedSeqNumber == 0))
      return;  // already empty

    resetContent();
  }

  void getAndReset(FULL*& outcombinedValidSignatureMsg) {
    outcombinedValidSignatureMsg = combinedValidSignatureMsg;

    if ((replicasInfo.size() == 0) && (combinedValidSignatureMsg == nullptr) &&
        (candidateCombinedSignatureMsg == nullptr) && (expectedSeqNumber == 0))
      return;  // already empty

    combinedValidSignatureMsg = nullptr;

    resetContent();
  }

  bool hasPartialMsgFromReplica(ReplicaId repId) const { return (replicasInfo.count(repId) > 0); }

  PART* getPartialMsgFromReplica(ReplicaId repId) const {
    if (replicasInfo.count(repId) == 0) return nullptr;

    const RepInfo& r = replicasInfo.at(repId);
    return r.partialSigMsg;
  }

  bool hasVoteMsgFromReplica(ReplicaId repId) const { return (replicasInfo.count(repId) > 0); }

  PART* getVoteMsgFromReplica(ReplicaId repId) const {
    if (replicasInfo.count(repId) == 0) return nullptr;

    const RepInfo& r = replicasInfo.at(repId);
    return r.partialSigMsg;
  }

  FULL* getMsgWithValidCombinedSignature() const { return combinedValidSignatureMsg; }

  bool isComplete() const { return (combinedValidSignatureMsg != nullptr); }

  void onCompletionOfSignaturesProcessing(SeqNum seqNumber,
                                          ViewNum view,
                                          const std::set<ReplicaId>& replicasWithBadSigs)  // if we found bad signatures
  {
    Assert(expectedSeqNumber == seqNumber);
    Assert(expectedView == view);
    Assert(processingSignaturesInTheBackground);
    Assert(combinedValidSignatureMsg == nullptr);

    processingSignaturesInTheBackground = false;

    for (const ReplicaId repId : replicasWithBadSigs) {
      RepInfo& repInfo = replicasInfo[repId];
      repInfo.state = SigState::Invalid;
      numberOfUnknownSignatures--;
    }

    trySendToBkThread();
  }

  void onCompletionOfSignaturesProcessing(SeqNum seqNumber,
                                          ViewNum view,
                                          const char* combinedSig,
                                          uint16_t combinedSigLen,
                                          const std::string& span_context)  // if we compute a valid combined signature
  {
    LOG_INFO(GL,
             "seqNum information is as follows ["
                 << seqNumber << " " << expectedSeqNumber <<"]");
    Assert(expectedSeqNumber == seqNumber);
    Assert(expectedView == view);
    Assert(processingSignaturesInTheBackground);
    Assert(combinedValidSignatureMsg == nullptr);

    processingSignaturesInTheBackground = false;

    if (candidateCombinedSignatureMsg != nullptr) {
      delete candidateCombinedSignatureMsg;
      candidateCombinedSignatureMsg = nullptr;
    }

    combinedValidSignatureMsg =
        ExternalFunc::createCombinedSignatureMsg(context, seqNumber, view, combinedSig, combinedSigLen, span_context);
  }

  void onCompletionOfCombinedSigVerification(SeqNum seqNumber, ViewNum view, bool isValid) {
    Assert(expectedSeqNumber == seqNumber);
    Assert(expectedView == view);
    Assert(processingSignaturesInTheBackground);
    Assert(combinedValidSignatureMsg == nullptr);
    Assert(candidateCombinedSignatureMsg != nullptr);

    processingSignaturesInTheBackground = false;

    if (isValid) {
      combinedValidSignatureMsg = candidateCombinedSignatureMsg;
      candidateCombinedSignatureMsg = nullptr;
    } else {
      delete candidateCombinedSignatureMsg;
      candidateCombinedSignatureMsg = nullptr;

      trySendToBkThread();
    }
  }

  // init the expected values (seqNum, view and digest) directly (without sending to a background thread)
  void initExpected(SeqNum seqNumber, ViewNum view, Digest& digest) {
    Assert(seqNumber != 0);
    Assert(expectedSeqNumber == 0);
    Assert(!processingSignaturesInTheBackground);

    expectedSeqNumber = seqNumber;
    expectedView = view;
    expectedDigest = digest;
  }

  // init the PART message directly (without sending to a background thread)
  bool initMsgWithPartialSignature(PART* partialSigMsg, ReplicaId repId) {
    Assert(partialSigMsg != nullptr);

    Assert(!processingSignaturesInTheBackground);
    Assert(expectedSeqNumber != 0);
    Assert(combinedValidSignatureMsg == nullptr);
    Assert(candidateCombinedSignatureMsg == nullptr);
    Assert(replicasInfo.count(repId) == 0);
    Assert(numberOfUnknownSignatures == 0);  // we can use this method to add at most one PART message

    // add partialSigMsg to replicasInfo
    RepInfo info = {partialSigMsg, SigState::Unknown};
    replicasInfo[repId] = info;

    // TODO(GG): do we want to verify the partial signature here?

    numberOfUnknownSignatures++;

    if (numOfRequiredSigs == 0)  // init numOfRequiredSigs
      numOfRequiredSigs = ExternalFunc::numberOfRequiredSignatures(context);

    Assert(numberOfUnknownSignatures < numOfRequiredSigs);  // because numOfRequiredSigs > 1

    return true;
  }

  // init the PART message directly (without sending to a background thread)
  bool initMsgWithVoteSignature(PART* voteSigMsg, ReplicaId repId) {
    Assert(voteSigMsg != nullptr);

    Assert(!processingSignaturesInTheBackground);
    Assert(expectedSeqNumber != 0);
    Assert(combinedValidSignatureMsg == nullptr);
    Assert(candidateCombinedSignatureMsg == nullptr);
    Assert(replicasInfo.count(repId) == 0);
    Assert(numberOfUnknownSignatures == 0);  // we can use this method to add at most one PART message

    // add voteSigMsg to replicasInfo
    RepInfo info = {voteSigMsg, SigState::Unknown};
    replicasInfo[repId] = info;

    // TODO(GG): do we want to verify the partial signature here?

    numberOfUnknownSignatures++;

    if (numOfRequiredSigs == 0)  // init numOfRequiredSigs
      numOfRequiredSigs = ExternalFunc::numberOfRequiredSignatures(context);
    
    // Assert(numberOfUnknownSignatures < numOfRequiredSigs);  // because numOfRequiredSigs > 1

    return true;
  }

  // init the FULL message directly (without sending to a background thread)
  bool initMsgWithCombinedSignature(FULL* combinedSigMsg) {
    Assert(!processingSignaturesInTheBackground);
    Assert(expectedSeqNumber != 0);
    Assert(combinedValidSignatureMsg == nullptr);
    Assert(candidateCombinedSignatureMsg == nullptr);

    IThresholdVerifier* const verifier = ExternalFunc::thresholdVerifier(context);
    bool succ = verifier->verify(
        (char*)&expectedDigest, sizeof(Digest), combinedSigMsg->signatureBody(), combinedSigMsg->signatureLen());
    Assert(succ);  // we should verify this signature when it is loaded

    combinedValidSignatureMsg = combinedSigMsg;

    return true;
  }

 protected:
  void trySendToBkThread() {
    Assert(combinedValidSignatureMsg == nullptr);
    //LOG_DEBUG(CNSUS, "On trySendToBkThread()");
    if (numOfRequiredSigs == 0)  // init numOfRequiredSigs
      numOfRequiredSigs = ExternalFunc::numberOfRequiredSignatures(context);
    LOG_DEBUG(CNSUS, "[CollectorOfThresholdSignatures] numOfRequiredSigs " << numOfRequiredSigs);
    LOG_DEBUG(CNSUS, "[CollectorOfThresholdSignatures] numberOfUnknownSignatures " << numberOfUnknownSignatures);

    if (processingSignaturesInTheBackground || expectedSeqNumber == 0) return;

    if (candidateCombinedSignatureMsg != nullptr) {
      processingSignaturesInTheBackground = true;

      CombinedSigVerificationJob* bkJob = new CombinedSigVerificationJob(ExternalFunc::thresholdVerifier(context),
                                                                         &ExternalFunc::incomingMsgsStorage(context),
                                                                         expectedSeqNumber,
                                                                         expectedView,
                                                                         expectedDigest,
                                                                         candidateCombinedSignatureMsg->signatureBody(),
                                                                         candidateCombinedSignatureMsg->signatureLen(),
                                                                         context);

      ExternalFunc::threadPool(context).add(bkJob);
    } else if (numberOfUnknownSignatures >= numOfRequiredSigs) {
      //LOG_DEBUG(CNSUS, "[Collector] numberOfUnknownSignatures >= numOfRequiredSigs");

      processingSignaturesInTheBackground = true;

      SignaturesProcessingJob* bkJob = new SignaturesProcessingJob(ExternalFunc::thresholdVerifier(context),
                                                                   &ExternalFunc::incomingMsgsStorage(context),
                                                                   expectedSeqNumber,
                                                                   expectedView,
                                                                   expectedDigest,
                                                                   numOfRequiredSigs,
                                                                   context);

      uint16_t numOfPartSigsInJob = 0;
      for (std::pair<uint16_t, RepInfo>&& info : replicasInfo) {
        if (info.second.state != SigState::Invalid) {
          //LOG_DEBUG(CNSUS, "From " << info.first << ": State is not invalid");
          auto msg = info.second.partialSigMsg;
          auto sig = msg->signatureBody();
          auto len = msg->signatureLen();
          const auto& span_context = msg->template spanContext<PART>();
          bkJob->add(info.first, sig, len, span_context);
          numOfPartSigsInJob++;
        }

        if (numOfPartSigsInJob == numOfRequiredSigs) break;
      }

      Assert(numOfPartSigsInJob == numOfRequiredSigs);
      //LOG_DEBUG(CNSUS, "[CollectorOfThresholdSignatures] number of required signatures is "<<numOfRequiredSigs);

      ExternalFunc::threadPool(context).add(bkJob);
    }
  }

  class SignaturesProcessingJob : public util::SimpleThreadPool::Job  // TODO(GG): include the replica Id (to identify
                                                                      // replicas that send bad combined signatures)
  {
   private:
    struct SigData {
      ReplicaId srcRepId;
      char* sigBody;
      uint16_t sigLength;
      std::string span_context;
    };

    IThresholdVerifier* const verifier;
    IncomingMsgsStorage* const repMsgsStorage;
    const SeqNum expectedSeqNumber;
    const ViewNum expectedView;
    const Digest expectedDigest;
    const uint16_t reqDataItems;
    SigData* const sigDataItems;

    uint16_t numOfDataItems;

    void* context;

    virtual ~SignaturesProcessingJob() {}

   public:
    SignaturesProcessingJob(IThresholdVerifier* const thresholdVerifier,
                            IncomingMsgsStorage* const replicaMsgsStorage,
                            SeqNum seqNum,
                            ViewNum view,
                            Digest& digest,
                            uint16_t numOfRequired,
                            void* cnt)
        : verifier{thresholdVerifier},
          repMsgsStorage{replicaMsgsStorage},
          expectedSeqNumber{seqNum},
          expectedView{view},
          expectedDigest{digest},
          reqDataItems{numOfRequired},
          sigDataItems{new SigData[numOfRequired]},
          numOfDataItems(0) {
      this->context = cnt;
    }

    void add(ReplicaId srcRepId, const char* sigBody, uint16_t sigLength, const std::string& span_context) {
      //LOG_DEBUG(CNSUS, "on add() from SignaturesProcessingJob");
      Assert(numOfDataItems < reqDataItems);

      SigData d;
      d.srcRepId = srcRepId;
      d.sigLength = sigLength;
      d.sigBody = (char*)std::malloc(sigLength);
      d.span_context = span_context;
      memcpy(d.sigBody, sigBody, sigLength);

      sigDataItems[numOfDataItems] = d;
      numOfDataItems++;
    }

    virtual void release() override {
      for (uint16_t i = 0; i < numOfDataItems; i++) {
        SigData& d = sigDataItems[i];
        std::free(d.sigBody);
      }

      delete[] sigDataItems;

      delete this;
    }

    virtual void execute() override {
      Assert(numOfDataItems == reqDataItems);
      //LOG_DEBUG(CNSUS, "on execute() from SignaturesProcessingJob");

      // TODO(GG): can utilize several threads (discuss with Alin)

      const uint16_t bufferSize = (uint16_t)verifier->requiredLengthForSignedData();
      char* const bufferForSigComputations = (char*)alloca(bufferSize);  // TODO(GG): check

      const auto& span_context_of_last_message =
          (reqDataItems - 1) ? sigDataItems[reqDataItems - 1].span_context : std::string{};
      {
        IThresholdAccumulator* acc = verifier->newAccumulator(false);

        for (uint16_t i = 0; i < reqDataItems; i++) {
          acc->add(sigDataItems[i].sigBody, sigDataItems[i].sigLength);
        }

        acc->setExpectedDigest(reinterpret_cast<unsigned char*>(expectedDigest.content()), DIGEST_SIZE);

        acc->getFullSignedData(bufferForSigComputations, bufferSize);

        verifier->release(acc);
      }

      bool succ = verifier->verify((char*)&expectedDigest, sizeof(Digest), bufferForSigComputations, bufferSize);

      //expectedDigest.print();

      if (!succ) {
        std::set<ReplicaId> replicasWithBadSigs;

        // TODO(GG): A clumsy way to do verification - find a better way ....

        IThresholdAccumulator* accWithVer = verifier->newAccumulator(true);
        accWithVer->setExpectedDigest(reinterpret_cast<unsigned char*>(expectedDigest.content()), DIGEST_SIZE);

        uint16_t currNumOfValidShares = 0;
        for (uint16_t i = 0; i < reqDataItems; i++) {
          uint16_t prevNumOfValidShares = currNumOfValidShares;

          currNumOfValidShares = (uint16_t)accWithVer->add(sigDataItems[i].sigBody, sigDataItems[i].sigLength);

          if (prevNumOfValidShares + 1 != currNumOfValidShares) replicasWithBadSigs.insert(sigDataItems[i].srcRepId);
        }

        if (replicasWithBadSigs.size() == 0) {
          // TODO(GG): print warning / error ??
        }

        verifier->release(accWithVer);

        auto iMsg(ExternalFunc::createInterCombinedSigFailed(expectedSeqNumber, expectedView, replicasWithBadSigs));
        repMsgsStorage->pushInternalMsg(std::move(iMsg));
      } else {
        auto iMsg(ExternalFunc::createInterCombinedSigSucceeded(
            expectedSeqNumber, expectedView, bufferForSigComputations, bufferSize, span_context_of_last_message));
        repMsgsStorage->pushInternalMsg(std::move(iMsg));
      }
    }
  };

  class CombinedSigVerificationJob : public util::SimpleThreadPool::Job {
   private:
    IThresholdVerifier* const verifier;
    IncomingMsgsStorage* const repMsgsStorage;
    const SeqNum expectedSeqNumber;
    const ViewNum expectedView;
    const Digest expectedDigest;
    char* const combinedSig;
    uint16_t combinedSigLen;
    void* context;

    virtual ~CombinedSigVerificationJob() {}

   public:
    CombinedSigVerificationJob(IThresholdVerifier* const thresholdVerifier,
                               IncomingMsgsStorage* const replicaMsgsStorage,
                               SeqNum seqNum,
                               ViewNum view,
                               Digest& digest,
                               char* const combinedSigBody,
                               uint16_t combinedSigLength,
                               void* cnt)
        : verifier{thresholdVerifier},
          repMsgsStorage{replicaMsgsStorage},
          expectedSeqNumber{seqNum},
          expectedView{view},
          expectedDigest{digest},
          combinedSig{(char*)std::malloc(combinedSigLength)},
          combinedSigLen{combinedSigLength} {
      memcpy(combinedSig, combinedSigBody, combinedSigLen);
      this->context = cnt;
    }

    virtual void release() override {
      std::free(combinedSig);

      delete this;
    }

    virtual void execute() override {
      bool succ = verifier->verify((char*)&expectedDigest, sizeof(Digest), combinedSig, combinedSigLen);
      auto iMsg(ExternalFunc::createInterVerifyCombinedSigResult(expectedSeqNumber, expectedView, succ));
      repMsgsStorage->pushInternalMsg(std::move(iMsg));
    }
  };

 protected:
  void* context;

  uint16_t numOfRequiredSigs = 0;

  void resetContent() {
    processingSignaturesInTheBackground = false;

    numberOfUnknownSignatures = 0;

    for (auto&& m : replicasInfo) {
      RepInfo& repInfo = m.second;
      delete repInfo.partialSigMsg;
      // delete repInfo.voteSigMsg;
    }
    replicasInfo.clear();

    if (combinedValidSignatureMsg != nullptr) delete combinedValidSignatureMsg;
    combinedValidSignatureMsg = nullptr;

    if (candidateCombinedSignatureMsg != nullptr) delete candidateCombinedSignatureMsg;
    candidateCombinedSignatureMsg = nullptr;

    if (expectedSeqNumber != 0)  // if we have expected values
    {
      expectedSeqNumber = 0;
      expectedView = 0;
      expectedDigest.makeZero();
    }
  }

  enum class SigState { Unknown = 0, Invalid };

  struct RepInfo {
    PART* partialSigMsg;
    // PART* voteSigMsg;
    SigState state;
  };

  bool processingSignaturesInTheBackground = false;

  uint16_t numberOfUnknownSignatures = 0;
  uint16_t numberOfVoteSignatures = 0;
  std::unordered_map<ReplicaId, RepInfo> replicasInfo;  // map from replica Id to RepInfo

  FULL* combinedValidSignatureMsg = nullptr;
  FULL* candidateCombinedSignatureMsg = nullptr;  // holds msg when expectedSeqNumber is not known yet

  SeqNum expectedSeqNumber = 0;
  ViewNum expectedView = 0;
  Digest expectedDigest;

  //struct VoteInfo {
  //  PART* voteSigMsg;
  //  SigState state;
  //};

  // bool processingSignaturesInTheBackground = false;

  // uint16_t numberOfUnknownSignatures = 0;
 // std::unordered_map<ReplicaId, VoteInfo> votersInfo;

  FULL* combinedVoteSignatureMsg = nullptr;

  // SeqNum expectedSeqNumber = 0;
  // ViewNum expectedView = 0;
  // Digest expectedDigest;
};

}  // namespace impl
}  // namespace bftEngine
