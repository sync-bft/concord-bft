// Concord
//
// Copyright (c) 2018-2020 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").  You may not use this product except in
// compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright notices and license terms. Your use of
// these subcomponents is subject to the terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#include "SeqNumInfo.hpp"
#include "InternalReplicaApi.hpp"
#include "messages/SignatureInternalMsgs.hpp"

namespace bftEngine {
namespace impl {

SeqNumInfo::SeqNumInfo()
    : replica(nullptr),
      prePrepareMsg(nullptr),
      proposalMsg(nullptr),
      prepareSigCollector(nullptr),
      commitMsgsCollector(nullptr),
      partialProofsSet(nullptr),
      primary(false),
      forcedCompleted(false),
      slowPathHasStarted(false),
      firstSeenFromPrimary(MinTime),
      timeOfLastInfoRequest(MinTime),
      commitUpdateTime(MinTime) {}

SeqNumInfo::~SeqNumInfo() {
  resetAndFree();

  delete prepareSigCollector;
  delete voteSigCollector;
  delete commitMsgsCollector;
  delete partialProofsSet;
}

void SeqNumInfo::resetAndFree() {
  delete prePrepareMsg;
  prePrepareMsg = nullptr;

  prepareSigCollector->resetAndFree();
  voteSigCollector->resetAndFree();
  commitMsgsCollector->resetAndFree();
  partialProofsSet->resetAndFree();

  primary = false;

  forcedCompleted = false;

  slowPathHasStarted = false;

  firstSeenFromPrimary = MinTime;
  timeOfLastInfoRequest = MinTime;
  commitUpdateTime = getMonotonicTime();  // TODO(GG): TBD
}

void SeqNumInfo::getAndReset(PrePrepareMsg*& outPrePrepare, PrepareFullMsg*& outCombinedValidSignatureMsg) {
  outPrePrepare = prePrepareMsg;
  prePrepareMsg = nullptr;

  prepareSigCollector->getAndReset(outCombinedValidSignatureMsg);

  resetAndFree();
}

bool SeqNumInfo::addMsg(PrePrepareMsg* m, bool directAdd) {
  if (prePrepareMsg != nullptr) return false;

  Assert(primary == false);
  Assert(!forcedCompleted);
  Assert(!prepareSigCollector->hasPartialMsgFromReplica(replica->getReplicasInfo().myId()));

  prePrepareMsg = m;

  // set expected
  Digest tmpDigest;
  Digest::calcCombination(m->digestOfRequests(), m->viewNumber(), m->seqNumber(), tmpDigest);
  if (!directAdd)
    prepareSigCollector->setExpected(m->seqNumber(), m->viewNumber(), tmpDigest);
  else
    prepareSigCollector->initExpected(m->seqNumber(), m->viewNumber(), tmpDigest);

  if (firstSeenFromPrimary == MinTime)  // TODO(GG): remove condition - TBD
    firstSeenFromPrimary = getMonotonicTime();

  return true;
}

bool SeqNumInfo::addSelfMsg(PrePrepareMsg* m, bool directAdd) {
  Assert(primary == false);
  Assert(replica->getReplicasInfo().myId() == replica->getReplicasInfo().primaryOfView(m->viewNumber()));
  Assert(!forcedCompleted);
  Assert(prePrepareMsg == nullptr);

  // Assert(me->id() == m->senderId()); // GG: incorrect assert - because after a view change it may has been sent by
  // another replica

  prePrepareMsg = m;
  primary = true;

  // set expected
  Digest tmpDigest;
  Digest::calcCombination(m->digestOfRequests(), m->viewNumber(), m->seqNumber(), tmpDigest);
  if (!directAdd)
    prepareSigCollector->setExpected(m->seqNumber(), m->viewNumber(), tmpDigest);
  else
    prepareSigCollector->initExpected(m->seqNumber(), m->viewNumber(), tmpDigest);

  if (firstSeenFromPrimary == MinTime)  // TODO(GG): remove condition - TBD
    firstSeenFromPrimary = getMonotonicTime();

  return true;
}

bool SeqNumInfo::addMsg(PreparePartialMsg* m) {
  Assert(replica->getReplicasInfo().myId() != m->senderId());
  Assert(!forcedCompleted);

  bool retVal = prepareSigCollector->addMsgWithPartialSignature(m, m->senderId());

  return retVal;
}

bool SeqNumInfo::addSelfMsg(PreparePartialMsg* m, bool directAdd) {
  Assert(replica->getReplicasInfo().myId() == m->senderId());
  Assert(!forcedCompleted);

  bool r;

  if (!directAdd)
    r = prepareSigCollector->addMsgWithPartialSignature(m, m->senderId());
  else
    r = prepareSigCollector->initMsgWithPartialSignature(m, m->senderId());

  Assert(r);

  return true;
}

bool SeqNumInfo::addMsg(PrepareFullMsg* m, bool directAdd) {
  Assert(directAdd || replica->getReplicasInfo().myId() != m->senderId());  // TODO(GG): TBD
  Assert(!forcedCompleted);

  bool retVal;
  if (!directAdd)
    retVal = prepareSigCollector->addMsgWithCombinedSignature(m);
  else
    retVal = prepareSigCollector->initMsgWithCombinedSignature(m);

  return retVal;
}

bool SeqNumInfo::addMsg(CommitPartialMsg* m) {
  Assert(replica->getReplicasInfo().myId() != m->senderId());  // TODO(GG): TBD
  Assert(!forcedCompleted);

  bool r = commitMsgsCollector->addMsgWithPartialSignature(m, m->senderId());

  if (r) commitUpdateTime = getMonotonicTime();

  return r;
}

bool SeqNumInfo::addSelfCommitPartialMsgAndDigest(CommitPartialMsg* m, Digest& commitDigest, bool directAdd) {
  Assert(replica->getReplicasInfo().myId() == m->senderId());
  Assert(!forcedCompleted);

  Digest tmpDigest;
  Digest::calcCombination(commitDigest, m->viewNumber(), m->seqNumber(), tmpDigest);
  bool r;
  if (!directAdd) {
    commitMsgsCollector->setExpected(m->seqNumber(), m->viewNumber(), tmpDigest);
    r = commitMsgsCollector->addMsgWithPartialSignature(m, m->senderId());
  } else {
    commitMsgsCollector->initExpected(m->seqNumber(), m->viewNumber(), tmpDigest);
    r = commitMsgsCollector->initMsgWithPartialSignature(m, m->senderId());
  }
  Assert(r);
  commitUpdateTime = getMonotonicTime();

  return true;
}

bool SeqNumInfo::addMsg(CommitFullMsg* m, bool directAdd) {
  Assert(directAdd || replica->getReplicasInfo().myId() != m->senderId());  // TODO(GG): TBD
  Assert(!forcedCompleted);

  bool r;
  if (!directAdd)
    r = commitMsgsCollector->addMsgWithCombinedSignature(m);
  else
    r = commitMsgsCollector->initMsgWithCombinedSignature(m);

  if (r) commitUpdateTime = getMonotonicTime();

  return r;
}

bool SeqNumInfo::canCommit() const {
  // return forcedCompleted || ((prePrepareMsg != nullptr) && voteSigCollector->isComplete());
  return voteSigCollector->votesCollected();
}

void SeqNumInfo::forceComplete() {
  Assert(!forcedCompleted);
  Assert(hasPrePrepareMsg());
  Assert(this->partialProofsSet->hasFullProof());

  forcedCompleted = true;
  commitUpdateTime = getMonotonicTime();
}

PrePrepareMsg* SeqNumInfo::getPrePrepareMsg() const { return prePrepareMsg; }

PrePrepareMsg* SeqNumInfo::getSelfPrePrepareMsg() const {
  if (primary) {
    return prePrepareMsg;
  }
  return nullptr;
}

PreparePartialMsg* SeqNumInfo::getSelfPreparePartialMsg() const {
  PreparePartialMsg* p = prepareSigCollector->getPartialMsgFromReplica(replica->getReplicasInfo().myId());
  return p;
}

PrepareFullMsg* SeqNumInfo::getValidPrepareFullMsg() const {
  return prepareSigCollector->getMsgWithValidCombinedSignature();
}

VoteMsg* SeqNumInfo::getSelfVoteMsg() const {
  VoteMsg* p = voteSigCollector->getVoteMsgFromReplica(replica->getReplicasInfo().myId());
  return p;
}

CommitPartialMsg* SeqNumInfo::getSelfCommitPartialMsg() const {
  CommitPartialMsg* p = commitMsgsCollector->getPartialMsgFromReplica(replica->getReplicasInfo().myId());
  return p;
}

CommitFullMsg* SeqNumInfo::getValidCommitFullMsg() const {
  return commitMsgsCollector->getMsgWithValidCombinedSignature();
}

bool SeqNumInfo::hasPrePrepareMsg() const { return (prePrepareMsg != nullptr); }

bool SeqNumInfo::isPrepared() const {
  return forcedCompleted || ((prePrepareMsg != nullptr) && prepareSigCollector->isComplete());
}

bool SeqNumInfo::isCommitted__gg() const {
  // TODO(GG): TBD - asserts on 'prepared'

  bool retVal = forcedCompleted || commitMsgsCollector->isComplete();
  return retVal;
}

bool SeqNumInfo::preparedOrHasPreparePartialFromReplica(ReplicaId repId) const {
  return isPrepared() || prepareSigCollector->hasPartialMsgFromReplica(repId);
}

bool SeqNumInfo::committedOrHasCommitPartialFromReplica(ReplicaId repId) const {
  return isCommitted__gg() || commitMsgsCollector->hasPartialMsgFromReplica(repId);
}

Time SeqNumInfo::getTimeOfFisrtRelevantInfoFromPrimary() const { return firstSeenFromPrimary; }

Time SeqNumInfo::getTimeOfLastInfoRequest() const { return timeOfLastInfoRequest; }

PartialProofsSet& SeqNumInfo::partialProofs() { return *partialProofsSet; }

void SeqNumInfo::startSlowPath() { slowPathHasStarted = true; }

bool SeqNumInfo::slowPathStarted() { return slowPathHasStarted; }

void SeqNumInfo::setTimeOfLastInfoRequest(Time t) { timeOfLastInfoRequest = t; }

void SeqNumInfo::onCompletionOfPrepareSignaturesProcessing(SeqNum seqNumber,
                                                           ViewNum viewNumber,
                                                           const std::set<ReplicaId>& replicasWithBadSigs) {
  prepareSigCollector->onCompletionOfSignaturesProcessing(seqNumber, viewNumber, replicasWithBadSigs);
}

void SeqNumInfo::onCompletionOfPrepareSignaturesProcessing(SeqNum seqNumber,
                                                           ViewNum viewNumber,
                                                           const char* combinedSig,
                                                           uint16_t combinedSigLen,
                                                           const std::string& span_context) {
  prepareSigCollector->onCompletionOfSignaturesProcessing(
      seqNumber, viewNumber, combinedSig, combinedSigLen, span_context);
}

void SeqNumInfo::onCompletionOfCombinedPrepareSigVerification(SeqNum seqNumber, ViewNum viewNumber, bool isValid) {
  prepareSigCollector->onCompletionOfCombinedSigVerification(seqNumber, viewNumber, isValid);
}

///////////////////////////////////////////////////////////////////////////////
// class SeqNumInfo::ExFuncForPrepareCollector
///////////////////////////////////////////////////////////////////////////////

PrepareFullMsg* SeqNumInfo::ExFuncForPrepareCollector::createCombinedSignatureMsg(void* context,
                                                                                  SeqNum seqNumber,
                                                                                  ViewNum viewNumber,
                                                                                  const char* const combinedSig,
                                                                                  uint16_t combinedSigLen,
                                                                                  const std::string& span_context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return PrepareFullMsg::create(
      viewNumber, seqNumber, r->getReplicasInfo().myId(), combinedSig, combinedSigLen, span_context);
}

InternalMessage SeqNumInfo::ExFuncForPrepareCollector::createInterCombinedSigFailed(
    SeqNum seqNumber, ViewNum viewNumber, std::set<uint16_t> replicasWithBadSigs) {
  return CombinedSigFailedInternalMsg(seqNumber, viewNumber, replicasWithBadSigs);
}

InternalMessage SeqNumInfo::ExFuncForPrepareCollector::createInterCombinedSigSucceeded(
    SeqNum seqNumber,
    ViewNum viewNumber,
    const char* combinedSig,
    uint16_t combinedSigLen,
    const std::string& span_context) {
  return CombinedSigSucceededInternalMsg(seqNumber, viewNumber, combinedSig, combinedSigLen, span_context);
}

InternalMessage SeqNumInfo::ExFuncForPrepareCollector::createInterVerifyCombinedSigResult(SeqNum seqNumber,
                                                                                          ViewNum viewNumber,
                                                                                          bool isValid) {
  return VerifyCombinedSigResultInternalMsg(seqNumber, viewNumber, isValid);
}

uint16_t SeqNumInfo::ExFuncForPrepareCollector::numberOfRequiredSignatures(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  const ReplicasInfo& info = r->getReplicasInfo();
  //return (uint16_t)((info.fVal() * 2) + info.cVal() + 1);
  return (uint16_t)(info.fVal()  + info.cVal() + 1);
}

IThresholdVerifier* SeqNumInfo::ExFuncForPrepareCollector::thresholdVerifier(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getThresholdVerifierForSlowPathCommit();
}

util::SimpleThreadPool& SeqNumInfo::ExFuncForPrepareCollector::threadPool(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getInternalThreadPool();
}

IncomingMsgsStorage& SeqNumInfo::ExFuncForPrepareCollector::incomingMsgsStorage(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getIncomingMsgsStorage();
}

///////////////////////////////////////////////////////////////////////////////
// class SeqNumInfo::ExFuncForCommitCollector
///////////////////////////////////////////////////////////////////////////////

CommitFullMsg* SeqNumInfo::ExFuncForCommitCollector::createCombinedSignatureMsg(void* context,
                                                                                SeqNum seqNumber,
                                                                                ViewNum viewNumber,
                                                                                const char* const combinedSig,
                                                                                uint16_t combinedSigLen,
                                                                                const std::string& span_context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return CommitFullMsg::create(
      viewNumber, seqNumber, r->getReplicasInfo().myId(), combinedSig, combinedSigLen, span_context);
}

InternalMessage SeqNumInfo::ExFuncForCommitCollector::createInterCombinedSigFailed(
    SeqNum seqNumber, ViewNum viewNumber, std::set<uint16_t> replicasWithBadSigs) {
  return CombinedCommitSigFailedInternalMsg(seqNumber, viewNumber, replicasWithBadSigs);
}

InternalMessage SeqNumInfo::ExFuncForCommitCollector::createInterCombinedSigSucceeded(SeqNum seqNumber,
                                                                                      ViewNum viewNumber,
                                                                                      const char* combinedSig,
                                                                                      uint16_t combinedSigLen,
                                                                                      const std::string& span_context) {
  return CombinedCommitSigSucceededInternalMsg(seqNumber, viewNumber, combinedSig, combinedSigLen, span_context);
}

InternalMessage SeqNumInfo::ExFuncForCommitCollector::createInterVerifyCombinedSigResult(SeqNum seqNumber,
                                                                                         ViewNum viewNumber,
                                                                                         bool isValid) {
  return VerifyCombinedCommitSigResultInternalMsg(seqNumber, viewNumber, isValid);
}

uint16_t SeqNumInfo::ExFuncForCommitCollector::numberOfRequiredSignatures(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  const ReplicasInfo& info = r->getReplicasInfo();
  //return (uint16_t)((info.fVal() * 2) + info.cVal() + 1);
  return (uint16_t)(info.fVal() + info.cVal() + 1);
}

IThresholdVerifier* SeqNumInfo::ExFuncForCommitCollector::thresholdVerifier(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getThresholdVerifierForSlowPathCommit();
}

util::SimpleThreadPool& SeqNumInfo::ExFuncForCommitCollector::threadPool(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getInternalThreadPool();
}

IncomingMsgsStorage& SeqNumInfo::ExFuncForCommitCollector::incomingMsgsStorage(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getIncomingMsgsStorage();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

void SeqNumInfo::init(SeqNumInfo& i, void* d) {
  void* context = d;
  InternalReplicaApi* r = (InternalReplicaApi*)context;

  i.replica = r;

  i.prepareSigCollector =
      new CollectorOfThresholdSignatures<PreparePartialMsg, PrepareFullMsg, ExFuncForPrepareCollector>(context);
  i.voteSigCollector =
      new CollectorOfThresholdSignatures<VoteMsg, VoteFullMsg, ExFuncForVoteCollector>(context);
  i.commitMsgsCollector =
      new CollectorOfThresholdSignatures<CommitPartialMsg, CommitFullMsg, ExFuncForCommitCollector>(context);
  i.partialProofsSet = new PartialProofsSet((InternalReplicaApi*)r);
}

//////////////////////////////////////////////////////////////////////
// Sync-HotStuff
//////////////////////////////////////////////////////////////////////

ProposalMsg* SeqNumInfo::getProposalMsg() const { return proposalMsg; }

bool SeqNumInfo::addMsg(ProposalMsg* m) {
  if (proposalMsg != nullptr) return false;

  Assert(primary == false);
  Assert(!forcedCompleted);
  Assert(!voteSigCollector->hasVoteMsgFromReplica(replica->getReplicasInfo().myId()));

  proposalMsg = m;

  Digest tmpDigest;
  Digest::calcCombination(m->digestOfRequestsSeqNum(), m->viewNumber(), m->seqNumber(), tmpDigest);
  voteSigCollector->setExpected(m->seqNumber(), m->viewNumber(), tmpDigest); // using backgroud thread

  if (firstSeenFromPrimary == MinTime)  // TODO(GG): remove condition - TBD
    firstSeenFromPrimary = getMonotonicTime();

  return true;
}

bool SeqNumInfo::addMsg(VoteMsg* m, bool directAdd) {
  Assert(replica->getReplicasInfo().myId() != m->senderId());
  Assert(!forcedCompleted);

  bool retVal;
  if (!directAdd)
    retVal = voteSigCollector->addMsgWithVoteSignature(m, replica->getReplicasInfo().myId());
  else
    retVal = voteSigCollector->initMsgWithVoteSignature(m, replica->getReplicasInfo().myId());
  
  return retVal;
}

bool SeqNumInfo::addSelfMsg(ProposalMsg* m, bool directAdd, bool primaryFirstMsg) {
  
  primary = primaryFirstMsg? true:primary;
//  Assert(primary == true);  // TODO(QF): in view change: could be the leader primary's first proposal before set to primary
  Assert(proposalMsg == nullptr);

  // Assert(me->id() == m->senderId()); // GG: incorrect assert - because after a view change it may has been sent by
  // another replica

  proposalMsg = m;
  //primary = true;

  // set expected
  Digest tmpDigest;
  Digest::calcCombination(m->digestOfRequestsSeqNum(), m->viewNumber(), m->seqNumber(), tmpDigest);
  if (!directAdd)
    voteSigCollector->setExpected(m->seqNumber(), m->viewNumber(), tmpDigest);
  else
    voteSigCollector->initExpected(m->seqNumber(), m->viewNumber(), tmpDigest);

  if (firstSeenFromPrimary == MinTime)  // TODO(GG): remove condition - TBD
    firstSeenFromPrimary = getMonotonicTime();

  return true;
}

bool SeqNumInfo::addSelfMsg(VoteMsg* m, bool directAdd) {
  Assert(replica->getReplicasInfo().myId() == m->senderId());
  Assert(!forcedCompleted);

  bool r;

  if (!directAdd)
    r = voteSigCollector->addMsgWithVoteSignature(m, m->senderId());
  else
    r = voteSigCollector->initMsgWithVoteSignature(m, m->senderId());

  Assert(r);

  return true;
}

bool SeqNumInfo::addCombinedSig(const char* sig, uint16_t len){
  combinedSig = sig;
  combinedSigLen = len;
  return true;
}

void SeqNumInfo::onCompletionOfVoteSignaturesProcessing(SeqNum seqNumber,
                                                        ViewNum viewNumber,
                                                        const std::set<ReplicaId>& replicasWithBadSigs) {
  voteSigCollector->onCompletionOfSignaturesProcessing(seqNumber, viewNumber, replicasWithBadSigs);
}

void SeqNumInfo::onCompletionOfVoteSignaturesProcessing(SeqNum seqNumber,
                                                        ViewNum viewNumber,
                                                        const char* combinedSig,
                                                        uint16_t combinedSigLen,
                                                        const std::string& span_context) {
  LOG_INFO(CNSUS,
           "seqNum information is as follows ["
               << seqNumber << "]");
  voteSigCollector->onCompletionOfSignaturesProcessing(
      seqNumber, viewNumber, combinedSig, combinedSigLen, span_context);
}

void SeqNumInfo::onCompletionOfCombinedVoteSigVerification(SeqNum seqNumber, ViewNum viewNumber, bool isValid) {
  voteSigCollector->onCompletionOfCombinedSigVerification(seqNumber, viewNumber, isValid);
}

///////////////////////////////////////////////////////////////////////////////
// class SeqNumInfo::ExFuncForVoteCollector
///////////////////////////////////////////////////////////////////////////////

VoteFullMsg* SeqNumInfo::ExFuncForVoteCollector::createCombinedSignatureMsg(void* context,
                                                                            SeqNum seqNumber,
                                                                            ViewNum viewNumber,
                                                                            const char* const combinedSig,
                                                                            uint16_t combinedSigLen,
                                                                            const std::string& span_context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return VoteFullMsg::create(
      viewNumber, seqNumber, r->getReplicasInfo().myId(), combinedSig, combinedSigLen, span_context);
}

InternalMessage SeqNumInfo::ExFuncForVoteCollector::createInterCombinedSigFailed(
    SeqNum seqNumber, ViewNum viewNumber, std::set<uint16_t> replicasWithBadSigs) {
  return CombinedSigFailedInternalMsg(seqNumber, viewNumber, replicasWithBadSigs);
}

InternalMessage SeqNumInfo::ExFuncForVoteCollector::createInterCombinedSigSucceeded(
    SeqNum seqNumber,
    ViewNum viewNumber,
    const char* combinedSig,
    uint16_t combinedSigLen,
    const std::string& span_context) {
  return CombinedSigSucceededInternalMsg(seqNumber, viewNumber, combinedSig, combinedSigLen, span_context);
}

InternalMessage SeqNumInfo::ExFuncForVoteCollector::createInterVerifyCombinedSigResult(SeqNum seqNumber,
                                                                                          ViewNum viewNumber,
                                                                                          bool isValid) {
  return VerifyCombinedSigResultInternalMsg(seqNumber, viewNumber, isValid);
}

uint16_t SeqNumInfo::ExFuncForVoteCollector::numberOfRequiredSignatures(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  const ReplicasInfo& info = r->getReplicasInfo();
  return (uint16_t)((info.fVal() * 2) + info.cVal() + 1);
}

IThresholdVerifier* SeqNumInfo::ExFuncForVoteCollector::thresholdVerifier(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getThresholdVerifierForSlowPathCommit();
}

util::SimpleThreadPool& SeqNumInfo::ExFuncForVoteCollector::threadPool(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getInternalThreadPool();
}

IncomingMsgsStorage& SeqNumInfo::ExFuncForVoteCollector::incomingMsgsStorage(void* context) {
  InternalReplicaApi* r = (InternalReplicaApi*)context;
  return r->getIncomingMsgsStorage();
}



}  // namespace impl
}  // namespace bftEngine
