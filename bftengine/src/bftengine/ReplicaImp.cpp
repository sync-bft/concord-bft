// Concord
//
// Copyright (c) 2018, 2019 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License").  You may not use this product except in
// compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright notices and license terms. Your use of
// these subcomponents is subject to the terms and conditions of the sub-component's license, as noted in the LICENSE
// file.

#include "ReplicaImp.hpp"
#include "Timers.hpp"
#include "assertUtils.hpp"
#include "Logger.hpp"
#include "ControllerWithSimpleHistory.hpp"
#include "DebugStatistics.hpp"
#include "SysConsts.hpp"
#include "ReplicaConfig.hpp"
#include "MsgsCommunicator.hpp"
#include "MsgHandlersRegistrator.hpp"
#include "ReplicaLoader.hpp"
#include "PersistentStorage.hpp"
#include "OpenTracing.hpp"

#include "messages/ClientRequestMsg.hpp"
#include "messages/PrePrepareMsg.hpp"
#include "messages/CheckpointMsg.hpp"
#include "messages/ClientReplyMsg.hpp"
#include "messages/PartialExecProofMsg.hpp"
#include "messages/FullExecProofMsg.hpp"
#include "messages/StartSlowCommitMsg.hpp"
#include "messages/ReqMissingDataMsg.hpp"
#include "messages/SimpleAckMsg.hpp"
#include "messages/ViewChangeMsg.hpp"
#include "messages/NewViewMsg.hpp"
#include "messages/PartialCommitProofMsg.hpp"
#include "messages/FullCommitProofMsg.hpp"
#include "messages/ReplicaStatusMsg.hpp"
#include "messages/AskForCheckpointMsg.hpp"

#include <string>
#include <type_traits>

using concordUtil::Timers;
using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

namespace bftEngine::impl {

void ReplicaImp::registerMsgHandlers() {
  msgHandlers_->registerMsgHandler(MsgCode::Checkpoint, bind(&ReplicaImp::messageHandler<CheckpointMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::CommitPartial,
                                   bind(&ReplicaImp::messageHandler<CommitPartialMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::CommitFull, bind(&ReplicaImp::messageHandler<CommitFullMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::FullCommitProof,
                                   bind(&ReplicaImp::messageHandler<FullCommitProofMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::NewView, bind(&ReplicaImp::messageHandler<NewViewMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::PrePrepare, bind(&ReplicaImp::messageHandler<PrePrepareMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::PartialCommitProof,
                                   bind(&ReplicaImp::messageHandler<PartialCommitProofMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::PartialExecProof,
                                   bind(&ReplicaImp::messageHandler<PartialExecProofMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::PreparePartial,
                                   bind(&ReplicaImp::messageHandler<PreparePartialMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::PrepareFull, bind(&ReplicaImp::messageHandler<PrepareFullMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::ReqMissingData,
                                   bind(&ReplicaImp::messageHandler<ReqMissingDataMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::SimpleAck, bind(&ReplicaImp::messageHandler<SimpleAckMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::StartSlowCommit,
                                   bind(&ReplicaImp::messageHandler<StartSlowCommitMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::ViewChange, bind(&ReplicaImp::messageHandler<ViewChangeMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::ClientRequest,
                                   bind(&ReplicaImp::messageHandler<ClientRequestMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::ReplicaStatus,
                                   bind(&ReplicaImp::messageHandler<ReplicaStatusMsg>, this, _1));

  msgHandlers_->registerMsgHandler(MsgCode::AskForCheckpoint,
                                   bind(&ReplicaImp::messageHandler<AskForCheckpointMsg>, this, _1));
}

template <typename T>
void ReplicaImp::messageHandler(MessageBase *msg) {
  if (validateMessage(msg) && !isCollectingState())
    onMessage<T>(static_cast<T *>(msg));
  else
    delete msg;
}

template <class T>
void onMessage(T *);

void ReplicaImp::send(MessageBase *m, NodeIdType dest) {
  // debug code begin
  if (m->type() == MsgCode::Checkpoint && static_cast<CheckpointMsg *>(m)->digestOfState().isZero())
    LOG_WARN(GL, "Debug: checkpoint digest is zero");
  // debug code end
  ReplicaBase::send(m, dest);
}

void ReplicaImp::sendAndIncrementMetric(MessageBase *m, NodeIdType id, CounterHandle &counterMetric) {
  send(m, id);
  counterMetric.Get().Inc();
}

void ReplicaImp::onReportAboutInvalidMessage(MessageBase *msg, const char *reason) {
  LOG_WARN(GL, "Received invalid message. " << KVLOG(msg->senderId(), msg->type(), reason));

  // TODO(GG): logic that deals with invalid messages (e.g., a node that sends invalid messages may have a problem (old
  // version,bug,malicious,...)).
}

template <>
void ReplicaImp::onMessage<ClientRequestMsg>(ClientRequestMsg *m) {
  // Analyzes the client message, and consider whether or not to operate/memorize it
  //this method includes the implementation the t0 phase of the pipeline (client-request-op)
  metric_received_client_requests_.Get().Inc(); //? // receive client request
  
  //collecting the basic infomation on the client request
  const NodeIdType senderId = m->senderId(); //store the sender id as constant
  const NodeIdType clientId = m->clientProxyId();// store the proxy id as constant
  const bool readOnly = m->isReadOnly();// evaluate whether the request is read-only
  const ReqId reqSeqNum = m->requestSeqNum();//store the request sequence number
  const uint8_t flags = m->flags(); // irrelevant

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_CID(m->getCid());// append temporary message
  LOG_DEBUG(GL, "Received ClientRequestMsg. " << KVLOG(clientId, reqSeqNum, flags, senderId));//debug

  // span: maybe relevant in execution con
  //not sure, something related to open tracingtext - unsure, will check
  const auto &span_context = m->spanContext<std::remove_pointer<decltype(m)>::type>();
  auto span = concordUtils::startChildSpanFromContext(span_context, "bft_client_request");
  span.setTag("rid", config_.replicaId);
  span.setTag("cid", m->getCid());
  span.setTag("seq_num", reqSeqNum);

  if (isCollectingState()) {//replica is collecting missing state
    LOG_INFO(GL,
             "ClientRequestMsg is ignored because this replica is collecting missing state from the other replicas. "
                 << KVLOG(reqSeqNum, clientId));
    delete m;// ignore this message
    return;
  }

  // check message validity
  const bool invalidClient = !isValidClient(clientId);
  const bool sentFromReplicaToNonPrimary = repsInfo->isIdOfReplica(senderId) && !isCurrentPrimary();

  // TODO(GG): more conditions (make sure that a request of client A cannot be generated by client B!=A)
  // only the primary replica will process the client request
  if (invalidClient || sentFromReplicaToNonPrimary) {//still evaluating validity
    std::ostringstream oss("ClientRequestMsg is invalid. ");//output invalid
    oss << KVLOG(invalidClient, sentFromReplicaToNonPrimary);
    onReportAboutInvalidMessage(m, oss.str().c_str());//report such message
    delete m;//delete message
    return;
  }

  if (readOnly) { // requests that don't need execution do not need consensus or futher the storage
    executeReadOnlyRequest(span, m); //execute the read only request
    delete m;//delete message
    return;
  }

  //ignore the method if not in the view is not active
  if (!currentViewIsActive()) {//ignore message if current view is inactive, not in steady state
    LOG_INFO(GL, "ClientRequestMsg is ignored because current view is inactive. " << KVLOG(reqSeqNum, clientId));
    delete m;//delete message
    return;
  }

  const ReqId seqNumberOfLastReply = seqNumberOfLastReplyToClient(clientId);//store the sequence number of last reply
  
  // TODO(QF): figure out the mechanism that is used to send/store/process requssts
  if (seqNumberOfLastReply < reqSeqNum) { // avoid msgs that are redundant //if request is up-to-date
    if (isCurrentPrimary()) {
      // TODO(GG): use config/parameter
      if (requestsQueueOfPrimary.size() >= 700) {//Too many messages, request queue is full
        LOG_WARN(GL,
                 "ClientRequestMsg dropped. Primary reqeust queue is full. "
                     << KVLOG(clientId, reqSeqNum, requestsQueueOfPrimary.size()));
        delete m;
        return;
      } //queue not full
      if (clientsManager->noPendingAndRequestCanBecomePending(clientId, reqSeqNum)) {
        LOG_DEBUG(CNSUS,
                  "Pushing to primary queue, request [" << reqSeqNum << "], client [" << clientId
                                                        << "], senderId=" << senderId);
        requestsQueueOfPrimary.push(m); // adds to the processing line //push request into queue
        primaryCombinedReqSize += m->size(); // increase the block size //update request size
        tryToSendPrePrepareMsg(true); // initialize the slow or fast path pipeline //try to enter T1
        return;
      } else {
        LOG_INFO(GL,
                 "ClientRequestMsg is ignored because: request is old, OR primary is current working on a request from "
                 "the same client. "
                     << KVLOG(clientId, reqSeqNum));
      }
    } else {  // not the current primary
      if (clientsManager->noPendingAndRequestCanBecomePending(clientId, reqSeqNum)) {
        clientsManager->addPendingRequest(clientId, reqSeqNum);

        // TODO(GG): add a mechanism that retransmits (otherwise we may start unnecessary view-change)
        send(m, currentPrimary()); // send the msg to the primary but why it will view-change?//send the message to current primary

        LOG_INFO(GL, "Forwarding ClientRequestMsg to the current primary. " << KVLOG(reqSeqNum, clientId));
      } else {
        LOG_INFO(GL,
                 "ClientRequestMsg is ignored because: request is old, OR primary is current working on a request "
                 "from the same client. "
                     << KVLOG(clientId, reqSeqNum));//ignore request if it is old
      }
    }
  } else if (seqNumberOfLastReply == reqSeqNum) { //receive the same msg
    LOG_DEBUG(
        GL,
        "ClientRequestMsg has already been executed: retransmitting reply to client. " << KVLOG(reqSeqNum, clientId));

    ClientReplyMsg *repMsg = clientsManager->allocateMsgWithLatestReply(clientId, currentPrimary());// inform the client it has been executed
    send(repMsg, clientId);//send reply message to client
    delete repMsg;
  } else {
    LOG_INFO(GL,
             "ClientRequestMsg is ignored because request sequence number is not monotonically increasing. "
                 << KVLOG(clientId, reqSeqNum, seqNumberOfLastReply));
  }

  delete m;
}

void ReplicaImp::tryToSendPrePrepareMsg(bool batchingLogic) {
  // This is the T1 phase
  //this method contains the implementation of t1 preprepare phase (only the current primary could execute this method)
  //it does not guarantee the sending of a preprepare msg as the primary queue of client request could be empty 
  Assert(isCurrentPrimary());// assert the replica is current primary
  Assert(currentViewIsActive()); // what is the scenario that it is current primary but the view is not active

  // used the sliding window mechanism to utilize bandwidth?
  // what is the difference between the last stable seqNum known to the primay and its last used seqNum?
    //is last stable seqNum the successfully executed?  
  if (primaryLastUsedSeqNum + 1 > lastStableSeqNum + kWorkWindowSize) { //exceed window size, should be expect workwindowsize to be 2?
    LOG_INFO(GL,
             "Will not send PrePrepare since next sequence number ["
                 << primaryLastUsedSeqNum + 1 << "] exceeds window threshold [" << lastStableSeqNum + kWorkWindowSize
                 << "]");
    return;
  }

  // the last used seqNum exceeds the seqNum that the primary replica could execute (concurrency means in parellel)
  if (primaryLastUsedSeqNum + 1 > lastExecutedSeqNum + config_.concurrencyLevel) {
    LOG_INFO(GL,
             "Will not send PrePrepare since next sequence number ["
                 << primaryLastUsedSeqNum + 1 << "] exceeds concurrency threshold ["
                 << lastExecutedSeqNum + config_.concurrencyLevel << "]");
    return;  // TODO(GG): should also be checked by the non-primary replicas
  }

  //queue: stores the client requests to be processed
  if (requestsQueueOfPrimary.empty()) return;

  //removed the processed(?) requests in the queue (stopped at the first request that could become pending)
  //the client manager shows pending status given the seqNum of a request -> indicates that the same request has been processed and marked as pending in the manager
  ClientRequestMsg *first = requestsQueueOfPrimary.front();//declare a pointer to the front most of queue
  while (first != nullptr &&
         !clientsManager->noPendingAndRequestCanBecomePending(first->clientProxyId(), first->requestSeqNum())) {
           //why is it at client side? requests from client
    SCOPED_MDC_CID(first->getCid());
    primaryCombinedReqSize -= first->size(); // the total size of combined/queued requests - size of executed requests
    delete first;//delete the message
    requestsQueueOfPrimary.pop();//pop another message
    first = (!requestsQueueOfPrimary.empty() ? requestsQueueOfPrimary.front() : nullptr);//update first/ assign new pointer to first
  }

  const size_t requestsInQueue = requestsQueueOfPrimary.size();//store size of requests in queue

  if (requestsInQueue == 0) return; //no requests to primary from the client //if empty, return

  AssertGE(primaryLastUsedSeqNum, lastExecutedSeqNum);

  uint64_t concurrentDiff = ((primaryLastUsedSeqNum + 1) - lastExecutedSeqNum); //what is this?
  uint64_t minBatchSize = 1;

  // update maxNumberOfPendingRequestsInRecentHistory (if needed)
  if (requestsInQueue > maxNumberOfPendingRequestsInRecentHistory)
    maxNumberOfPendingRequestsInRecentHistory = requestsInQueue;

  // TODO(GG): the batching logic should be part of the configuration - TBD.
  if (batchingLogic && (concurrentDiff >= 2)) {
    minBatchSize = concurrentDiff * batchingFactor;

    const size_t maxReasonableMinBatchSize = 350;  // TODO(GG): use param from configuration

    if (minBatchSize > maxReasonableMinBatchSize) minBatchSize = maxReasonableMinBatchSize;
  }

  if (requestsInQueue < minBatchSize) {
    LOG_INFO(GL,
             "Not enough client requests to fill the batch threshold size. " << KVLOG(minBatchSize, requestsInQueue));
    metric_not_enough_client_requests_event_.Get().Inc();
    return;
  }

  // update batchingFactor // update batchingFactor Do not quite understand what bacthing factor is
  if (((primaryLastUsedSeqNum + 1) % kWorkWindowSize) ==
      0)  // TODO(GG): do we want to update batchingFactor when the view is changed
  {
    const size_t aa = 4;  // TODO(GG): read from configuration
    batchingFactor = (maxNumberOfPendingRequestsInRecentHistory / aa);
    if (batchingFactor < 1) batchingFactor = 1;
    maxNumberOfPendingRequestsInRecentHistory = 0;
    LOG_DEBUG(GL, "PrePrepare batching factor updated. " << KVLOG(batchingFactor));
  }

  // because maxConcurrentAgreementsByPrimary <  MaxConcurrentFastPaths
  AssertLE((primaryLastUsedSeqNum + 1), lastExecutedSeqNum + MaxConcurrentFastPaths);

  CommitPath firstPath = controller->getCurrentFirstPath();

  AssertOR((config_.cVal != 0), (firstPath != CommitPath::FAST_WITH_THRESHOLD));

  controller->onSendingPrePrepare((primaryLastUsedSeqNum + 1), firstPath);

  //after the update of the client request queue, the next request is at the front position// not sure what span context is
  ClientRequestMsg *nextRequest = (!requestsQueueOfPrimary.empty() ? requestsQueueOfPrimary.front() : nullptr);
  //if the request queue is not empty, retrieve its span_context
  const auto &span_context = nextRequest ? nextRequest->spanContext<ClientRequestMsg>() : std::string{}; //what is at span_context?

  //create the preprepare msg
  PrePrepareMsg *pp = new PrePrepareMsg(
      config_.replicaId, curView, (primaryLastUsedSeqNum + 1), firstPath, span_context, primaryCombinedReqSize); // set configuration of the preprepare message
      // each replica has its unique id, the seqNum to be assigned, ?, ?, ?
  uint16_t initialStorageForRequests = pp->remainingSizeForRequests();

  //process the current client requests waiting in the queue
  while (nextRequest != nullptr) {//while there is still next request
    if (nextRequest->size() <= pp->remainingSizeForRequests()) {
      SCOPED_MDC_CID(nextRequest->getCid());
      if (clientsManager->noPendingAndRequestCanBecomePending(nextRequest->clientProxyId(),
                                                              nextRequest->requestSeqNum())) {
        pp->addRequest(nextRequest->body(), nextRequest->size()); // attach the current client request to the preprepare msg
        clientsManager->addPendingRequest(nextRequest->clientProxyId(), nextRequest->requestSeqNum()); // add the request (specified by its seqNum) to the pending in the client side
      }
      primaryCombinedReqSize -= nextRequest->size(); // total size of combined/queued requests - sizes of the requests to be processed 
    } else if (nextRequest->size() > initialStorageForRequests) {
      // The message is too big
      LOG_ERROR(GL,
                "Request was dropped because it exceeds maximum allowed size. "
                    << KVLOG(nextRequest->senderId(), nextRequest->size(), initialStorageForRequests));
    }
    delete nextRequest;
    requestsQueueOfPrimary.pop(); //pop another request
    nextRequest = (!requestsQueueOfPrimary.empty() ? requestsQueueOfPrimary.front() : nullptr);//set popped request to next request if satisfy condition
  }

  if (pp->numberOfRequests() == 0) { // no client requests to be processed// if no client requests at this time, return
    LOG_WARN(GL, "No client requests added to PrePrepare batch. Nothing to send.");
    return;
  }

  pp->finishAddingRequests(); // sign off the msg

  if (config_.debugStatisticsEnabled) {
    DebugStatistics::onSendPrePrepareMessage(pp->numberOfRequests(), requestsQueueOfPrimary.size());
  }
  primaryLastUsedSeqNum++; // the new preprepare msg used a seqNum
  SCOPED_MDC_SEQ_NUM(std::to_string(primaryLastUsedSeqNum));// append temporary key-value pairs to the log messages
  SCOPED_MDC_PATH(CommitPathToMDCString(firstPath));// append temporary key-value pairs to the log messages
  {
    LOG_INFO(CNSUS,
             "Sending PrePrepare with the following payload of the following correlation ids ["
                 << pp->getBatchCorrelationIdAsString() << "]");
  }
  SeqNumInfo &seqNumInfo = mainLog->get(primaryLastUsedSeqNum);
  seqNumInfo.addSelfMsg(pp);

  if (ps_) { // persistent storage -> writes seqNum & prepreparemsg & path mode
    ps_->beginWriteTran();
    ps_->setPrimaryLastUsedSeqNum(primaryLastUsedSeqNum);
    ps_->setPrePrepareMsgInSeqNumWindow(primaryLastUsedSeqNum, pp);
    if (firstPath == CommitPath::SLOW) ps_->setSlowStartedInSeqNumWindow(primaryLastUsedSeqNum, true);
    ps_->endWriteTran();
  }

  for (ReplicaId x : repsInfo->idsOfPeerReplicas()) {
    sendRetransmittableMsgToReplica(pp, x, primaryLastUsedSeqNum); // sends the preprepare msg to peer replicas - guesses: retransmittable means it can take ack?
  }

  if (firstPath == CommitPath::SLOW) {
    seqNumInfo.startSlowPath();  // initiate a slow path
    metric_slow_path_count_.Get().Inc();  // metric update
    sendPreparePartial(seqNumInfo);  // send a t2 msg (primary is also a replica) to itself? (the previous pp msg only sent to peers)
  } else {  // fast path
    sendPartialProof(seqNumInfo);
  }
}

template <typename T>
bool ReplicaImp::relevantMsgForActiveView(const T *msg) {
  //the method determines if the msg if relevant to the current view (view is active, msg view and replica view match, seqNum is within the window)

  //gets the seqNum and view number of the current msg
  const SeqNum msgSeqNum = msg->seqNumber(); //obtain sequence number
  const ViewNum msgViewNum = msg->viewNumber();//obtain view number

  const bool isCurrentViewActive = currentViewIsActive();// evaluate whether current view is active
  if (isCurrentViewActive && (msgViewNum == curView) && (msgSeqNum > strictLowerBoundOfSeqNums) &&
      (mainLog->insideActiveWindow(msgSeqNum))) {/ current view active, message relevant and valid
    AssertGT(msgSeqNum, lastStableSeqNum);
    AssertLE(msgSeqNum, lastStableSeqNum + kWorkWindowSize);

    return true;
  } else if (!isCurrentViewActive) {// current view is not active, ignore message
    LOG_INFO(GL,
             "My current view is not active, ignoring msg."
                 << KVLOG(curView, isCurrentViewActive, msg->senderId(), msgSeqNum, msgViewNum));
    return false;
  } else {
    const SeqNum activeWindowStart = mainLog->currentActiveWindow().first;   // is the currentActiveWindow different with the the lastStableSeqNum?
    const SeqNum activeWindowEnd = mainLog->currentActiveWindow().second;   // each replica has one mainLog that keeps track of properties
    const bool myReplicaMayBeBehind = (curView < msgViewNum) || (msgSeqNum > activeWindowEnd); // the replica is behind in view or its window of the msg replica
    if (myReplicaMayBeBehind) {
      onReportAboutAdvancedReplica(msg->senderId(), msgSeqNum, msgViewNum);
      LOG_INFO(GL,
               "Msg is not relevant for my current view. The sending replica may be in advance."
                   << KVLOG(curView,
                            isCurrentViewActive,
                            msg->senderId(),
                            msgSeqNum,
                            msgViewNum,
                            activeWindowStart,
                            activeWindowEnd));
    } else {
      const bool msgReplicaMayBeBehind = (curView > msgViewNum) || (msgSeqNum + kWorkWindowSize < activeWindowStart); // otherwise

      if (msgReplicaMayBeBehind) {
        onReportAboutLateReplica(msg->senderId(), msgSeqNum, msgViewNum);
        LOG_INFO(
            GL,
            "Msg is not relevant for my current view. The sending replica may be behind." << KVLOG(curView,
                                                                                                   isCurrentViewActive,
                                                                                                   msg->senderId(),
                                                                                                   msgSeqNum,
                                                                                                   msgViewNum,
                                                                                                   activeWindowStart,
                                                                                                   activeWindowEnd));
      }
    }
    return false;
  }
}

template <>
void ReplicaImp::onMessage<PrePrepareMsg>(PrePrepareMsg *msg) { 
  // the method contains the implementation of processing a preparemsg 

  metric_received_pre_prepares_.Get().Inc(); //updates the metric
  const SeqNum msgSeqNum = msg->seqNumber();

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary key-value pairs to the log messages
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));// append temporary key-value pairs to the log messages
  LOG_DEBUG(GL, KVLOG(msg->senderId(), msg->size()));
  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "handle_bft_preprepare"); // span documents the basic properties related to the msg?
  span.setTag("rid", config_.replicaId);
  span.setTag("seq_num", msgSeqNum);

  // the repica is not in active view but the received msg is righteous
  // what is viewsManager?  
  if (!currentViewIsActive() && viewsManager->waitingForMsgs() && msgSeqNum > lastStableSeqNum) { 
    Assert(!msg->isNull());  // we should never send (and never accept) null PrePrepare message

    if (viewsManager->addPotentiallyMissingPP(msg, lastStableSeqNum)) { // the view is not active but the msg might be handy when in the correct view ?
      LOG_INFO(GL, "PrePrepare added to views manager. " << KVLOG(lastStableSeqNum));
      tryToEnterView(); // enter the view
    } else {
      LOG_INFO(GL, "PrePrepare discarded.");
    }

    return;  // TODO(GG): memory deallocation is confusing .....
  }

  bool msgAdded = false;

  // msg is relevant (view is active, msg view and replica view match, seqNum is within the window) & correct sender
  if (relevantMsgForActiveView(msg) && (msg->senderId() == currentPrimary())) {
    sendAckIfNeeded(msg, msg->senderId(), msgSeqNum); // send ack
    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);
    const bool slowStarted = (msg->firstPath() == CommitPath::SLOW || seqNumInfo.slowPathStarted());

    // For MDC it doesn't matter which type of fast path
    SCOPED_MDC_PATH(CommitPathToMDCString(slowStarted ? CommitPath::SLOW : CommitPath::OPTIMISTIC_FAST));
    if (seqNumInfo.addMsg(msg)) { // received msg added to the mainLog
      {
        LOG_INFO(CNSUS,
                 "PrePrepare with the following correlation IDs [" << msg->getBatchCorrelationIdAsString() << "]");
      }
      msgAdded = true;

      if (ps_) { //seems that each replica has a storage place to write info like received msg (if persistent memory is on)
        ps_->beginWriteTran(); // also writes the msg to the storage
        ps_->setPrePrepareMsgInSeqNumWindow(msgSeqNum, msg);
        if (slowStarted) ps_->setSlowStartedInSeqNumWindow(msgSeqNum, true);
        ps_->endWriteTran();
      }

      if (!slowStarted)  // TODO(GG): make sure we correctly handle a situation where StartSlowCommitMsg is handled
                         // before PrePrepareMsg
      {
        sendPartialProof(seqNumInfo);
      } else {
        seqNumInfo.startSlowPath(); // starts the slow path (recorded in its mainLog)
        metric_slow_path_count_.Get().Inc();
        sendPreparePartial(seqNumInfo); // sends its reply -> initiate the t2 phase
        ;
      }
    }
  }

  if (!msgAdded) delete msg;
}

void ReplicaImp::tryToStartSlowPaths() { 
  // not sure about the method functionality -> start slow paths? has to be run by the primary replica
  if (!isCurrentPrimary() || isCollectingState() || !currentViewIsActive()) // what is isCollectingState()? - should have found its implementation somewhere
    return;  // TODO(GG): consider to stop the related timer when this method is not needed (to avoid useless
             // invocations)

  const SeqNum minSeqNum = lastExecutedSeqNum + 1; // last executed + 1
  // the difference between lastExecutedSeqNum (executed) and lastStableSeqNum (received & the start in the window) 
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));

  if (minSeqNum > lastStableSeqNum + kWorkWindowSize) {
    LOG_INFO(GL,
             "Try to start slow path: minSeqNum > lastStableSeqNum + kWorkWindowSize."
                 << KVLOG(minSeqNum, lastStableSeqNum, kWorkWindowSize));
    return;
  }

  const SeqNum maxSeqNum = primaryLastUsedSeqNum; // last used seqNum

  AssertLE(maxSeqNum, lastStableSeqNum + kWorkWindowSize);
  AssertLE(minSeqNum, maxSeqNum + 1);

  if (minSeqNum > maxSeqNum) return;

  sendCheckpointIfNeeded();  // TODO(GG): TBD - do we want it here ?

  const Time currTime = getMonotonicTime();

  for (SeqNum i = minSeqNum; i <= maxSeqNum; i++) { // includes the seqNum hasn't committed but has been created
    SeqNumInfo &seqNumInfo = mainLog->get(i); // gets the seqNumInfo attached to the seqNum

    if (seqNumInfo.partialProofs().hasFullProof() ||                          // already has a full proof
        seqNumInfo.slowPathStarted() ||                                       // slow path has already started
        seqNumInfo.partialProofs().getSelfPartialCommitProof() == nullptr ||  // did not start a fast path (?)
        (!seqNumInfo.hasPrePrepareMsg()))                                     // hasn't started (msg could be packed with others?)
      continue;  // slow path is not needed

    const Time timeOfPartProof = seqNumInfo.partialProofs().getTimeOfSelfPartialProof();

    if (currTime - timeOfPartProof < milliseconds(controller->timeToStartSlowPathMilli())) break; // (?)

    SCOPED_MDC_SEQ_NUM(std::to_string(i));
    SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));
    LOG_INFO(CNSUS,
             "Primary initiates slow path for seqNum="
                 << i << " (currTime=" << duration_cast<microseconds>(currTime.time_since_epoch()).count()
                 << " timeOfPartProof=" << duration_cast<microseconds>(timeOfPartProof.time_since_epoch()).count()
                 << " threshold for degradation [" << controller->timeToStartSlowPathMilli() << "ms]");

    controller->onStartingSlowCommit(i);

    seqNumInfo.startSlowPath();
    metric_slow_path_count_.Get().Inc();

    if (ps_) { // stores the slow start status
      ps_->beginWriteTran();
      ps_->setSlowStartedInSeqNumWindow(i, true);
      ps_->endWriteTran();
    }

    // send StartSlowCommitMsg to all replicas

    StartSlowCommitMsg *startSlow = new StartSlowCommitMsg(config_.replicaId, curView, i);

    for (ReplicaId x : repsInfo->idsOfPeerReplicas()) {
      sendRetransmittableMsgToReplica(startSlow, x, i);
    }

    delete startSlow;

    sendPreparePartial(seqNumInfo); // send pp msg to itself?
  }
}

void ReplicaImp::tryToAskForMissingInfo() {
  if (!currentViewIsActive() || isCollectingState()) return;

  //maxSeqNumTransferredFromPrevViews: the max processed seqNum from previou view
  AssertLE(maxSeqNumTransferredFromPrevViews, lastStableSeqNum + kWorkWindowSize);

  const bool recentViewChange = (maxSeqNumTransferredFromPrevViews > lastStableSeqNum); // true -> in view change? how is the maxSeqNum... maintained?

  SeqNum minSeqNum = 0; // starting seqNum that needs to be recovered
  SeqNum maxSeqNum = 0; // ending seqNum

  // searchWindow: max num of msgs to be asked per time (?)
  if (!recentViewChange) { // no recent view change
    const int16_t searchWindow = 4;  // TODO(GG): TBD - read from configuration
    minSeqNum = lastExecutedSeqNum + 1;
    maxSeqNum = std::min(minSeqNum + searchWindow - 1, lastStableSeqNum + kWorkWindowSize);
  } else {
    const int16_t searchWindow = 32;  // TODO(GG): TBD - read from configuration 
    minSeqNum = lastStableSeqNum + 1;
    while (minSeqNum <= lastStableSeqNum + kWorkWindowSize) { // finds the first seqNum in the view hasn't been committed / needs to be retransimitted
      SeqNumInfo &seqNumInfo = mainLog->get(minSeqNum);
      if (!seqNumInfo.isCommitted__gg()) break;
      minSeqNum++;
    }
    maxSeqNum = std::min(minSeqNum + searchWindow - 1, lastStableSeqNum + kWorkWindowSize);
  }

  if (minSeqNum > lastStableSeqNum + kWorkWindowSize) return; // is it too relaxing?

  const Time curTime = getMonotonicTime();

  SeqNum lastRelatedSeqNum = 0;

  // TODO(GG): improve/optimize the following loops

  // find the largest seqNum that is missing
  for (SeqNum i = minSeqNum; i <= maxSeqNum; i++) {
    Assert(mainLog->insideActiveWindow(i)); // the seqNum is within the current view

    const SeqNumInfo &seqNumInfo = mainLog->get(i);

    Time t = seqNumInfo.getTimeOfFisrtRelevantInfoFromPrimary(); // the time when the seqNum msg enters primary (?) 
    // why it shouldn't the last relevant info (bc we need the waiting time of this seqNum

    const Time lastInfoRequest = seqNumInfo.getTimeOfLastInfoRequest();  // (?) what is info request

    if ((t < lastInfoRequest)) t = lastInfoRequest;

    if (t != MinTime && (t < curTime)) { // (? when this condition would fail)
      auto diffMilli = duration_cast<milliseconds>(curTime - t);
      if (diffMilli.count() >= dynamicUpperLimitOfRounds->upperLimit()) lastRelatedSeqNum = i; // the waiting time (diffMilli) exceeds a consenus round (its upper limit is dynamically determined)
    }
  }

  // sending the request for the missing msg

  for (SeqNum i = minSeqNum; i <= lastRelatedSeqNum; i++) {
    if (!recentViewChange)
      tryToSendReqMissingDataMsg(i);
    else
      tryToSendReqMissingDataMsg(i, true, currentPrimary());
  }
}

template <>
void ReplicaImp::onMessage<StartSlowCommitMsg>(StartSlowCommitMsg *msg) {
  // the method contains the implementation of reaction to a startslowcommit msg (sent by the primary)
  metric_received_start_slow_commits_.Get().Inc();
  const SeqNum msgSeqNum = msg->seqNumber();
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  LOG_INFO(GL, " ");

  // what exactly does span do ?
  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_start_slow_commit_msg");
  if (relevantMsgForActiveView(msg)) { // (view is active, msg view and replica view match, seqNum is within the window)
    sendAckIfNeeded(msg, currentPrimary(), msgSeqNum);

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);

    if (!seqNumInfo.slowPathStarted() && !seqNumInfo.isPrepared()) { // the seqNum hasn't slow started nor prepared - what isPrepared()? guess it already engages in the fast path? 
      LOG_INFO(GL, "Start slow path.");

      seqNumInfo.startSlowPath();  // start slow path (recorded to its mainLog)
      metric_slow_path_count_.Get().Inc(); // update metric

      if (ps_) { // writes in the persistent storage if has one
        ps_->beginWriteTran();
        ps_->setSlowStartedInSeqNumWindow(msgSeqNum, true);
        ps_->endWriteTran();
      }

      if (seqNumInfo.hasPrePrepareMsg() == false)
        tryToSendReqMissingDataMsg(msgSeqNum); // ask for t1 msg 
      else
        sendPreparePartial(seqNumInfo); // engages in t2
    }
  }

  delete msg;
}

void ReplicaImp::sendPartialProof(SeqNumInfo &seqNumInfo) { // fast path
  // the method contains the implementation of sending its (replica's) partial signature to the collector
  PartialProofsSet &partialProofs = seqNumInfo.partialProofs(); // can the replica get access to others' proofs in this stage -> security concern? (can it also be accessed at mainLog?)

  if (!seqNumInfo.hasPrePrepareMsg()) return; // only sent if the seqNum has its preprep msg

  PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();
  Digest &ppDigest = pp->digestOfRequests(); // what is in the digest of request
  const SeqNum seqNum = pp->seqNumber();

  if (!partialProofs.hasFullProof()) { // still in the sign-share phase of the fast path
    // send PartialCommitProofMsg to all collectors

    PartialCommitProofMsg *part = partialProofs.getSelfPartialCommitProof(); // creates the pointer to its own partial commit proof 

    if (part == nullptr) {
      IThresholdSigner *commitSigner = nullptr;
      CommitPath commitPath = pp->firstPath();

      AssertOR((config_.cVal != 0), (commitPath != CommitPath::FAST_WITH_THRESHOLD));

      if ((commitPath == CommitPath::FAST_WITH_THRESHOLD) && (config_.cVal > 0)) // determines which mode the fast path is at
        commitSigner = config_.thresholdSignerForCommit;
      else
        commitSigner = config_.thresholdSignerForOptimisticCommit;

      Digest tmpDigest;
      Digest::calcCombination(ppDigest, curView, seqNum, tmpDigest); // get the its own (?) digest of pp, view, seqNum and assign the val to tmpDigest

      const auto &span_context = pp->spanContext<std::remove_pointer<decltype(pp)>::type>();
      part = new PartialCommitProofMsg(
          config_.replicaId, curView, seqNum, commitPath, tmpDigest, commitSigner, span_context);
      partialProofs.addSelfMsgAndPPDigest(part, tmpDigest); // adds its own partial commit proof msg and digest of its copy of ppdigest
    }

    partialProofs.setTimeOfSelfPartialProof(getMonotonicTime());

    // send PartialCommitProofMsg (only if, from my point of view, at most MaxConcurrentFastPaths are in progress)
    if (seqNum <= lastExecutedSeqNum + MaxConcurrentFastPaths) {
      // TODO(GG): improve the following code (use iterators instead of a simple array)
      int8_t numOfRouters = 0;
      ReplicaId routersArray[2];

      repsInfo->getCollectorsForPartialProofs(curView, seqNum, &numOfRouters, routersArray); //gets number of rounters and routers info at the current view & seqNum

      for (int i = 0; i < numOfRouters; i++) { // rounters are the collectors?
        ReplicaId router = routersArray[i];
        if (router != config_.replicaId) { // not me
          sendRetransmittableMsgToReplica(part, router, seqNum);
        }
      }
    }
  }
}

void ReplicaImp::sendPreparePartial(SeqNumInfo &seqNumInfo) { // t2 in slow path
  // the method contains the implementation of sending sign-share msg in slow path
  Assert(currentViewIsActive());

  if (seqNumInfo.getSelfPreparePartialMsg() == nullptr && // its own sign-share msg has not been created
      seqNumInfo.hasPrePrepareMsg() && // t1 is passed
      !seqNumInfo.isPrepared()) { // (?) t2 is not passed (could be possible that the current replica is in the minority) - still count as local observation?
    PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();

    AssertNE(pp, nullptr);

    LOG_DEBUG(GL, "Sending PreparePartialMsg. " << KVLOG(pp->seqNumber()));

    const auto &span_context = pp->spanContext<std::remove_pointer<decltype(pp)>::type>();
    PreparePartialMsg *p = PreparePartialMsg::create(curView,
                                                     pp->seqNumber(),
                                                     config_.replicaId,
                                                     pp->digestOfRequests(),
                                                     config_.thresholdSignerForSlowPathCommit,
                                                     span_context);
    seqNumInfo.addSelfMsg(p); // writes the self msg to the seqNum (stored in the mainLog)

    if (!isCurrentPrimary()) sendRetransmittableMsgToReplica(p, currentPrimary(), pp->seqNumber()); // sends the msg to the primary
  }
}

void ReplicaImp::sendCommitPartial(const SeqNum s) { //t4
  // this method contains the implementation of sending partial commit proof
  Assert(currentViewIsActive());
  Assert(mainLog->insideActiveWindow(s));
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(s));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));

  SeqNumInfo &seqNumInfo = mainLog->get(s);
  PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();

  Assert(seqNumInfo.isPrepared()); // has passed t3
  AssertNE(pp, nullptr);
  AssertEQ(pp->seqNumber(), s);

  if (seqNumInfo.committedOrHasCommitPartialFromReplica(config_.replicaId)) return;  // not needed

  LOG_DEBUG(CNSUS, "Sending CommitPartialMsg");

  Digest d;
  Digest::digestOfDigest(pp->digestOfRequests(), d); // pass the digest of pp to d

  auto prepareFullMsg = seqNumInfo.getValidPrepareFullMsg();

  CommitPartialMsg *c =
      CommitPartialMsg::create(curView,
                               s,
                               config_.replicaId,
                               d,
                               config_.thresholdSignerForSlowPathCommit,
                               prepareFullMsg->spanContext<std::remove_pointer<decltype(prepareFullMsg)>::type>());
  seqNumInfo.addSelfCommitPartialMsgAndDigest(c, d);

  if (!isCurrentPrimary()) sendRetransmittableMsgToReplica(c, currentPrimary(), s);
}

template <>
void ReplicaImp::onMessage<PartialCommitProofMsg>(PartialCommitProofMsg *msg) { // collector in sign-share phase (fast commit)
  //this method contains the implementation of collector processing partial commit proof from replicas
  metric_received_partial_commit_proofs_.Get().Inc();
  const SeqNum msgSeqNum = msg->seqNumber();
  const SeqNum msgView = msg->viewNumber();
  const NodeIdType msgSender = msg->senderId();
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  SCOPED_MDC_PATH(CommitPathToMDCString(msg->commitPath()));
  Assert(repsInfo->isIdOfPeerReplica(msgSender));
  Assert(repsInfo->isCollectorForPartialProofs(msgView, msgSeqNum)); // has to be the collector

  LOG_DEBUG(GL, KVLOG(msgSender, msg->size()));

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_partial_commit_proof_msg");
  if (relevantMsgForActiveView(msg)) { //(view is active, msg view and replica view match, seqNum is within the window)
    sendAckIfNeeded(msg, msgSender, msgSeqNum); // sends ack

    if (msgSeqNum > lastExecutedSeqNum) { // the msg to the seqNum hasn't been executed
      SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);
      PartialProofsSet &pps = seqNumInfo.partialProofs();

      if (pps.addMsg(msg)) { // adds received paritial proof to the msg
        return;
      }
    }
  }

  delete msg; // discard the msg (irrelavant or added)
  return;
}

template <>
void ReplicaImp::onMessage<FullCommitProofMsg>(FullCommitProofMsg *msg) { // the full commit phase (fast path)
  // the method implements the reactionary process after receiving full commit proof (sent by the collector)
  metric_received_full_commit_proofs_.Get().Inc();

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_full_commit_proof_msg");
  const SeqNum msgSeqNum = msg->seqNumber();
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(msg->seqNumber()));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::OPTIMISTIC_FAST));

  LOG_DEBUG(CNSUS, "Reached consensus, Received FullCommitProofMsg message");

  if (relevantMsgForActiveView(msg)) { // (view is active, msg view and replica view match, seqNum is within the window)
    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);
    PartialProofsSet &pps = seqNumInfo.partialProofs();

    if (!pps.hasFullProof() && pps.addMsg(msg))  // TODO(GG): consider to verify the signature in another thread
    // the seqNum hasn't processed the full proof phase and this full commit proof msg hasn't been added before
    {
      Assert(seqNumInfo.hasPrePrepareMsg()); // validates that it has preprepare msg

      seqNumInfo.forceComplete();  // TODO(GG): remove forceComplete() (we know that seqNumInfo is committed because
                                   // of the FullCommitProofMsg message)
                                   // what is this forceComplete?

      if (ps_) { // writes if it has persistent storage
        ps_->beginWriteTran();
        ps_->setFullCommitProofMsgInSeqNumWindow(msgSeqNum, msg);
        ps_->setForceCompletedInSeqNumWindow(msgSeqNum, true);
        ps_->endWriteTran();
      }

      if (msg->senderId() == config_.replicaId) sendToAllOtherReplicas(msg);

      const bool askForMissingInfoAboutCommittedItems =
          (msgSeqNum > lastExecutedSeqNum + config_.concurrencyLevel);  // TODO(GG): check/improve this logic
          // if the there is msg before this msg possibly unexecuted (the concurrencyLevel is a pretty relaxing condition - could be tighten, like to figure out the exact number request executing in parellel rn)

      auto execution_span = concordUtils::startChildSpan("bft_execute_committed_reqs", span);
      executeNextCommittedRequests(execution_span, askForMissingInfoAboutCommittedItems); // execute - is the span relevant to execution phase?
      return;
    } else {
      LOG_INFO(GL, "Failed to satisfy full proof requirements");
    }
  }

  delete msg; // discard msg
  return;
}

void ReplicaImp::onInternalMsg(FullCommitProofMsg *msg) { // on the upper of onMessage 
  if (isCollectingState() || (!currentViewIsActive()) || (curView != msg->viewNumber()) ||
      (!mainLog->insideActiveWindow(msg->seqNumber()))) { //not collecting missing data, view is active, msg view and replica view match, seqNum is within the window)
    delete msg;
    return;
  }
  onMessage(msg);
}

template <>
void ReplicaImp::onMessage<PreparePartialMsg>(PreparePartialMsg *msg) { // receiving t2 msg (slow path) (should only be the primary)
  metric_received_prepare_partials_.Get().Inc(); // updates metric
  const SeqNum msgSeqNum = msg->seqNumber();
  const ReplicaId msgSender = msg->senderId();

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));

  bool msgAdded = false;
  
  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_prepare_partial_msg");

  if (relevantMsgForActiveView(msg)) {
    Assert(isCurrentPrimary());

    sendAckIfNeeded(msg, msgSender, msgSeqNum);

    LOG_DEBUG(GL, "Received relevant PreparePartialMsg." << KVLOG(msgSender));

    controller->onMessage(msg);

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof(); // is it generating the full proof or the full proof is previously added to here

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg(); // what is this msg?

    PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg(); // what is this msg? t3?

    if (fcp != nullptr) { //full commit proof msg has already been created - current partial proofs received by the collector meets the criteria
      send(fcp, msgSender); //updates the sender (the sender is lagging)
    } else if (commitFull != nullptr) { // send t4 msg as replica? (should it happen?) 
                                        // or an alternative method to commit full proof -> proof is unable to be obtained but has receives=d enough commit partial msg 
      send(commitFull, msgSender);
    } else if (preFull != nullptr) { // send t3 msg as primary
      send(preFull, msgSender);
    } else { //the received t2 msg is useful
      msgAdded = seqNumInfo.addMsg(msg);
    }
  }

  if (!msgAdded) {
    LOG_DEBUG(GL,
              "Node " << config_.replicaId << " ignored the PreparePartialMsg from node " << msgSender << " (seqNumber "
                      << msgSeqNum << ")");
    delete msg; // discard
  }
}

template <>
void ReplicaImp::onMessage<CommitPartialMsg>(CommitPartialMsg *msg) { //receiving t4 msg (only by primary)
  metric_received_commit_partials_.Get().Inc();
  const SeqNum msgSeqNum = msg->seqNumber();
  const ReplicaId msgSender = msg->senderId();

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));

  bool msgAdded = false;

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_commit_partial_msg");
  if (relevantMsgForActiveView(msg)) {
    Assert(isCurrentPrimary());

    sendAckIfNeeded(msg, msgSender, msgSeqNum);

    LOG_DEBUG(GL, "Received CommitPartialMsg from node " << msgSender);

    //similar to the previous method

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();

    if (fcp != nullptr) {
      send(fcp, msgSender);
    } else if (commitFull != nullptr) {
      send(commitFull, msgSender);
    } else {
      msgAdded = seqNumInfo.addMsg(msg);
    }
  }

  if (!msgAdded) {
    LOG_INFO(GL, "Ignored CommitPartialMsg. " << KVLOG(msgSender));
    delete msg;
  }
}

template <>
void ReplicaImp::onMessage<PrepareFullMsg>(PrepareFullMsg *msg) { //receiving preparefull msg from the primary
  metric_received_prepare_fulls_.Get().Inc();
  const SeqNum msgSeqNum = msg->seqNumber();
  const ReplicaId msgSender = msg->senderId();
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));
  bool msgAdded = false;

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_preprare_full_msg");
  if (relevantMsgForActiveView(msg)) {
    sendAckIfNeeded(msg, msgSender, msgSeqNum);

    LOG_DEBUG(GL, "received PrepareFullMsg");

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();

    PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg();

    if (fcp != nullptr) {
      send(fcp, msgSender); // can non-primary send fcp msg? ig it doesn't break the protocol
    } else if (commitFull != nullptr) {
      send(commitFull, msgSender);
    } else if (preFull != nullptr) {
      // nop
    } else {
      msgAdded = seqNumInfo.addMsg(msg);
    }
  }

  if (!msgAdded) {
    LOG_DEBUG(GL, "Ignored PrepareFullMsg." << KVLOG(msgSender));
    delete msg;
  }
}
template <>
void ReplicaImp::onMessage<CommitFullMsg>(CommitFullMsg *msg) { //receiving commitfull msg from the primary
  metric_received_prepare_fulls_.Get().Inc();
  metric_received_commit_fulls_.Get().Inc();
  const SeqNum msgSeqNum = msg->seqNumber();
  const ReplicaId msgSender = msg->senderId();
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));
  bool msgAdded = false;

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_commit_full_msg");
  if (relevantMsgForActiveView(msg)) {
    sendAckIfNeeded(msg, msgSender, msgSeqNum);

    LOG_DEBUG(GL, "Received CommitFullMsg" << KVLOG(msgSender));

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();

    if (fcp != nullptr) {
      send(fcp,
           msgSender);  // TODO(GG): do we really want to send this message ? (msgSender already has a CommitFullMsg
                        // for the same seq number) - probably also has fcp msg already?
    } else if (commitFull != nullptr) {
      // nop
    } else {
      msgAdded = seqNumInfo.addMsg(msg);
    }
  }

  if (!msgAdded) {
    LOG_DEBUG(GL,
              "Node " << config_.replicaId << " ignored the CommitFullMsg from node " << msgSender << " (seqNumber "
                      << msgSeqNum << ")");
    delete msg;
  }
}

void ReplicaImp::onPrepareCombinedSigFailed(SeqNum seqNumber,
                                            ViewNum view,
                                            const std::set<uint16_t> &replicasWithBadSigs) {
  LOG_DEBUG(GL, KVLOG(seqNumber, view));

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_DEBUG(GL, "Dropping irrelevant signature." << KVLOG(seqNumber, view));

    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);

  seqNumInfo.onCompletionOfPrepareSignaturesProcessing(seqNumber, view, replicasWithBadSigs);

  // TODO(GG): add logic that handles bad replicas ...
}

void ReplicaImp::onPrepareCombinedSigSucceeded( // t3 - the primary sucessfully collects enough prepare partial msgs and could proceed 
    SeqNum seqNumber, ViewNum view, const char *combinedSig, uint16_t combinedSigLen, const std::string &span_context) {
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(seqNumber));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));
  LOG_DEBUG(GL, KVLOG(view));

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {  //irrelevant msg - what is this isCollectingState
    LOG_INFO(GL, "Not sending prepare full: Invalid state, view, or sequence number." << KVLOG(view, curView));
    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);

  seqNumInfo.onCompletionOfPrepareSignaturesProcessing(seqNumber, view, combinedSig, combinedSigLen, span_context); // generates PrepareFull msg 
                                                                                                                    // isPrepared() flag set to true (?)

  FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();

  PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg(); // why the preparefull msg doen't have fullprepareproof msg 

  AssertNE(preFull, nullptr); 

  if (fcp != nullptr) return;  // don't send if we already have FullCommitProofMsg
  LOG_DEBUG(CNSUS, "Sending prepare full" << KVLOG(view));
  if (ps_) {
    ps_->beginWriteTran();
    ps_->setPrepareFullMsgInSeqNumWindow(seqNumber, preFull);
    ps_->endWriteTran();
  }

  for (ReplicaId x : repsInfo->idsOfPeerReplicas()) sendRetransmittableMsgToReplica(preFull, x, seqNumber); //send preparefull msg to peers

  Assert(seqNumInfo.isPrepared());

  sendCommitPartial(seqNumber); // sends its reply as replica
}

void ReplicaImp::onPrepareVerifyCombinedSigResult(SeqNum seqNumber, ViewNum view, bool isValid) { 
  //between t3 and t4 - replica verifying the result of combined prepare partial sigs
  LOG_DEBUG(GL, KVLOG(view));

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_INFO(GL,
             "Not sending commit partial: Invalid state, view, or sequence number."
                 << KVLOG(seqNumber, view, curView, mainLog->insideActiveWindow(seqNumber)));
    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);

  seqNumInfo.onCompletionOfCombinedPrepareSigVerification(seqNumber, view, isValid); //vertification - result is assigned to isValid

  if (!isValid) return;  // TODO(GG): we should do something about the replica that sent this invalid message

  FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();

  if (fcp != nullptr) return;  // don't send if we already have FullCommitProofMsg

  Assert(seqNumInfo.isPrepared());

  if (ps_) {
    PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg(); 
      // two possibilities on why preFull is not null:
      // 1: primary added to the mainLog when seqNumInfo.onCompletionOfPrepareSignaturesProcessing(...) and all replicas share the same the mainLog
      // 2: replica added when executing onCompletionOfCombinedPrepareSigVerification()
    AssertNE(preFull, nullptr);
    ps_->beginWriteTran();
    ps_->setPrepareFullMsgInSeqNumWindow(seqNumber, preFull);
    ps_->endWriteTran();
  }

  sendCommitPartial(seqNumber); // moves to t4
}

void ReplicaImp::onCommitCombinedSigFailed(SeqNum seqNumber,
                                           ViewNum view,
                                           const std::set<uint16_t> &replicasWithBadSigs) {
  LOG_DEBUG(GL, KVLOG(seqNumber, view));

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) { // could isCollectingState() be that the primary is still collecting partials (haven't reached 2f+1)
    LOG_DEBUG(GL, "Invalid state, view, or sequence number." << KVLOG(seqNumber, view, curView));
    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);

  seqNumInfo.onCompletionOfCommitSignaturesProcessing(seqNumber, view, replicasWithBadSigs);

  // TODO(GG): add logic that handles bad replicas ...
}

void ReplicaImp::onCommitCombinedSigSucceeded( // primary on t5
    SeqNum seqNumber, ViewNum view, const char *combinedSig, uint16_t combinedSigLen, const std::string &span_context) {
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(seqNumber));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));
  LOG_DEBUG(GL, KVLOG(view));

  if (isCollectingState() || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_INFO(GL,
             "Not sending full commit: Invalid state, view, or sequence number."
                 << KVLOG(view, curView, mainLog->insideActiveWindow(seqNumber)));
    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);

  seqNumInfo.onCompletionOfCommitSignaturesProcessing(seqNumber, view, combinedSig, combinedSigLen, span_context);

  FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();
  CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();

  AssertNE(commitFull, nullptr);
  if (fcp != nullptr) return;  // ignore if we already have FullCommitProofMsg
  LOG_DEBUG(CNSUS, "Sending full commit" << KVLOG(view));
  if (ps_) {
    ps_->beginWriteTran();
    ps_->setCommitFullMsgInSeqNumWindow(seqNumber, commitFull);
    ps_->endWriteTran();
  }

  for (ReplicaId x : repsInfo->idsOfPeerReplicas()) sendRetransmittableMsgToReplica(commitFull, x, seqNumber); //send commit full to its peers

  Assert(seqNumInfo.isCommitted__gg());

  bool askForMissingInfoAboutCommittedItems = (seqNumber > lastExecutedSeqNum + config_.concurrencyLevel); // if true, indicates it skips msgs that needs to be committed

  auto span = concordUtils::startChildSpanFromContext(
      commitFull->spanContext<std::remove_pointer<decltype(commitFull)>::type>(), "bft_execute_committed_reqs");
  executeNextCommittedRequests(span, askForMissingInfoAboutCommittedItems); // primary executes the msg
}

void ReplicaImp::onCommitVerifyCombinedSigResult(SeqNum seqNumber, ViewNum view, bool isValid) { //replica verifying received commit full ?
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));
  SCOPED_MDC_SEQ_NUM(std::to_string(seqNumber));
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));

  LOG_DEBUG(GL, KVLOG(view));

  if (isCollectingState() || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_INFO(
        GL, "Invalid state, view, or sequence number." << KVLOG(view, curView, mainLog->insideActiveWindow(seqNumber)));
    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);

  seqNumInfo.onCompletionOfCombinedCommitSigVerification(seqNumber, view, isValid);

  if (!isValid) return;  // TODO(GG): we should do something about the replica that sent this invalid message

  Assert(seqNumInfo.isCommitted__gg());

  CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();
  AssertNE(commitFull, nullptr);
  if (ps_) {
    ps_->beginWriteTran();
    ps_->setCommitFullMsgInSeqNumWindow(seqNumber, commitFull);
    ps_->endWriteTran();
  }
  LOG_DEBUG(CNSUS, "Request commited, proceeding to try to execute" << KVLOG(view));
  auto span = concordUtils::startChildSpanFromContext(
      commitFull->spanContext<std::remove_pointer<decltype(commitFull)>::type>(), "bft_execute_committed_reqs");
  bool askForMissingInfoAboutCommittedItems = (seqNumber > lastExecutedSeqNum + config_.concurrencyLevel);
  executeNextCommittedRequests(span, askForMissingInfoAboutCommittedItems); //execute
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template <>
void ReplicaImp::onMessage<CheckpointMsg>(CheckpointMsg *msg) {
  metric_received_checkpoints_.Get().Inc();
  const ReplicaId msgSenderId = msg->senderId();
  const SeqNum msgSeqNum = msg->seqNumber();
  const Digest msgDigest = msg->digestOfState();
  const bool msgIsStable = msg->isStableState();
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  LOG_INFO(GL, "Received checkpoint message from node. " << KVLOG(msgSenderId, msg->size(), msgIsStable, msgDigest));
  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_checkpoint_msg");

  if ((msgSeqNum > lastStableSeqNum) && (msgSeqNum <= lastStableSeqNum + kWorkWindowSize)) {
    Assert(mainLog->insideActiveWindow(msgSeqNum));
    CheckpointInfo &checkInfo = checkpointsLog->get(msgSeqNum);
    bool msgAdded = checkInfo.addCheckpointMsg(msg, msg->senderId());

    if (msgAdded) {
      LOG_DEBUG(GL, "Added checkpoint message: " << KVLOG(msgSenderId));
    }

    if (checkInfo.isCheckpointCertificateComplete()) {
      AssertNE(checkInfo.selfCheckpointMsg(), nullptr);
      onSeqNumIsStable(msgSeqNum);

      return;
    }
  } else {
    delete msg;
  }

  bool askForStateTransfer = false;

  if (msgIsStable && msgSeqNum > lastExecutedSeqNum) {
    auto pos = tableOfStableCheckpoints.find(msgSenderId);
    if (pos == tableOfStableCheckpoints.end() || pos->second->seqNumber() < msgSeqNum) {
      if (pos != tableOfStableCheckpoints.end()) delete pos->second;
      CheckpointMsg *x = new CheckpointMsg(msgSenderId, msgSeqNum, msgDigest, msgIsStable);
      tableOfStableCheckpoints[msgSenderId] = x;

      LOG_INFO(GL, "Added stable Checkpoint message to tableOfStableCheckpoints: " << KVLOG(msgSenderId));

      if ((uint16_t)tableOfStableCheckpoints.size() >= config_.fVal + 1) {
        uint16_t numRelevant = 0;
        uint16_t numRelevantAboveWindow = 0;
        auto tableItrator = tableOfStableCheckpoints.begin();
        while (tableItrator != tableOfStableCheckpoints.end()) {
          if (tableItrator->second->seqNumber() <= lastExecutedSeqNum) {
            delete tableItrator->second;
            tableItrator = tableOfStableCheckpoints.erase(tableItrator);
          } else {
            numRelevant++;
            if (tableItrator->second->seqNumber() > lastStableSeqNum + kWorkWindowSize) numRelevantAboveWindow++;
            tableItrator++;
          }
        }
        AssertEQ(numRelevant, tableOfStableCheckpoints.size());

        LOG_DEBUG(GL, KVLOG(numRelevant, numRelevantAboveWindow));

        if (numRelevantAboveWindow >= config_.fVal + 1) {
          askForStateTransfer = true;
        } else if (numRelevant >= config_.fVal + 1) {
          Time timeOfLastCommit = MinTime;
          if (mainLog->insideActiveWindow(lastExecutedSeqNum))
            timeOfLastCommit = mainLog->get(lastExecutedSeqNum).lastUpdateTimeOfCommitMsgs();

          if ((getMonotonicTime() - timeOfLastCommit) >
              (milliseconds(timeToWaitBeforeStartingStateTransferInMainWindowMilli))) {
            askForStateTransfer = true;
          }
        }
      }
    }
  }

  if (askForStateTransfer) {
    LOG_INFO(GL, "Call to startCollectingState()");

    stateTransfer->startCollectingState();
  } else if (msgSeqNum > lastStableSeqNum + kWorkWindowSize) {
    onReportAboutAdvancedReplica(msgSenderId, msgSeqNum);
  } else if (msgSeqNum + kWorkWindowSize < lastStableSeqNum) {
    onReportAboutLateReplica(msgSenderId, msgSeqNum);
  }
}
/**
 * Is sent from a read-only replica
 */
template <>
void ReplicaImp::onMessage<AskForCheckpointMsg>(AskForCheckpointMsg *msg) {
  // metric_received_checkpoints_.Get().Inc(); // TODO [TK]

  LOG_INFO(GL, "Received AskForCheckpoint message: " << KVLOG(msg->senderId(), lastStableSeqNum));

  const CheckpointInfo &checkpointInfo = checkpointsLog->get(lastStableSeqNum);
  CheckpointMsg *checkpointMsg = checkpointInfo.selfCheckpointMsg();

  if (checkpointMsg == nullptr) {
    LOG_INFO(GL, "This replica does not have the current checkpoint. " << KVLOG(msg->senderId(), lastStableSeqNum));
  } else {
    // TODO [TK] check if already sent within a configurable time period
    auto destination = msg->senderId();
    LOG_INFO(GL, "Sending CheckpointMsg: " << KVLOG(destination));
    send(checkpointMsg, msg->senderId());
  }
}

bool ReplicaImp::handledByRetransmissionsManager(const ReplicaId sourceReplica,
                                                 const ReplicaId destReplica,
                                                 const ReplicaId primaryReplica,
                                                 const SeqNum seqNum,
                                                 const uint16_t msgType) {
  Assert(retransmissionsLogicEnabled);

  if (sourceReplica == destReplica) return false;

  const bool sourcePrimary = (sourceReplica == primaryReplica);

  if (sourcePrimary && ((msgType == MsgCode::PrePrepare) || (msgType == MsgCode::StartSlowCommit))) return true;

  const bool dstPrimary = (destReplica == primaryReplica);

  if (dstPrimary && ((msgType == MsgCode::PreparePartial) || (msgType == MsgCode::CommitPartial))) return true;

  //  TODO(GG): do we want to use acks for FullCommitProofMsg ?

  if (msgType == MsgCode::PartialCommitProof) {
    const bool destIsCollector =
        repsInfo->getCollectorsForPartialProofs(destReplica, curView, seqNum, nullptr, nullptr);
    if (destIsCollector) return true;
  }

  return false;
}

void ReplicaImp::sendAckIfNeeded(MessageBase *msg, const NodeIdType sourceNode, const SeqNum seqNum) {
  if (!retransmissionsLogicEnabled) return;

  if (!repsInfo->isIdOfPeerReplica(sourceNode)) return;

  if (handledByRetransmissionsManager(sourceNode, config_.replicaId, currentPrimary(), seqNum, msg->type())) {
    SimpleAckMsg *ackMsg = new SimpleAckMsg(seqNum, curView, config_.replicaId, msg->type());

    send(ackMsg, sourceNode);

    delete ackMsg;
  }
}

void ReplicaImp::sendRetransmittableMsgToReplica(MessageBase *msg,
                                                 ReplicaId destReplica,
                                                 SeqNum s,
                                                 bool ignorePreviousAcks) {
  send(msg, destReplica);

  if (!retransmissionsLogicEnabled) return;

  if (handledByRetransmissionsManager(config_.replicaId, destReplica, currentPrimary(), s, msg->type()))
    retransmissionsManager->onSend(destReplica, s, msg->type(), ignorePreviousAcks);
}

void ReplicaImp::onRetransmissionsTimer(Timers::Handle timer) {
  Assert(retransmissionsLogicEnabled);

  retransmissionsManager->tryToStartProcessing();
}

void ReplicaImp::onRetransmissionsProcessingResults(
    SeqNum relatedLastStableSeqNum,
    const ViewNum relatedViewNumber,
    const std::forward_list<RetSuggestion> *const suggestedRetransmissions) {
  Assert(retransmissionsLogicEnabled);

  if (isCollectingState() || (relatedViewNumber != curView) || (!currentViewIsActive())) return;
  if (relatedLastStableSeqNum + kWorkWindowSize <= lastStableSeqNum) return;

  const uint16_t myId = config_.replicaId;
  const uint16_t primaryId = currentPrimary();

  for (const RetSuggestion &s : *suggestedRetransmissions) {
    if ((s.msgSeqNum <= lastStableSeqNum) || (s.msgSeqNum > lastStableSeqNum + kWorkWindowSize)) continue;

    AssertNE(s.replicaId, myId);

    Assert(handledByRetransmissionsManager(myId, s.replicaId, primaryId, s.msgSeqNum, s.msgType));

    switch (s.msgType) {
      case MsgCode::PrePrepare: {
        SeqNumInfo &seqNumInfo = mainLog->get(s.msgSeqNum);
        PrePrepareMsg *msgToSend = seqNumInfo.getSelfPrePrepareMsg();
        AssertNE(msgToSend, nullptr);
        sendRetransmittableMsgToReplica(msgToSend, s.replicaId, s.msgSeqNum);
        LOG_DEBUG(GL,
                  "Replica " << myId << " retransmits to replica " << s.replicaId << " PrePrepareMsg with seqNumber "
                             << s.msgSeqNum);
      } break;
      case MsgCode::PartialCommitProof: {
        SeqNumInfo &seqNumInfo = mainLog->get(s.msgSeqNum);
        PartialCommitProofMsg *msgToSend = seqNumInfo.partialProofs().getSelfPartialCommitProof();
        AssertNE(msgToSend, nullptr);
        sendRetransmittableMsgToReplica(msgToSend, s.replicaId, s.msgSeqNum);
        LOG_DEBUG(GL,
                  "Replica " << myId << " retransmits to replica " << s.replicaId
                             << " PartialCommitProofMsg with seqNumber " << s.msgSeqNum);
      } break;
        /*  TODO(GG): do we want to use acks for FullCommitProofMsg ?
         */
      case MsgCode::StartSlowCommit: {
        StartSlowCommitMsg *msgToSend = new StartSlowCommitMsg(myId, curView, s.msgSeqNum);
        sendRetransmittableMsgToReplica(msgToSend, s.replicaId, s.msgSeqNum);
        delete msgToSend;
        LOG_DEBUG(GL,
                  "Replica " << myId << " retransmits to replica " << s.replicaId
                             << " StartSlowCommitMsg with seqNumber " << s.msgSeqNum);
      } break;
      case MsgCode::PreparePartial: {
        SeqNumInfo &seqNumInfo = mainLog->get(s.msgSeqNum);
        PreparePartialMsg *msgToSend = seqNumInfo.getSelfPreparePartialMsg();
        AssertNE(msgToSend, nullptr);
        sendRetransmittableMsgToReplica(msgToSend, s.replicaId, s.msgSeqNum);
        LOG_DEBUG(GL,
                  "Replica " << myId << " retransmits to replica " << s.replicaId
                             << " PreparePartialMsg with seqNumber " << s.msgSeqNum);
      } break;
      case MsgCode::PrepareFull: {
        SeqNumInfo &seqNumInfo = mainLog->get(s.msgSeqNum);
        PrepareFullMsg *msgToSend = seqNumInfo.getValidPrepareFullMsg();
        AssertNE(msgToSend, nullptr);
        sendRetransmittableMsgToReplica(msgToSend, s.replicaId, s.msgSeqNum);
        LOG_DEBUG(GL,
                  "Replica " << myId << " retransmits to replica " << s.replicaId << " PrepareFullMsg with seqNumber "
                             << s.msgSeqNum);
      } break;

      case MsgCode::CommitPartial: {
        SeqNumInfo &seqNumInfo = mainLog->get(s.msgSeqNum);
        CommitPartialMsg *msgToSend = seqNumInfo.getSelfCommitPartialMsg();
        AssertNE(msgToSend, nullptr);
        sendRetransmittableMsgToReplica(msgToSend, s.replicaId, s.msgSeqNum);
        LOG_DEBUG(GL,
                  "Replica " << myId << " retransmits to replica " << s.replicaId << " CommitPartialMsg with seqNumber "
                             << s.msgSeqNum);
      } break;

      case MsgCode::CommitFull: {
        SeqNumInfo &seqNumInfo = mainLog->get(s.msgSeqNum);
        CommitFullMsg *msgToSend = seqNumInfo.getValidCommitFullMsg();
        AssertNE(msgToSend, nullptr);
        sendRetransmittableMsgToReplica(msgToSend, s.replicaId, s.msgSeqNum);
        LOG_INFO(GL, "Retransmit CommitFullMsg: " << KVLOG(s.msgSeqNum, s.replicaId));
      } break;

      default:
        Assert(false);
    }
  }
}

template <>
void ReplicaImp::onMessage<ReplicaStatusMsg>(ReplicaStatusMsg *msg) {
  metric_received_replica_statuses_.Get().Inc();
  // TODO(GG): we need filter for msgs (to avoid denial of service attack) + avoid sending messages at a high rate.
  // TODO(GG): for some communication modules/protocols, we can also utilize information about
  // connection/disconnection.

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handling_status_report");
  const ReplicaId msgSenderId = msg->senderId();
  const SeqNum msgLastStable = msg->getLastStableSeqNum();
  const ViewNum msgViewNum = msg->getViewNumber();
  AssertEQ(msgLastStable % checkpointWindowSize, 0);

  LOG_DEBUG(GL, KVLOG(msgSenderId));

  /////////////////////////////////////////////////////////////////////////
  // Checkpoints
  /////////////////////////////////////////////////////////////////////////

  if (lastStableSeqNum > msgLastStable + kWorkWindowSize) {
    CheckpointMsg *checkMsg = checkpointsLog->get(lastStableSeqNum).selfCheckpointMsg();

    if (checkMsg == nullptr || !checkMsg->isStableState()) {
      // TODO(GG): warning
    } else {
      sendAndIncrementMetric(checkMsg, msgSenderId, metric_sent_checkpoint_msg_due_to_status_);
    }

    delete msg;
    return;
  } else if (msgLastStable > lastStableSeqNum + kWorkWindowSize) {
    tryToSendStatusReport();  // ask for help
  } else {
    // Send checkpoints that may be useful for msgSenderId
    const SeqNum beginRange =
        std::max(checkpointsLog->currentActiveWindow().first, msgLastStable + checkpointWindowSize);
    const SeqNum endRange = std::min(checkpointsLog->currentActiveWindow().second, msgLastStable + kWorkWindowSize);

    AssertEQ(beginRange % checkpointWindowSize, 0);

    if (beginRange <= endRange) {
      AssertLE(endRange - beginRange, kWorkWindowSize);

      for (SeqNum i = beginRange; i <= endRange; i = i + checkpointWindowSize) {
        CheckpointMsg *checkMsg = checkpointsLog->get(i).selfCheckpointMsg();
        if (checkMsg != nullptr) {
          sendAndIncrementMetric(checkMsg, msgSenderId, metric_sent_checkpoint_msg_due_to_status_);
        }
      }
    }
  }

  /////////////////////////////////////////////////////////////////////////
  // msgSenderId in older view
  /////////////////////////////////////////////////////////////////////////

  if (msgViewNum < curView) {
    ViewChangeMsg *myVC = viewsManager->getMyLatestViewChangeMsg();
    AssertNE(myVC, nullptr);  // because curView>0
    sendAndIncrementMetric(myVC, msgSenderId, metric_sent_viewchange_msg_due_to_status_);
  }

  /////////////////////////////////////////////////////////////////////////
  // msgSenderId needes information to enter view curView
  /////////////////////////////////////////////////////////////////////////

  else if ((msgViewNum == curView) && (!msg->currentViewIsActive())) {
    if (isCurrentPrimary() || (repsInfo->primaryOfView(curView) == msgSenderId))  // if the primary is involved
    {
      if (!isCurrentPrimary())  // I am not the primary of curView
      {
        // send ViewChangeMsg
        if (msg->hasListOfMissingViewChangeMsgForViewChange() &&
            msg->isMissingViewChangeMsgForViewChange(config_.replicaId)) {
          ViewChangeMsg *myVC = viewsManager->getMyLatestViewChangeMsg();
          AssertNE(myVC, nullptr);
          sendAndIncrementMetric(myVC, msgSenderId, metric_sent_viewchange_msg_due_to_status_);
        }
      } else  // I am the primary of curView
      {
        // send NewViewMsg
        if (!msg->currentViewHasNewViewMessage() && viewsManager->viewIsActive(curView)) {
          NewViewMsg *nv = viewsManager->getMyNewViewMsgForCurrentView();
          AssertNE(nv, nullptr);
          sendAndIncrementMetric(nv, msgSenderId, metric_sent_newview_msg_due_to_status_);
        }

        // send ViewChangeMsg
        if (msg->hasListOfMissingViewChangeMsgForViewChange() &&
            msg->isMissingViewChangeMsgForViewChange(config_.replicaId)) {
          ViewChangeMsg *myVC = viewsManager->getMyLatestViewChangeMsg();
          AssertNE(myVC, nullptr);
          sendAndIncrementMetric(myVC, msgSenderId, metric_sent_viewchange_msg_due_to_status_);
        }
        // TODO(GG): send all VC msgs that can help making progress (needed because the original senders may not send
        // the ViewChangeMsg msgs used by the primary)

        // TODO(GG): if viewsManager->viewIsActive(curView), we can send only the VC msgs which are really needed for
        // curView (see in ViewsManager)
      }

      if (viewsManager->viewIsActive(curView)) {
        if (msg->hasListOfMissingPrePrepareMsgForViewChange()) {
          for (SeqNum i = msgLastStable + 1; i <= msgLastStable + kWorkWindowSize; i++) {
            if (mainLog->insideActiveWindow(i) && msg->isMissingPrePrepareMsgForViewChange(i)) {
              PrePrepareMsg *prePrepareMsg = mainLog->get(i).getPrePrepareMsg();
              if (prePrepareMsg != nullptr) {
                sendAndIncrementMetric(prePrepareMsg, msgSenderId, metric_sent_preprepare_msg_due_to_status_);
              }
            }
          }
        }
      } else  // if I am also not in curView --- In this case we take messages from viewsManager
      {
        if (msg->hasListOfMissingPrePrepareMsgForViewChange()) {
          for (SeqNum i = msgLastStable + 1; i <= msgLastStable + kWorkWindowSize; i++) {
            if (msg->isMissingPrePrepareMsgForViewChange(i)) {
              PrePrepareMsg *prePrepareMsg =
                  viewsManager->getPrePrepare(i);  // TODO(GG): we can avoid sending misleading message by using the
                                                   // digest of the expected pre prepare message
              if (prePrepareMsg != nullptr) {
                sendAndIncrementMetric(prePrepareMsg, msgSenderId, metric_sent_preprepare_msg_due_to_status_);
              }
            }
          }
        }
      }
    }
  }

  /////////////////////////////////////////////////////////////////////////
  // msgSenderId is also in view curView
  /////////////////////////////////////////////////////////////////////////

  else if ((msgViewNum == curView) && msg->currentViewIsActive()) {
    if (isCurrentPrimary()) {
      if (viewsManager->viewIsActive(curView)) {
        SeqNum beginRange =
            std::max(lastStableSeqNum + 1,
                     msg->getLastExecutedSeqNum() + 1);  // Notice that after a view change, we don't have to pass the
                                                         // PrePrepare messages from the previous view. TODO(GG): verify
        SeqNum endRange = std::min(lastStableSeqNum + kWorkWindowSize, msgLastStable + kWorkWindowSize);

        for (SeqNum i = beginRange; i <= endRange; i++) {
          if (msg->isPrePrepareInActiveWindow(i)) continue;

          PrePrepareMsg *prePrepareMsg = mainLog->get(i).getSelfPrePrepareMsg();
          if (prePrepareMsg != nullptr) {
            sendAndIncrementMetric(prePrepareMsg, msgSenderId, metric_sent_preprepare_msg_due_to_status_);
          }
        }
      } else {
        tryToSendStatusReport();
      }
    } else {
      // TODO(GG): TBD
    }
  }

  /////////////////////////////////////////////////////////////////////////
  // msgSenderId is in a newer view curView
  /////////////////////////////////////////////////////////////////////////
  else {
    AssertGT(msgViewNum, curView);
    tryToSendStatusReport();
  }

  delete msg;
}

void ReplicaImp::tryToSendStatusReport(bool onTimer) {
  // TODO(GG): in some cases, we can limit the amount of such messages by using information about
  // connection/disconnection (from the communication module)
  // TODO(GG): explain that the current "Status Report" approch is relatively simple (it can be more efficient and
  // sophisticated).

  const Time currentTime = getMonotonicTime();

  const milliseconds milliSinceLastTime =
      duration_cast<milliseconds>(currentTime - lastTimeThisReplicaSentStatusReportMsgToAllPeerReplicas);

  if (milliSinceLastTime < milliseconds(minTimeBetweenStatusRequestsMilli)) return;

  const int64_t dynamicMinTimeBetweenStatusRequestsMilli =
      (int64_t)((double)dynamicUpperLimitOfRounds->upperLimit() * factorForMinTimeBetweenStatusRequestsMilli);

  if (milliSinceLastTime < milliseconds(dynamicMinTimeBetweenStatusRequestsMilli)) return;

  // TODO(GG): handle restart/pause !! (restart/pause may affect time measurements...)
  lastTimeThisReplicaSentStatusReportMsgToAllPeerReplicas = currentTime;

  const bool viewIsActive = currentViewIsActive();
  const bool hasNewChangeMsg = viewsManager->hasNewViewMessage(curView);
  const bool listOfPPInActiveWindow = viewIsActive;
  const bool listOfMissingVCMsg = !viewIsActive && !viewsManager->viewIsPending(curView);
  const bool listOfMissingPPMsg = !viewIsActive && viewsManager->viewIsPending(curView);

  ReplicaStatusMsg msg(config_.replicaId,
                       curView,
                       lastStableSeqNum,
                       lastExecutedSeqNum,
                       viewIsActive,
                       hasNewChangeMsg,
                       listOfPPInActiveWindow,
                       listOfMissingVCMsg,
                       listOfMissingPPMsg);

  if (listOfPPInActiveWindow) {
    const SeqNum start = lastStableSeqNum + 1;
    const SeqNum end = lastStableSeqNum + kWorkWindowSize;

    for (SeqNum i = start; i <= end; i++) {
      if (mainLog->get(i).hasPrePrepareMsg()) msg.setPrePrepareInActiveWindow(i);
    }
  }
  if (listOfMissingVCMsg) {
    for (ReplicaId i : repsInfo->idsOfPeerReplicas()) {
      if (!viewsManager->hasViewChangeMessageForFutureView(i)) msg.setMissingViewChangeMsgForViewChange(i);
    }
  } else if (listOfMissingPPMsg) {
    std::vector<SeqNum> missPP;
    if (viewsManager->getNumbersOfMissingPP(lastStableSeqNum, &missPP)) {
      for (SeqNum i : missPP) {
        AssertGT(i, lastStableSeqNum);
        AssertLE(i, lastStableSeqNum + kWorkWindowSize);
        msg.setMissingPrePrepareMsgForViewChange(i);
      }
    }
  }

  sendToAllOtherReplicas(&msg);
  if (!onTimer) metric_sent_status_msgs_not_due_timer_.Get().Inc();
}

template <>
void ReplicaImp::onMessage<ViewChangeMsg>(ViewChangeMsg *msg) {
  if (!viewChangeProtocolEnabled) {
    delete msg;
    return;
  }
  metric_received_view_changes_.Get().Inc();

  const ReplicaId generatedReplicaId =
      msg->idOfGeneratedReplica();  // Notice that generatedReplicaId may be != msg->senderId()
  AssertNE(generatedReplicaId, config_.replicaId);

  bool msgAdded = viewsManager->add(msg);

  LOG_INFO(GL, KVLOG(generatedReplicaId, msg->newView(), msg->lastStable(), msg->numberOfElements(), msgAdded));

  if (!msgAdded) return;

  // if the current primary wants to leave view
  if (generatedReplicaId == currentPrimary() && msg->newView() > curView) {
    LOG_INFO(GL, "Primary asks to leave view: " << KVLOG(generatedReplicaId, curView));
    MoveToHigherView(curView + 1);
  }

  ViewNum maxKnownCorrectView = 0;
  ViewNum maxKnownAgreedView = 0;
  viewsManager->computeCorrectRelevantViewNumbers(&maxKnownCorrectView, &maxKnownAgreedView);
  LOG_INFO(GL, KVLOG(maxKnownCorrectView, maxKnownAgreedView));

  if (maxKnownCorrectView > curView) {
    // we have at least f+1 view-changes with view number >= maxKnownCorrectView
    MoveToHigherView(maxKnownCorrectView);

    // update maxKnownCorrectView and maxKnownAgreedView
    // TODO(GG): consider to optimize (this part is not always needed)
    viewsManager->computeCorrectRelevantViewNumbers(&maxKnownCorrectView, &maxKnownAgreedView);
    LOG_INFO(GL,
             "Computed new view numbers. " << KVLOG(
                 maxKnownCorrectView, maxKnownAgreedView, viewsManager->viewIsActive(curView), lastAgreedView));
  }

  if (viewsManager->viewIsActive(curView)) return;  // return, if we are still in the previous view

  if (maxKnownAgreedView != curView) return;  // return, if we can't move to the new view yet

  // Replica now has at least 2f+2c+1 ViewChangeMsg messages with view  >= curView

  if (lastAgreedView < curView) {
    lastAgreedView = curView;
    metric_last_agreed_view_.Get().Set(lastAgreedView);
    timeOfLastAgreedView = getMonotonicTime();
  }

  tryToEnterView();
}

template <>
void ReplicaImp::onMessage<NewViewMsg>(NewViewMsg *msg) {
  if (!viewChangeProtocolEnabled) {
    delete msg;
    return;
  }
  metric_received_new_views_.Get().Inc();

  const ReplicaId senderId = msg->senderId();

  AssertNE(senderId, config_.replicaId);  // should be verified in ViewChangeMsg

  bool msgAdded = viewsManager->add(msg);

  LOG_INFO(GL, KVLOG(senderId, msg->newView(), msgAdded, curView, viewsManager->viewIsActive(curView)));

  if (!msgAdded) return;

  if (viewsManager->viewIsActive(curView)) return;  // return, if we are still in the previous view

  tryToEnterView();
}

void ReplicaImp::MoveToHigherView(ViewNum nextView) {
  Assert(viewChangeProtocolEnabled);
  AssertLT(curView, nextView);

  const bool wasInPrevViewNumber = viewsManager->viewIsActive(curView);

  LOG_INFO(GL, "**************** In MoveToHigherView " << KVLOG(curView, nextView, wasInPrevViewNumber));

  ViewChangeMsg *pVC = nullptr;

  if (!wasInPrevViewNumber) {
    pVC = viewsManager->getMyLatestViewChangeMsg();
    AssertNE(pVC, nullptr);
    pVC->setNewViewNumber(nextView);
  } else {
    std::vector<ViewsManager::PrevViewInfo> prevViewInfo;
    for (SeqNum i = lastStableSeqNum + 1; i <= lastStableSeqNum + kWorkWindowSize; i++) {
      SeqNumInfo &seqNumInfo = mainLog->get(i);

      if (seqNumInfo.getPrePrepareMsg() != nullptr) {
        ViewsManager::PrevViewInfo x;

        seqNumInfo.getAndReset(x.prePrepare, x.prepareFull);
        x.hasAllRequests = true;

        AssertNE(x.prePrepare, nullptr);
        AssertEQ(x.prePrepare->viewNumber(), curView);
        // (x.prepareFull!=nullptr) ==> (x.hasAllRequests==true)
        AssertOR(x.prepareFull == nullptr, x.hasAllRequests);
        // (x.prepareFull!=nullptr) ==> (x.prepareFull->viewNumber() == curView)
        AssertOR(x.prepareFull == nullptr, x.prepareFull->viewNumber() == curView);

        prevViewInfo.push_back(x);
      } else {
        seqNumInfo.resetAndFree();
      }
    }

    if (ps_) {
      ViewChangeMsg *myVC = (curView == 0 ? nullptr : viewsManager->getMyLatestViewChangeMsg());
      SeqNum stableLowerBoundWhenEnteredToView = viewsManager->stableLowerBoundWhenEnteredToView();
      const DescriptorOfLastExitFromView desc{
          curView, lastStableSeqNum, lastExecutedSeqNum, prevViewInfo, myVC, stableLowerBoundWhenEnteredToView};
      ps_->beginWriteTran();
      ps_->setDescriptorOfLastExitFromView(desc);
      ps_->clearSeqNumWindow();
      ps_->endWriteTran();
    }

    pVC = viewsManager->exitFromCurrentView(lastStableSeqNum, lastExecutedSeqNum, prevViewInfo);

    AssertNE(pVC, nullptr);
    pVC->setNewViewNumber(nextView);
  }

  curView = nextView;
  metric_view_.Get().Set(nextView);
  metric_current_primary_.Get().Set(curView % config_.numReplicas);

  auto newView = curView;
  auto newPrimary = currentPrimary();
  LOG_INFO(GL,
           "Sending view change message. "
               << KVLOG(newView, wasInPrevViewNumber, newPrimary, lastExecutedSeqNum, lastStableSeqNum));

  pVC->finalizeMessage();
  sendToAllOtherReplicas(pVC);
}

void ReplicaImp::GotoNextView() {
  // at this point we don't have f+1 ViewChangeMsg messages with view >= curView

  MoveToHigherView(curView + 1);

  // at this point we don't have enough ViewChangeMsg messages (2f+2c+1) to enter the new view (because 2f+2c+1 > f+1)
}

bool ReplicaImp::tryToEnterView() {
  Assert(!currentViewIsActive());

  std::vector<PrePrepareMsg *> prePreparesForNewView;

  bool enteredView =
      viewsManager->tryToEnterView(curView, lastStableSeqNum, lastExecutedSeqNum, &prePreparesForNewView);

  LOG_INFO(GL,
           "**************** Called viewsManager->tryToEnterView "
               << KVLOG(curView, lastStableSeqNum, lastExecutedSeqNum, enteredView));
  if (enteredView)
    onNewView(prePreparesForNewView);
  else
    tryToSendStatusReport();

  return enteredView;
}

void ReplicaImp::onNewView(const std::vector<PrePrepareMsg *> &prePreparesForNewView) {
  SeqNum firstPPSeq = 0;
  SeqNum lastPPSeq = 0;

  if (!prePreparesForNewView.empty()) {
    firstPPSeq = prePreparesForNewView.front()->seqNumber();
    lastPPSeq = prePreparesForNewView.back()->seqNumber();
  }

  LOG_INFO(GL,
           "**************** In onNewView " << KVLOG(curView,
                                                     prePreparesForNewView.size(),
                                                     firstPPSeq,
                                                     lastPPSeq,
                                                     lastStableSeqNum,
                                                     lastExecutedSeqNum,
                                                     viewsManager->stableLowerBoundWhenEnteredToView()));

  Assert(viewsManager->viewIsActive(curView));
  AssertGE(lastStableSeqNum, viewsManager->stableLowerBoundWhenEnteredToView());
  AssertGE(lastExecutedSeqNum, lastStableSeqNum);  // we moved to the new state, only after synchronizing the state

  timeOfLastViewEntrance = getMonotonicTime();  // TODO(GG): handle restart/pause

  NewViewMsg *newNewViewMsgToSend = nullptr;

  if (repsInfo->primaryOfView(curView) == config_.replicaId) {
    NewViewMsg *nv = viewsManager->getMyNewViewMsgForCurrentView();

    nv->finalizeMessage(*repsInfo);

    AssertEQ(nv->newView(), curView);

    newNewViewMsgToSend = nv;
  }

  if (prePreparesForNewView.empty()) {
    primaryLastUsedSeqNum = lastStableSeqNum;
    strictLowerBoundOfSeqNums = lastStableSeqNum;
    maxSeqNumTransferredFromPrevViews = lastStableSeqNum;
  } else {
    primaryLastUsedSeqNum = lastPPSeq;
    strictLowerBoundOfSeqNums = firstPPSeq - 1;
    maxSeqNumTransferredFromPrevViews = lastPPSeq;
  }

  if (ps_) {
    vector<ViewChangeMsg *> viewChangeMsgsForCurrentView = viewsManager->getViewChangeMsgsForCurrentView();
    NewViewMsg *newViewMsgForCurrentView = viewsManager->getNewViewMsgForCurrentView();

    bool myVCWasUsed = false;
    for (size_t i = 0; i < viewChangeMsgsForCurrentView.size() && !myVCWasUsed; i++) {
      AssertNE(viewChangeMsgsForCurrentView[i], nullptr);
      if (viewChangeMsgsForCurrentView[i]->idOfGeneratedReplica() == config_.replicaId) myVCWasUsed = true;
    }

    ViewChangeMsg *myVC = nullptr;
    if (!myVCWasUsed) {
      myVC = viewsManager->getMyLatestViewChangeMsg();
    } else {
      // debug/test: check that my VC should be included
      ViewChangeMsg *tempMyVC = viewsManager->getMyLatestViewChangeMsg();
      AssertNE(tempMyVC, nullptr);
      Digest d;
      tempMyVC->getMsgDigest(d);
      Assert(newViewMsgForCurrentView->includesViewChangeFromReplica(config_.replicaId, d));
    }

    DescriptorOfLastNewView viewDesc{curView,
                                     newViewMsgForCurrentView,
                                     viewChangeMsgsForCurrentView,
                                     myVC,
                                     viewsManager->stableLowerBoundWhenEnteredToView(),
                                     maxSeqNumTransferredFromPrevViews};

    ps_->beginWriteTran();
    ps_->setDescriptorOfLastNewView(viewDesc);
    ps_->setPrimaryLastUsedSeqNum(primaryLastUsedSeqNum);
    ps_->setStrictLowerBoundOfSeqNums(strictLowerBoundOfSeqNums);
  }

  const bool primaryIsMe = (config_.replicaId == repsInfo->primaryOfView(curView));

  for (size_t i = 0; i < prePreparesForNewView.size(); i++) {
    PrePrepareMsg *pp = prePreparesForNewView[i];
    AssertGE(pp->seqNumber(), firstPPSeq);
    AssertLE(pp->seqNumber(), lastPPSeq);
    AssertEQ(pp->firstPath(), CommitPath::SLOW);  // TODO(GG): don't we want to use the fast path?
    SeqNumInfo &seqNumInfo = mainLog->get(pp->seqNumber());

    if (ps_) {
      ps_->setPrePrepareMsgInSeqNumWindow(pp->seqNumber(), pp);
      ps_->setSlowStartedInSeqNumWindow(pp->seqNumber(), true);
    }

    if (primaryIsMe)
      seqNumInfo.addSelfMsg(pp);
    else
      seqNumInfo.addMsg(pp);

    seqNumInfo.startSlowPath();
    metric_slow_path_count_.Get().Inc();
  }

  if (ps_) ps_->endWriteTran();

  clientsManager->clearAllPendingRequests();

  // clear requestsQueueOfPrimary
  while (!requestsQueueOfPrimary.empty()) {
    auto msg = requestsQueueOfPrimary.front();
    primaryCombinedReqSize -= msg->size();
    requestsQueueOfPrimary.pop();
    delete msg;
  }

  // send messages

  if (newNewViewMsgToSend != nullptr) {
    LOG_INFO(GL, "**************** Sending NewView message to all replicas. " << KVLOG(curView));
    sendToAllOtherReplicas(newNewViewMsgToSend);
  }

  for (size_t i = 0; i < prePreparesForNewView.size(); i++) {
    PrePrepareMsg *pp = prePreparesForNewView[i];
    SeqNumInfo &seqNumInfo = mainLog->get(pp->seqNumber());
    sendPreparePartial(seqNumInfo);
  }

  LOG_INFO(GL, "**************** Start working in new view as primary. " << KVLOG(curView));

  controller->onNewView(curView, primaryLastUsedSeqNum);
  metric_current_active_view_.Get().Set(curView);
}

void ReplicaImp::sendCheckpointIfNeeded() {
  if (isCollectingState() || !currentViewIsActive()) return;

  const SeqNum lastCheckpointNumber = (lastExecutedSeqNum / checkpointWindowSize) * checkpointWindowSize;

  if (lastCheckpointNumber == 0) return;

  Assert(checkpointsLog->insideActiveWindow(lastCheckpointNumber));

  CheckpointInfo &checkInfo = checkpointsLog->get(lastCheckpointNumber);
  CheckpointMsg *checkpointMessage = checkInfo.selfCheckpointMsg();

  if (!checkpointMessage) {
    LOG_INFO(GL, "My Checkpoint message is missing. " << KVLOG(lastCheckpointNumber, lastExecutedSeqNum));
    return;
  }

  if (checkInfo.checkpointSentAllOrApproved()) return;

  // TODO(GG): 3 seconds, should be in configuration
  if ((getMonotonicTime() - checkInfo.selfExecutionTime()) >= 3s) {
    checkInfo.setCheckpointSentAllOrApproved();
    sendToAllOtherReplicas(checkpointMessage, true);
    return;
  }

  const SeqNum refSeqNumberForCheckpoint = lastCheckpointNumber + MaxConcurrentFastPaths;

  if (lastExecutedSeqNum < refSeqNumberForCheckpoint) return;

  if (mainLog->insideActiveWindow(lastExecutedSeqNum))  // TODO(GG): condition is needed ?
  {
    SeqNumInfo &seqNumInfo = mainLog->get(lastExecutedSeqNum);

    if (seqNumInfo.partialProofs().hasFullProof()) {
      checkInfo.tryToMarkCheckpointCertificateCompleted();

      Assert(checkInfo.isCheckpointCertificateComplete());

      onSeqNumIsStable(lastCheckpointNumber);

      checkInfo.setCheckpointSentAllOrApproved();

      return;
    }
  }

  checkInfo.setCheckpointSentAllOrApproved();
  sendToAllOtherReplicas(checkpointMessage, true);
}

void ReplicaImp::onTransferringCompleteImp(SeqNum newStateCheckpoint) {
  AssertEQ(newStateCheckpoint % checkpointWindowSize, 0);

  LOG_INFO(GL, KVLOG(newStateCheckpoint));

  if (ps_) {
    ps_->beginWriteTran();
  }

  if (newStateCheckpoint <= lastExecutedSeqNum) {
    LOG_DEBUG(GL,
              "Executing onTransferringCompleteImp(newStateCheckpoint) where newStateCheckpoint <= lastExecutedSeqNum");
    if (ps_) ps_->endWriteTran();
    return;
  }
  lastExecutedSeqNum = newStateCheckpoint;
  if (ps_) ps_->setLastExecutedSeqNum(lastExecutedSeqNum);
  if (config_.debugStatisticsEnabled) {
    DebugStatistics::onLastExecutedSequenceNumberChanged(lastExecutedSeqNum);
  }
  bool askAnotherStateTransfer = false;

  timeOfLastStateSynch = getMonotonicTime();  // TODO(GG): handle restart/pause

  clientsManager->loadInfoFromReservedPages();

  if (newStateCheckpoint > lastStableSeqNum + kWorkWindowSize) {
    const SeqNum refPoint = newStateCheckpoint - kWorkWindowSize;
    const bool withRefCheckpoint = (checkpointsLog->insideActiveWindow(refPoint) &&
                                    (checkpointsLog->get(refPoint).selfCheckpointMsg() != nullptr));

    onSeqNumIsStable(refPoint, withRefCheckpoint, true);
  }

  // newStateCheckpoint should be in the active window
  Assert(checkpointsLog->insideActiveWindow(newStateCheckpoint));

  // create and send my checkpoint
  Digest digestOfNewState;
  const uint64_t checkpointNum = newStateCheckpoint / checkpointWindowSize;
  stateTransfer->getDigestOfCheckpoint(checkpointNum, sizeof(Digest), (char *)&digestOfNewState);
  CheckpointMsg *checkpointMsg = new CheckpointMsg(config_.replicaId, newStateCheckpoint, digestOfNewState, false);
  CheckpointInfo &checkpointInfo = checkpointsLog->get(newStateCheckpoint);
  checkpointInfo.addCheckpointMsg(checkpointMsg, config_.replicaId);
  checkpointInfo.setCheckpointSentAllOrApproved();

  if (newStateCheckpoint > primaryLastUsedSeqNum) primaryLastUsedSeqNum = newStateCheckpoint;

  if (ps_) {
    ps_->setPrimaryLastUsedSeqNum(primaryLastUsedSeqNum);
    ps_->setCheckpointMsgInCheckWindow(newStateCheckpoint, checkpointMsg);
    ps_->endWriteTran();
  }
  metric_last_executed_seq_num_.Get().Set(lastExecutedSeqNum);

  sendToAllOtherReplicas(checkpointMsg);

  if ((uint16_t)tableOfStableCheckpoints.size() >= config_.fVal + 1) {
    uint16_t numOfStableCheckpoints = 0;
    auto tableItrator = tableOfStableCheckpoints.begin();
    while (tableItrator != tableOfStableCheckpoints.end()) {
      if (tableItrator->second->seqNumber() >= newStateCheckpoint) numOfStableCheckpoints++;

      if (tableItrator->second->seqNumber() <= lastExecutedSeqNum) {
        delete tableItrator->second;
        tableItrator = tableOfStableCheckpoints.erase(tableItrator);
      } else {
        tableItrator++;
      }
    }
    if (numOfStableCheckpoints >= config_.fVal + 1) onSeqNumIsStable(newStateCheckpoint);

    if ((uint16_t)tableOfStableCheckpoints.size() >= config_.fVal + 1) askAnotherStateTransfer = true;
  }

  if (askAnotherStateTransfer) {
    LOG_INFO(GL, "Call to startCollectingState()");

    stateTransfer->startCollectingState();
  }
}

void ReplicaImp::onSeqNumIsStable(SeqNum newStableSeqNum, bool hasStateInformation, bool oldSeqNum) {
  AssertOR(hasStateInformation, oldSeqNum);  // !hasStateInformation ==> oldSeqNum
  AssertEQ(newStableSeqNum % checkpointWindowSize, 0);

  if (newStableSeqNum <= lastStableSeqNum) return;

  LOG_INFO(GL,
           "New stable sequence number. " << KVLOG(lastStableSeqNum, newStableSeqNum, hasStateInformation, oldSeqNum));

  if (ps_) ps_->beginWriteTran();

  lastStableSeqNum = newStableSeqNum;
  metric_last_stable_seq_num_.Get().Set(lastStableSeqNum);

  if (ps_) ps_->setLastStableSeqNum(lastStableSeqNum);

  if (lastStableSeqNum > strictLowerBoundOfSeqNums) {
    strictLowerBoundOfSeqNums = lastStableSeqNum;
    if (ps_) ps_->setStrictLowerBoundOfSeqNums(strictLowerBoundOfSeqNums);
  }

  if (lastStableSeqNum > primaryLastUsedSeqNum) {
    primaryLastUsedSeqNum = lastStableSeqNum;
    if (ps_) ps_->setPrimaryLastUsedSeqNum(primaryLastUsedSeqNum);
  }

  mainLog->advanceActiveWindow(lastStableSeqNum + 1);

  checkpointsLog->advanceActiveWindow(lastStableSeqNum);

  if (hasStateInformation) {
    if (lastStableSeqNum > lastExecutedSeqNum) {
      lastExecutedSeqNum = lastStableSeqNum;
      if (ps_) ps_->setLastExecutedSeqNum(lastExecutedSeqNum);
      metric_last_executed_seq_num_.Get().Set(lastExecutedSeqNum);
      if (config_.debugStatisticsEnabled) {
        DebugStatistics::onLastExecutedSequenceNumberChanged(lastExecutedSeqNum);
      }
      clientsManager->loadInfoFromReservedPages();
    }

    CheckpointInfo &checkpointInfo = checkpointsLog->get(lastStableSeqNum);
    CheckpointMsg *checkpointMsg = checkpointInfo.selfCheckpointMsg();

    if (checkpointMsg == nullptr) {
      Digest digestOfState;
      const uint64_t checkpointNum = lastStableSeqNum / checkpointWindowSize;
      stateTransfer->getDigestOfCheckpoint(checkpointNum, sizeof(Digest), (char *)&digestOfState);
      checkpointMsg = new CheckpointMsg(config_.replicaId, lastStableSeqNum, digestOfState, true);
      checkpointInfo.addCheckpointMsg(checkpointMsg, config_.replicaId);
    } else {
      checkpointMsg->setStateAsStable();
    }

    if (!checkpointInfo.isCheckpointCertificateComplete()) checkpointInfo.tryToMarkCheckpointCertificateCompleted();
    Assert(checkpointInfo.isCheckpointCertificateComplete());

    if (ps_) {
      ps_->setCheckpointMsgInCheckWindow(lastStableSeqNum, checkpointMsg);
      ps_->setCompletedMarkInCheckWindow(lastStableSeqNum, true);
    }
  }

  if (ps_) ps_->endWriteTran();

  if (!oldSeqNum && currentViewIsActive() && (currentPrimary() == config_.replicaId) && !isCollectingState()) {
    tryToSendPrePrepareMsg();
  }
}

void ReplicaImp::tryToSendReqMissingDataMsg(SeqNum seqNumber, bool slowPathOnly, uint16_t destReplicaId) {
  if ((!currentViewIsActive()) || (seqNumber <= strictLowerBoundOfSeqNums) ||
      (!mainLog->insideActiveWindow(seqNumber)) || (!mainLog->insideActiveWindow(seqNumber))) {
    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);
  PartialProofsSet &partialProofsSet = seqNumInfo.partialProofs();

  const Time curTime = getMonotonicTime();

  {
    Time t = seqNumInfo.getTimeOfFisrtRelevantInfoFromPrimary();
    const Time lastInfoRequest = seqNumInfo.getTimeOfLastInfoRequest();

    if ((t < lastInfoRequest)) t = lastInfoRequest;

    if (t == MinTime && (t < curTime)) {
      auto diffMilli = duration_cast<milliseconds>(curTime - t);
      if (diffMilli.count() < dynamicUpperLimitOfRounds->upperLimit() / 4)  // TODO(GG): config
        return;
    }
  }

  seqNumInfo.setTimeOfLastInfoRequest(curTime);

  LOG_INFO(GL, "Try to request missing data. " << KVLOG(seqNumber, curView));

  ReqMissingDataMsg reqData(config_.replicaId, curView, seqNumber);

  const bool routerForPartialProofs = repsInfo->isCollectorForPartialProofs(curView, seqNumber);

  const bool routerForPartialPrepare = (currentPrimary() == config_.replicaId);

  const bool routerForPartialCommit = (currentPrimary() == config_.replicaId);

  const bool missingPrePrepare = (seqNumInfo.getPrePrepareMsg() == nullptr);
  const bool missingBigRequests = (!missingPrePrepare) && (!seqNumInfo.hasPrePrepareMsg());

  ReplicaId firstRepId = 0;
  ReplicaId lastRepId = config_.numReplicas - 1;
  if (destReplicaId != ALL_OTHER_REPLICAS) {
    firstRepId = destReplicaId;
    lastRepId = destReplicaId;
  }

  for (ReplicaId destRep = firstRepId; destRep <= lastRepId; destRep++) {
    if (destRep == config_.replicaId) continue;  // don't send to myself

    const bool destIsPrimary = (currentPrimary() == destRep);

    const bool missingPartialPrepare =
        (routerForPartialPrepare && (!seqNumInfo.preparedOrHasPreparePartialFromReplica(destRep)));

    const bool missingFullPrepare = !seqNumInfo.preparedOrHasPreparePartialFromReplica(destRep);

    const bool missingPartialCommit =
        (routerForPartialCommit && (!seqNumInfo.committedOrHasCommitPartialFromReplica(destRep)));

    const bool missingFullCommit = !seqNumInfo.committedOrHasCommitPartialFromReplica(destRep);

    const bool missingPartialProof = !slowPathOnly && routerForPartialProofs && !partialProofsSet.hasFullProof() &&
                                     !partialProofsSet.hasPartialProofFromReplica(destRep);

    const bool missingFullProof = !slowPathOnly && !partialProofsSet.hasFullProof();

    bool sendNeeded = missingPartialProof || missingPartialPrepare || missingFullPrepare || missingPartialCommit ||
                      missingFullCommit || missingFullProof;

    if (destIsPrimary && !sendNeeded) sendNeeded = missingBigRequests || missingPrePrepare;

    if (!sendNeeded) continue;

    reqData.resetFlags();

    if (destIsPrimary && missingPrePrepare) reqData.setPrePrepareIsMissing();

    if (missingPartialProof) reqData.setPartialProofIsMissing();
    if (missingPartialPrepare) reqData.setPartialPrepareIsMissing();
    if (missingFullPrepare) reqData.setFullPrepareIsMissing();
    if (missingPartialCommit) reqData.setPartialCommitIsMissing();
    if (missingFullCommit) reqData.setFullCommitIsMissing();
    if (missingFullProof) reqData.setFullCommitProofIsMissing();

    const bool slowPathStarted = seqNumInfo.slowPathStarted();

    if (slowPathStarted) reqData.setSlowPathHasStarted();

    auto destinationReplica = destRep;
    LOG_INFO(GL, "Send ReqMissingDataMsg. " << KVLOG(destinationReplica, seqNumber, reqData.getFlags()));

    send(&reqData, destRep);
    metric_sent_req_for_missing_data_.Get().Inc();
  }
}

template <>
void ReplicaImp::onMessage<ReqMissingDataMsg>(ReqMissingDataMsg *msg) {
  metric_received_req_missing_datas_.Get().Inc();
  const SeqNum msgSeqNum = msg->seqNumber();
  const ReplicaId msgSender = msg->senderId();
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));
  LOG_INFO(GL, "Received ReqMissingDataMsg. " << KVLOG(msgSender, msg->getFlags()));
  if ((currentViewIsActive()) && (msgSeqNum > strictLowerBoundOfSeqNums) && (mainLog->insideActiveWindow(msgSeqNum)) &&
      (mainLog->insideActiveWindow(msgSeqNum))) {
    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);

    if (config_.replicaId == currentPrimary()) {
      PrePrepareMsg *pp = seqNumInfo.getSelfPrePrepareMsg();
      if (msg->getPrePrepareIsMissing()) {
        if (pp != nullptr) {
          sendAndIncrementMetric(pp, msgSender, metric_sent_preprepare_msg_due_to_reqMissingData_);
        }
      }

      if (seqNumInfo.slowPathStarted() && !msg->getSlowPathHasStarted()) {
        StartSlowCommitMsg startSlowMsg(config_.replicaId, curView, msgSeqNum);
        sendAndIncrementMetric(&startSlowMsg, msgSender, metric_sent_startSlowPath_msg_due_to_reqMissingData_);
      }
    }

    if (msg->getPartialProofIsMissing()) {
      // TODO(GG): consider not to send if msgSender is not a collector

      PartialCommitProofMsg *pcf = seqNumInfo.partialProofs().getSelfPartialCommitProof();

      if (pcf != nullptr) {
        sendAndIncrementMetric(pcf, msgSender, metric_sent_partialCommitProof_msg_due_to_reqMissingData_);
      }
    }

    if (msg->getPartialPrepareIsMissing() && (currentPrimary() == msgSender)) {
      PreparePartialMsg *pr = seqNumInfo.getSelfPreparePartialMsg();

      if (pr != nullptr) {
        sendAndIncrementMetric(pr, msgSender, metric_sent_preparePartial_msg_due_to_reqMissingData_);
      }
    }

    if (msg->getFullPrepareIsMissing()) {
      PrepareFullMsg *pf = seqNumInfo.getValidPrepareFullMsg();

      if (pf != nullptr) {
        sendAndIncrementMetric(pf, msgSender, metric_sent_prepareFull_msg_due_to_reqMissingData_);
      }
    }

    if (msg->getPartialCommitIsMissing() && (currentPrimary() == msgSender)) {
      CommitPartialMsg *c = mainLog->get(msgSeqNum).getSelfCommitPartialMsg();
      if (c != nullptr) {
        sendAndIncrementMetric(c, msgSender, metric_sent_commitPartial_msg_due_to_reqMissingData_);
      }
    }

    if (msg->getFullCommitIsMissing()) {
      CommitFullMsg *c = mainLog->get(msgSeqNum).getValidCommitFullMsg();
      if (c != nullptr) {
        sendAndIncrementMetric(c, msgSender, metric_sent_commitFull_msg_due_to_reqMissingData_);
      }
    }

    if (msg->getFullCommitProofIsMissing() && seqNumInfo.partialProofs().hasFullProof()) {
      FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();
      sendAndIncrementMetric(fcp, msgSender, metric_sent_fullCommitProof_msg_due_to_reqMissingData_);
    }
  } else {
    LOG_INFO(GL, "Ignore the ReqMissingDataMsg message. " << KVLOG(msgSender));
  }

  delete msg;
}

void ReplicaImp::onViewsChangeTimer(Timers::Handle timer)  // TODO(GG): review/update logic
{
  Assert(viewChangeProtocolEnabled);

  if (isCollectingState()) return;

  Time currTime = getMonotonicTime();

  //////////////////////////////////////////////////////////////////////////////
  //
  //////////////////////////////////////////////////////////////////////////////

  if (autoPrimaryRotationEnabled && currentViewIsActive()) {
    const uint64_t timeout = (isCurrentPrimary() ? (autoPrimaryRotationTimerMilli)
                                                 : (autoPrimaryRotationTimerMilli + viewChangeTimeoutMilli));

    const uint64_t diffMilli = duration_cast<milliseconds>(currTime - timeOfLastViewEntrance).count();

    if (diffMilli > timeout) {
      LOG_INFO(GL,
               "**************** Initiate automatic view change in view="
                   << curView << " (" << diffMilli << " milli seconds after start working in the previous view)");

      GotoNextView();
      return;
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  //
  //////////////////////////////////////////////////////////////////////////////

  uint64_t viewChangeTimeout = viewChangeTimerMilli;
  if (autoIncViewChangeTimer && ((lastViewThatTransferredSeqNumbersFullyExecuted + 1) < curView)) {
    uint64_t factor = (curView - lastViewThatTransferredSeqNumbersFullyExecuted);
    viewChangeTimeout = viewChangeTimeout * factor;  // TODO(GG): review logic here
  }

  if (currentViewIsActive()) {
    if (isCurrentPrimary()) return;

    const Time timeOfEarliestPendingRequest = clientsManager->timeOfEarliestPendingRequest();

    const bool hasPendingRequest = (timeOfEarliestPendingRequest != MaxTime);

    if (!hasPendingRequest) return;

    const uint64_t diffMilli1 = duration_cast<milliseconds>(currTime - timeOfLastStateSynch).count();
    const uint64_t diffMilli2 = duration_cast<milliseconds>(currTime - timeOfLastViewEntrance).count();
    const uint64_t diffMilli3 = duration_cast<milliseconds>(currTime - timeOfEarliestPendingRequest).count();

    if ((diffMilli1 > viewChangeTimeout) && (diffMilli2 > viewChangeTimeout) && (diffMilli3 > viewChangeTimeout)) {
      LOG_INFO(GL,
               "**************** Ask to leave view=" << curView << " (" << diffMilli3
                                                     << " milli seconds after receiving a client request)");

      GotoNextView();
      return;
    }
  } else  // not currentViewIsActive()
  {
    if (lastAgreedView != curView) return;
    if (repsInfo->primaryOfView(lastAgreedView) == config_.replicaId) return;

    currTime = getMonotonicTime();
    const uint64_t diffMilli1 = duration_cast<milliseconds>(currTime - timeOfLastStateSynch).count();
    const uint64_t diffMilli2 = duration_cast<milliseconds>(currTime - timeOfLastAgreedView).count();

    if ((diffMilli1 > viewChangeTimeout) && (diffMilli2 > viewChangeTimeout)) {
      LOG_INFO(GL,
               "**************** Ask to jump to view=" << curView << " (" << diffMilli2
                                                       << " milliseconds after receiving 2f+2c+1 view change msgs)");
      GotoNextView();
      return;
    }
  }
}

void ReplicaImp::onStatusReportTimer(Timers::Handle timer) {
  tryToSendStatusReport(true);

#ifdef DEBUG_MEMORY_MSG
  MessageBase::printLiveMessages();
#endif
}

void ReplicaImp::onSlowPathTimer(Timers::Handle timer) {
  tryToStartSlowPaths();
  auto newPeriod = milliseconds(controller->slowPathsTimerMilli());
  timers_.reset(timer, newPeriod);
  metric_slow_path_timer_.Get().Set(controller->slowPathsTimerMilli());
}

void ReplicaImp::onInfoRequestTimer(Timers::Handle timer) {
  tryToAskForMissingInfo();
  auto newPeriod = milliseconds(dynamicUpperLimitOfRounds->upperLimit() / 2);
  timers_.reset(timer, newPeriod);
  metric_info_request_timer_.Get().Set(dynamicUpperLimitOfRounds->upperLimit() / 2);
}

template <>
void ReplicaImp::onMessage<SimpleAckMsg>(SimpleAckMsg *msg) {
  metric_received_simple_acks_.Get().Inc();
  SCOPED_MDC_SEQ_NUM(std::to_string(msg->seqNumber()));
  uint16_t relatedMsgType = (uint16_t)msg->ackData();  // TODO(GG): does this make sense ?
  if (retransmissionsLogicEnabled) {
    LOG_DEBUG(GL, KVLOG(msg->senderId(), relatedMsgType));
    retransmissionsManager->onAck(msg->senderId(), msg->seqNumber(), relatedMsgType);
  } else {
    LOG_WARN(GL, "Received Ack, but retransmissions not enabled. " << KVLOG(msg->senderId(), relatedMsgType));
  }

  delete msg;
}

void ReplicaImp::onMerkleExecSignature(ViewNum v, SeqNum s, uint16_t signatureLength, const char *signature) {
  Assert(false);
  // TODO(GG): use code from previous drafts
}

template <>
void ReplicaImp::onMessage<PartialExecProofMsg>(PartialExecProofMsg *m) {
  Assert(false);
  // TODO(GG): use code from previous drafts
}

void ReplicaImp::onReportAboutAdvancedReplica(ReplicaId reportedReplica, SeqNum seqNum, ViewNum viewNum) {
  // TODO(GG): simple implementation - should be improved
  tryToSendStatusReport();
}

void ReplicaImp::onReportAboutLateReplica(ReplicaId reportedReplica, SeqNum seqNum, ViewNum viewNum) {
  // TODO(GG): simple implementation - should be improved
  tryToSendStatusReport();
}

void ReplicaImp::onReportAboutAdvancedReplica(ReplicaId reportedReplica, SeqNum seqNum) {
  // TODO(GG): simple implementation - should be improved
  tryToSendStatusReport();
}

void ReplicaImp::onReportAboutLateReplica(ReplicaId reportedReplica, SeqNum seqNum) {
  // TODO(GG): simple implementation - should be improved
  tryToSendStatusReport();
}

ReplicaImp::ReplicaImp(const LoadedReplicaData &ld,
                       IRequestsHandler *requestsHandler,
                       IStateTransfer *stateTrans,
                       shared_ptr<MsgsCommunicator> msgsCommunicator,
                       shared_ptr<PersistentStorage> persistentStorage,
                       shared_ptr<MsgHandlersRegistrator> msgHandlers,
                       concordUtil::Timers &timers)
    : ReplicaImp(false,
                 ld.repConfig,
                 requestsHandler,
                 stateTrans,
                 ld.sigManager,
                 ld.repsInfo,
                 ld.viewsManager,
                 msgsCommunicator,
                 msgHandlers,
                 timers) {
  AssertNE(persistentStorage, nullptr);

  ps_ = persistentStorage;

  curView = ld.viewsManager->latestActiveView();
  lastAgreedView = curView;
  metric_view_.Get().Set(curView);
  metric_last_agreed_view_.Get().Set(lastAgreedView);
  metric_current_primary_.Get().Set(curView % config_.numReplicas);

  const bool inView = ld.viewsManager->viewIsActive(curView);

  primaryLastUsedSeqNum = ld.primaryLastUsedSeqNum;
  lastStableSeqNum = ld.lastStableSeqNum;
  metric_last_stable_seq_num_.Get().Set(lastStableSeqNum);
  lastExecutedSeqNum = ld.lastExecutedSeqNum;
  metric_last_executed_seq_num_.Get().Set(lastExecutedSeqNum);
  strictLowerBoundOfSeqNums = ld.strictLowerBoundOfSeqNums;
  maxSeqNumTransferredFromPrevViews = ld.maxSeqNumTransferredFromPrevViews;
  lastViewThatTransferredSeqNumbersFullyExecuted = ld.lastViewThatTransferredSeqNumbersFullyExecuted;

  mainLog->resetAll(lastStableSeqNum + 1);
  checkpointsLog->resetAll(lastStableSeqNum);

  bool viewIsActive = inView;
  LOG_INFO(GL,
           "Restarted ReplicaImp from persistent storage. " << KVLOG(curView,
                                                                     lastAgreedView,
                                                                     viewIsActive,
                                                                     primaryLastUsedSeqNum,
                                                                     lastStableSeqNum,
                                                                     lastExecutedSeqNum,
                                                                     strictLowerBoundOfSeqNums,
                                                                     maxSeqNumTransferredFromPrevViews,
                                                                     lastViewThatTransferredSeqNumbersFullyExecuted));

  if (inView) {
    const bool isPrimaryOfView = (repsInfo->primaryOfView(curView) == config_.replicaId);

    SeqNum s = ld.lastStableSeqNum;

    for (size_t i = 0; i < kWorkWindowSize; i++) {
      s++;
      Assert(mainLog->insideActiveWindow(s));

      const SeqNumData &e = ld.seqNumWinArr[i];

      if (!e.isPrePrepareMsgSet()) continue;

      // such properties should be verified by the code the loads the persistent data
      AssertEQ(e.getPrePrepareMsg()->seqNumber(), s);

      SeqNumInfo &seqNumInfo = mainLog->get(s);

      // add prePrepareMsg

      if (isPrimaryOfView)
        seqNumInfo.addSelfMsg(e.getPrePrepareMsg(), true);
      else
        seqNumInfo.addMsg(e.getPrePrepareMsg(), true);

      Assert(e.getPrePrepareMsg()->equals(*seqNumInfo.getPrePrepareMsg()));

      const CommitPath pathInPrePrepare = e.getPrePrepareMsg()->firstPath();

      // TODO(GG): check this when we load the data from disk
      AssertOR(pathInPrePrepare != CommitPath::SLOW, e.getSlowStarted());

      if (pathInPrePrepare != CommitPath::SLOW) {
        // add PartialCommitProofMsg

        PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();
        Assert(e.getPrePrepareMsg()->equals(*pp));
        Digest &ppDigest = pp->digestOfRequests();
        const SeqNum seqNum = pp->seqNumber();

        IThresholdSigner *commitSigner = nullptr;

        AssertOR((config_.cVal != 0), (pathInPrePrepare != CommitPath::FAST_WITH_THRESHOLD));

        if ((pathInPrePrepare == CommitPath::FAST_WITH_THRESHOLD) && (config_.cVal > 0))
          commitSigner = config_.thresholdSignerForCommit;
        else
          commitSigner = config_.thresholdSignerForOptimisticCommit;

        Digest tmpDigest;
        Digest::calcCombination(ppDigest, curView, seqNum, tmpDigest);

        PartialCommitProofMsg *p =
            new PartialCommitProofMsg(config_.replicaId, curView, seqNum, pathInPrePrepare, tmpDigest, commitSigner);
        seqNumInfo.partialProofs().addSelfMsgAndPPDigest(
            p,
            tmpDigest);  // TODO(GG): consider using a method that directly adds the message/digest (as in the
                         // examples below)
      }

      if (e.getSlowStarted()) {
        seqNumInfo.startSlowPath();

        // add PreparePartialMsg
        PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();
        PreparePartialMsg *p = PreparePartialMsg::create(curView,
                                                         pp->seqNumber(),
                                                         config_.replicaId,
                                                         pp->digestOfRequests(),
                                                         config_.thresholdSignerForSlowPathCommit);
        bool added = seqNumInfo.addSelfMsg(p, true);
        Assert(added);
      }

      if (e.isPrepareFullMsgSet()) {
        seqNumInfo.addMsg(e.getPrepareFullMsg(), true);

        Digest d;
        Digest::digestOfDigest(e.getPrePrepareMsg()->digestOfRequests(), d);
        CommitPartialMsg *c =
            CommitPartialMsg::create(curView, s, config_.replicaId, d, config_.thresholdSignerForSlowPathCommit);

        seqNumInfo.addSelfCommitPartialMsgAndDigest(c, d, true);
      }

      if (e.isCommitFullMsgSet()) {
        seqNumInfo.addMsg(e.getCommitFullMsg(), true);
        Assert(e.getCommitFullMsg()->equals(*seqNumInfo.getValidCommitFullMsg()));
      }

      if (e.isFullCommitProofMsgSet()) {
        PartialProofsSet &pps = seqNumInfo.partialProofs();
        bool added = pps.addMsg(e.getFullCommitProofMsg());  // TODO(GG): consider using a method that directly adds
                                                             // the message (as in the examples below)
        Assert(added);  // we should verify the relevant signature when it is loaded
        Assert(e.getFullCommitProofMsg()->equals(*pps.getFullProof()));
      }

      if (e.getForceCompleted()) seqNumInfo.forceComplete();
    }
  }

  Assert(ld.lastStableSeqNum % checkpointWindowSize == 0);

  for (SeqNum s = ld.lastStableSeqNum; s <= ld.lastStableSeqNum + kWorkWindowSize; s = s + checkpointWindowSize) {
    size_t i = (s - ld.lastStableSeqNum) / checkpointWindowSize;
    AssertLT(i, (sizeof(ld.checkWinArr) / sizeof(ld.checkWinArr[0])));
    const CheckData &e = ld.checkWinArr[i];

    Assert(checkpointsLog->insideActiveWindow(s));
    Assert(s == 0 ||                                                         // no checkpoints yet
           s > ld.lastStableSeqNum ||                                        // not stable
           e.isCheckpointMsgSet() ||                                         // if stable need to be set
           ld.lastStableSeqNum == ld.lastExecutedSeqNum - kWorkWindowSize);  // after ST last executed may be on the
                                                                             // upper working window boundary

    if (!e.isCheckpointMsgSet()) continue;

    CheckpointInfo &checkInfo = checkpointsLog->get(s);

    AssertEQ(e.getCheckpointMsg()->seqNumber(), s);
    AssertEQ(e.getCheckpointMsg()->senderId(), config_.replicaId);
    AssertOR((s != ld.lastStableSeqNum), e.getCheckpointMsg()->isStableState());

    checkInfo.addCheckpointMsg(e.getCheckpointMsg(), config_.replicaId);
    Assert(checkInfo.selfCheckpointMsg()->equals(*e.getCheckpointMsg()));

    if (e.getCompletedMark()) checkInfo.tryToMarkCheckpointCertificateCompleted();
  }

  if (ld.isExecuting) {
    Assert(viewsManager->viewIsActive(curView));
    Assert(mainLog->insideActiveWindow(lastExecutedSeqNum + 1));
    const SeqNumInfo &seqNumInfo = mainLog->get(lastExecutedSeqNum + 1);
    PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();
    AssertNE(pp, nullptr);
    AssertEQ(pp->seqNumber(), lastExecutedSeqNum + 1);
    AssertEQ(pp->viewNumber(), curView);
    AssertGT(pp->numberOfRequests(), 0);

    Bitmap b = ld.validRequestsThatAreBeingExecuted;
    size_t expectedValidRequests = 0;
    for (uint32_t i = 0; i < b.numOfBits(); i++) {
      if (b.get(i)) expectedValidRequests++;
    }
    AssertLE(expectedValidRequests, pp->numberOfRequests());

    recoveringFromExecutionOfRequests = true;
    mapOfRequestsThatAreBeingRecovered = b;
  }

  internalThreadPool.start(8);  // TODO(GG): use configuration
}

ReplicaImp::ReplicaImp(const ReplicaConfig &config,
                       IRequestsHandler *requestsHandler,
                       IStateTransfer *stateTrans,
                       shared_ptr<MsgsCommunicator> msgsCommunicator,
                       shared_ptr<PersistentStorage> persistentStorage,
                       shared_ptr<MsgHandlersRegistrator> msgHandlers,
                       concordUtil::Timers &timers)
    : ReplicaImp(
          true, config, requestsHandler, stateTrans, nullptr, nullptr, nullptr, msgsCommunicator, msgHandlers, timers) {
  if (persistentStorage != nullptr) {
    ps_ = persistentStorage;

    Assert(!ps_->hasReplicaConfig());

    ps_->beginWriteTran();
    ps_->setReplicaConfig(config);
    ps_->endWriteTran();
  }

  auto numThreads = 8;
  LOG_INFO(GL, "Starting internal replica thread pool. " << KVLOG(numThreads));
  internalThreadPool.start(numThreads);  // TODO(GG): use configuration
}

ReplicaImp::ReplicaImp(bool firstTime,
                       const ReplicaConfig &config,
                       IRequestsHandler *requestsHandler,
                       IStateTransfer *stateTrans,
                       SigManager *sigMgr,
                       ReplicasInfo *replicasInfo,
                       ViewsManager *viewsMgr,
                       shared_ptr<MsgsCommunicator> msgsCommunicator,
                       shared_ptr<MsgHandlersRegistrator> msgHandlers,
                       concordUtil::Timers &timers)
    : ReplicaForStateTransfer(config, stateTrans, msgsCommunicator, msgHandlers, firstTime, timers),
      viewChangeProtocolEnabled{config.viewChangeProtocolEnabled},
      autoPrimaryRotationEnabled{config.autoPrimaryRotationEnabled},
      restarted_{!firstTime},
      replyBuffer{(char *)std::malloc(config_.maxReplyMessageSize - sizeof(ClientReplyMsgHeader))},
      userRequestsHandler{requestsHandler},
      timeOfLastStateSynch{getMonotonicTime()},    // TODO(GG): TBD
      timeOfLastViewEntrance{getMonotonicTime()},  // TODO(GG): TBD
      timeOfLastAgreedView{getMonotonicTime()},    // TODO(GG): TBD
      metric_view_{metrics_.RegisterGauge("view", curView)},
      metric_last_stable_seq_num_{metrics_.RegisterGauge("lastStableSeqNum", lastStableSeqNum)},
      metric_last_executed_seq_num_{metrics_.RegisterGauge("lastExecutedSeqNum", lastExecutedSeqNum)},
      metric_last_agreed_view_{metrics_.RegisterGauge("lastAgreedView", lastAgreedView)},
      metric_current_active_view_{metrics_.RegisterGauge("currentActiveView", 0)},
      metric_viewchange_timer_{metrics_.RegisterGauge("viewChangeTimer", 0)},
      metric_retransmissions_timer_{metrics_.RegisterGauge("retransmissionTimer", 0)},
      metric_status_report_timer_{metrics_.RegisterGauge("statusReportTimer", 0)},
      metric_slow_path_timer_{metrics_.RegisterGauge("slowPathTimer", 0)},
      metric_info_request_timer_{metrics_.RegisterGauge("infoRequestTimer", 0)},
      metric_current_primary_{metrics_.RegisterGauge("currentPrimary", curView % config_.numReplicas)},
      metric_first_commit_path_{metrics_.RegisterStatus(
          "firstCommitPath", CommitPathToStr(ControllerWithSimpleHistory_debugInitialFirstPath))},
      metric_slow_path_count_{metrics_.RegisterCounter("slowPathCount", 0)},
      metric_received_internal_msgs_{metrics_.RegisterCounter("receivedInternalMsgs")},
      metric_received_client_requests_{metrics_.RegisterCounter("receivedClientRequestMsgs")},
      metric_received_pre_prepares_{metrics_.RegisterCounter("receivedPrePrepareMsgs")},
      metric_received_start_slow_commits_{metrics_.RegisterCounter("receivedStartSlowCommitMsgs")},
      metric_received_partial_commit_proofs_{metrics_.RegisterCounter("receivedPartialCommitProofMsgs")},
      metric_received_full_commit_proofs_{metrics_.RegisterCounter("receivedFullCommitProofMsgs")},
      metric_received_prepare_partials_{metrics_.RegisterCounter("receivedPreparePartialMsgs")},
      metric_received_commit_partials_{metrics_.RegisterCounter("receivedCommitPartialMsgs")},
      metric_received_prepare_fulls_{metrics_.RegisterCounter("receivedPrepareFullMsgs")},
      metric_received_commit_fulls_{metrics_.RegisterCounter("receivedCommitFullMsgs")},
      metric_received_checkpoints_{metrics_.RegisterCounter("receivedCheckpointMsgs")},
      metric_received_replica_statuses_{metrics_.RegisterCounter("receivedReplicaStatusMsgs")},
      metric_received_view_changes_{metrics_.RegisterCounter("receivedViewChangeMsgs")},
      metric_received_new_views_{metrics_.RegisterCounter("receivedNewViewMsgs")},
      metric_received_req_missing_datas_{metrics_.RegisterCounter("receivedReqMissingDataMsgs")},
      metric_received_simple_acks_{metrics_.RegisterCounter("receivedSimpleAckMsgs")},
      metric_sent_status_msgs_not_due_timer_{metrics_.RegisterCounter("sentStatusMsgsNotDueTime")},
      metric_sent_req_for_missing_data_{metrics_.RegisterCounter("sentReqForMissingData")},
      metric_sent_checkpoint_msg_due_to_status_{metrics_.RegisterCounter("sentCheckpointMsgDueToStatus")},
      metric_sent_viewchange_msg_due_to_status_{metrics_.RegisterCounter("sentViewChangeMsgDueToTimer")},
      metric_sent_newview_msg_due_to_status_{metrics_.RegisterCounter("sentNewviewMsgDueToCounter")},
      metric_sent_preprepare_msg_due_to_status_{metrics_.RegisterCounter("sentPreprepareMsgDueToStatus")},
      metric_sent_preprepare_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentPreprepareMsgDueToReqMissingData")},
      metric_sent_startSlowPath_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentStartSlowPathMsgDueToReqMissingData")},
      metric_sent_partialCommitProof_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentPartialCommitProofMsgDueToReqMissingData")},
      metric_sent_preparePartial_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentPrepreparePartialMsgDueToReqMissingData")},
      metric_sent_prepareFull_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentPrepareFullMsgDueToReqMissingData")},
      metric_sent_commitPartial_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentCommitPartialMsgDueToRewMissingData")},
      metric_sent_commitFull_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentCommitFullMsgDueToReqMissingData")},
      metric_sent_fullCommitProof_msg_due_to_reqMissingData_{
          metrics_.RegisterCounter("sentFullCommitProofMsgDueToReqMissingData")},
      metric_not_enough_client_requests_event_{metrics_.RegisterCounter("notEnoughClientRequestsEvent")},
      metric_total_finished_consensuses_{metrics_.RegisterCounter("totalOrderedRequests")},
      metric_total_slowPath_{metrics_.RegisterCounter("totalSlowPaths")},
      metric_total_fastPath_{metrics_.RegisterCounter("totalFastPaths")} {
  AssertLT(config_.replicaId, config_.numReplicas);
  // TODO(GG): more asserts on params !!!!!!!!!!!

  // !firstTime ==> ((sigMgr != nullptr) && (replicasInfo != nullptr) && (viewsMgr != nullptr))
  Assert(firstTime || ((sigMgr != nullptr) && (replicasInfo != nullptr) && (viewsMgr != nullptr)));

  registerMsgHandlers();
  // Register metrics component with the default aggregator.
  metrics_.Register();

  if (firstTime) {
    sigManager = new SigManager(config_.replicaId,
                                config_.numReplicas + config_.numOfClientProxies,
                                config_.replicaPrivateKey,
                                config_.publicKeysOfReplicas);
    repsInfo = new ReplicasInfo(config_, dynamicCollectorForPartialProofs, dynamicCollectorForExecutionProofs);
    viewsManager = new ViewsManager(repsInfo, sigManager, config_.thresholdVerifierForSlowPathCommit);
  } else {
    sigManager = sigMgr;
    repsInfo = replicasInfo;
    viewsManager = viewsMgr;

    // TODO(GG): consider to add relevant asserts
  }

  std::set<NodeIdType> clientsSet;
  for (uint16_t i = config_.numReplicas; i < config_.numReplicas + config_.numOfClientProxies; i++)
    clientsSet.insert(i);

  clientsManager =
      new ClientsManager(config_.replicaId, clientsSet, ReplicaConfigSingleton::GetInstance().GetSizeOfReservedPage());

  clientsManager->init(stateTransfer.get());

  if (!firstTime || config_.debugPersistentStorageEnabled) clientsManager->loadInfoFromReservedPages();

  // autoPrimaryRotationEnabled implies viewChangeProtocolEnabled
  // Note: "p=>q" is equivalent to "not p or q"
  AssertOR(!autoPrimaryRotationEnabled, viewChangeProtocolEnabled);

  viewChangeTimerMilli = (viewChangeTimeoutMilli > 0) ? viewChangeTimeoutMilli : config.viewChangeTimerMillisec;
  AssertGT(viewChangeTimerMilli, 0);

  if (autoPrimaryRotationEnabled) {
    autoPrimaryRotationTimerMilli =
        (autoPrimaryRotationTimerMilli > 0) ? autoPrimaryRotationTimerMilli : config.autoPrimaryRotationTimerMillisec;
    AssertGT(autoPrimaryRotationTimerMilli, 0);
  }

  // TODO(GG): use config ...
  dynamicUpperLimitOfRounds = new DynamicUpperLimitWithSimpleFilter<int64_t>(400, 2, 2500, 70, 32, 1000, 2, 2);

  mainLog =
      new SequenceWithActiveWindow<kWorkWindowSize, 1, SeqNum, SeqNumInfo, SeqNumInfo>(1, (InternalReplicaApi *)this);

  checkpointsLog = new SequenceWithActiveWindow<kWorkWindowSize + checkpointWindowSize,
                                                checkpointWindowSize,
                                                SeqNum,
                                                CheckpointInfo,
                                                CheckpointInfo>(0, (InternalReplicaApi *)this);

  // create controller . TODO(GG): do we want to pass the controller as a parameter ?
  controller =
      new ControllerWithSimpleHistory(config_.cVal, config_.fVal, config_.replicaId, curView, primaryLastUsedSeqNum);

  if (retransmissionsLogicEnabled)
    retransmissionsManager =
        new RetransmissionsManager(this, &internalThreadPool, &getIncomingMsgsStorage(), kWorkWindowSize, 0);
  else
    retransmissionsManager = nullptr;

  LOG_INFO(GL,
           "ReplicaConfig parameters isReadOnly="
               << config_.isReadOnly << ", numReplicas=" << config_.numReplicas
               << ", numRoReplicas=" << config_.numRoReplicas << ", fVal=" << config_.fVal << ", cVal=" << config_.cVal
               << ", replicaId=" << config_.replicaId << ", numOfClientProxies=" << config_.numOfClientProxies
               << ", statusReportTimerMillisec=" << config_.statusReportTimerMillisec << ", concurrencyLevel="
               << config_.concurrencyLevel << ", viewChangeProtocolEnabled=" << config_.viewChangeProtocolEnabled
               << ", viewChangeTimerMillisec=" << config_.viewChangeTimerMillisec
               << ", autoPrimaryRotationEnabled=" << config_.autoPrimaryRotationEnabled
               << ", autoPrimaryRotationTimerMillisec=" << config_.autoPrimaryRotationTimerMillisec
               << ", preExecReqStatusCheckTimerMillisec=" << config_.preExecReqStatusCheckTimerMillisec
               << ", maxExternalMessageSize=" << config_.maxExternalMessageSize << ", maxReplyMessageSize="
               << config_.maxReplyMessageSize << ", maxNumOfReservedPages=" << config_.maxNumOfReservedPages
               << ", sizeOfReservedPage=" << config_.sizeOfReservedPage
               << ", debugStatisticsEnabled=" << config_.debugStatisticsEnabled
               << ", metricsDumpIntervalSeconds=" << config_.metricsDumpIntervalSeconds);
}

ReplicaImp::~ReplicaImp() {
  // TODO(GG): rewrite this method !!!!!!!! (notice that the order may be important here ).
  // TODO(GG): don't delete objects that are passed as params (TBD)

  internalThreadPool.stop();

  delete viewsManager;
  delete controller;
  delete dynamicUpperLimitOfRounds;
  delete mainLog;
  delete checkpointsLog;
  delete clientsManager;
  delete sigManager;
  delete repsInfo;
  free(replyBuffer);

  if (config_.debugStatisticsEnabled) {
    DebugStatistics::freeDebugStatisticsData();
  }
}

void ReplicaImp::stop() {
  if (retransmissionsLogicEnabled) timers_.cancel(retranTimer_);
  timers_.cancel(slowPathTimer_);
  timers_.cancel(infoReqTimer_);
  timers_.cancel(statusReportTimer_);
  if (viewChangeProtocolEnabled) timers_.cancel(viewChangeTimer_);

  ReplicaForStateTransfer::stop();
}

void ReplicaImp::addTimers() {
  int statusReportTimerMilli = (sendStatusPeriodMilli > 0) ? sendStatusPeriodMilli : config_.statusReportTimerMillisec;
  AssertGT(statusReportTimerMilli, 0);
  metric_status_report_timer_.Get().Set(statusReportTimerMilli);
  statusReportTimer_ = timers_.add(milliseconds(statusReportTimerMilli),
                                   Timers::Timer::RECURRING,
                                   [this](Timers::Handle h) { onStatusReportTimer(h); });
  if (viewChangeProtocolEnabled) {
    int t = viewChangeTimerMilli;
    if (autoPrimaryRotationEnabled && t > autoPrimaryRotationTimerMilli) t = autoPrimaryRotationTimerMilli;
    metric_viewchange_timer_.Get().Set(t / 2);
    // TODO(GG): What should be the time period here?
    // TODO(GG): Consider to split to 2 different timers
    viewChangeTimer_ =
        timers_.add(milliseconds(t / 2), Timers::Timer::RECURRING, [this](Timers::Handle h) { onViewsChangeTimer(h); });
  }
  if (retransmissionsLogicEnabled) {
    metric_retransmissions_timer_.Get().Set(retransmissionsTimerMilli);
    retranTimer_ = timers_.add(milliseconds(retransmissionsTimerMilli),
                               Timers::Timer::RECURRING,
                               [this](Timers::Handle h) { onRetransmissionsTimer(h); });
  }
  const int slowPathsTimerPeriod = controller->timeToStartSlowPathMilli();
  metric_slow_path_timer_.Get().Set(slowPathsTimerPeriod);
  slowPathTimer_ = timers_.add(
      milliseconds(slowPathsTimerPeriod), Timers::Timer::RECURRING, [this](Timers::Handle h) { onSlowPathTimer(h); });

  metric_info_request_timer_.Get().Set(dynamicUpperLimitOfRounds->upperLimit() / 2);
  infoReqTimer_ = timers_.add(milliseconds(dynamicUpperLimitOfRounds->upperLimit() / 2),
                              Timers::Timer::RECURRING,
                              [this](Timers::Handle h) { onInfoRequestTimer(h); });
}

void ReplicaImp::start() {
  ReplicaForStateTransfer::start();
  addTimers();
  processMessages();
}

void ReplicaImp::processMessages() {
  LOG_INFO(GL, "Running ReplicaImp");

  if (recoveringFromExecutionOfRequests) {
    SeqNumInfo &seqNumInfo = mainLog->get(lastExecutedSeqNum + 1);
    PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();
    AssertNE(pp, nullptr);
    auto span = concordUtils::startSpan("bft_process_messages_on_start");
    executeRequestsInPrePrepareMsg(span, pp, true);
    metric_last_executed_seq_num_.Get().Set(lastExecutedSeqNum);
    metric_total_finished_consensuses_.Get().Inc();
    if (seqNumInfo.slowPathStarted()) {
      metric_total_slowPath_.Get().Inc();
    } else {
      metric_total_fastPath_.Get().Inc();
    }
    recoveringFromExecutionOfRequests = false;
    mapOfRequestsThatAreBeingRecovered = Bitmap();
  }
}

void ReplicaImp::executeReadOnlyRequest(concordUtils::SpanWrapper &parent_span, ClientRequestMsg *request) {
  Assert(request->isReadOnly());
  Assert(!isCollectingState());

  auto span = concordUtils::startChildSpan("bft_execute_read_only_request", parent_span);
  ClientReplyMsg reply(currentPrimary(), request->requestSeqNum(), config_.replicaId);

  uint16_t clientId = request->clientProxyId();

  int error = 0;
  uint32_t actualReplyLength = 0;

  if (!supportDirectProofs) {
    error = userRequestsHandler->execute(clientId,
                                         lastExecutedSeqNum,
                                         READ_ONLY_FLAG,
                                         request->requestLength(),
                                         request->requestBuf(),
                                         reply.maxReplyLength(),
                                         reply.replyBuf(),
                                         actualReplyLength,
                                         span);

    LOG_DEBUG(
        GL,
        "Executed read only request. " << KVLOG(
            clientId, lastExecutedSeqNum, request->requestLength(), reply.maxReplyLength(), actualReplyLength, error));
  } else {
    // TODO(GG): use code from previous drafts
    Assert(false);
  }

  // TODO(GG): TBD - how do we want to support empty replies? (actualReplyLength==0)

  if (!error) {
    if (actualReplyLength > 0) {
      reply.setReplyLength(actualReplyLength);
      send(&reply, clientId);
    } else {
      LOG_ERROR(GL, "Received zero size response. " << KVLOG(clientId));
    }

  } else {
    LOG_ERROR(GL, "Received error while executing RO request. " << KVLOG(clientId, error));
  }

  if (config_.debugStatisticsEnabled) {
    DebugStatistics::onRequestCompleted(true);
  }
}

void ReplicaImp::executeRequestsInPrePrepareMsg(concordUtils::SpanWrapper &parent_span,
                                                PrePrepareMsg *ppMsg,
                                                bool recoverFromErrorInRequestsExecution) {
  auto span = concordUtils::startChildSpan("bft_execute_requests_in_preprepare", parent_span);
  AssertAND(!isCollectingState(), currentViewIsActive());
  AssertNE(ppMsg, nullptr);
  AssertEQ(ppMsg->viewNumber(), curView);
  AssertEQ(ppMsg->seqNumber(), lastExecutedSeqNum + 1);

  const uint16_t numOfRequests = ppMsg->numberOfRequests();

  // recoverFromErrorInRequestsExecution ==> (numOfRequests > 0)
  AssertOR(!recoverFromErrorInRequestsExecution, (numOfRequests > 0));

  if (numOfRequests > 0) {
    Bitmap requestSet(numOfRequests);
    size_t reqIdx = 0;
    RequestsIterator reqIter(ppMsg);
    char *requestBody = nullptr;

    //////////////////////////////////////////////////////////////////////
    // Phase 1:
    // a. Find the requests that should be executed
    // b. Send reply for each request that has already been executed
    //////////////////////////////////////////////////////////////////////
    if (!recoverFromErrorInRequestsExecution) {
      while (reqIter.getAndGoToNext(requestBody)) {
        ClientRequestMsg req((ClientRequestMsgHeader *)requestBody);
        SCOPED_MDC_CID(req.getCid());
        NodeIdType clientId = req.clientProxyId();

        const bool validClient = isValidClient(clientId);
        if (!validClient) {
          LOG_WARN(GL, "The client is not valid. " << KVLOG(clientId));
          continue;
        }

        if (seqNumberOfLastReplyToClient(clientId) >= req.requestSeqNum()) {
          ClientReplyMsg *replyMsg = clientsManager->allocateMsgWithLatestReply(clientId, currentPrimary());
          send(replyMsg, clientId);
          delete replyMsg;
          continue;
        }

        requestSet.set(reqIdx);
        reqIdx++;
      }
      reqIter.restart();

      if (ps_) {
        DescriptorOfLastExecution execDesc{lastExecutedSeqNum + 1, requestSet};
        ps_->beginWriteTran();
        ps_->setDescriptorOfLastExecution(execDesc);
        ps_->endWriteTran();
      }
    } else {
      requestSet = mapOfRequestsThatAreBeingRecovered;
    }

    //////////////////////////////////////////////////////////////////////
    // Phase 2: execute requests + send replies
    // In this phase the application state may be changed. We also change data in the state transfer module.
    // TODO(GG): Explain what happens in recovery mode (what are the requirements from  the application, and from the
    // state transfer module.
    //////////////////////////////////////////////////////////////////////

    reqIdx = 0;
    requestBody = nullptr;

    auto dur = controller->durationSincePrePrepare(lastExecutedSeqNum + 1);
    if (dur > 0) {
      // Primary
      LOG_DEBUG(CNSUS, "Consensus reached, duration [" << dur << "ms]");

    } else {
      LOG_DEBUG(CNSUS, "Consensus reached");
    }

    while (reqIter.getAndGoToNext(requestBody)) {
      size_t tmp = reqIdx;
      reqIdx++;
      if (!requestSet.get(tmp)) {
        continue;
      }

      ClientRequestMsg req((ClientRequestMsgHeader *)requestBody);
      SCOPED_MDC_CID(req.getCid());
      NodeIdType clientId = req.clientProxyId();

      uint32_t actualReplyLength = 0;
      userRequestsHandler->execute(
          clientId,
          lastExecutedSeqNum + 1,
          req.flags(),
          req.requestLength(),
          req.requestBuf(),
          ReplicaConfigSingleton::GetInstance().GetMaxReplyMessageSize() - sizeof(ClientReplyMsgHeader),
          replyBuffer,
          actualReplyLength,
          span);

      AssertGT(actualReplyLength,
               0);  // TODO(GG): TBD - how do we want to support empty replies? (actualReplyLength==0)

      ClientReplyMsg *replyMsg = clientsManager->allocateNewReplyMsgAndWriteToStorage(
          clientId, req.requestSeqNum(), currentPrimary(), replyBuffer, actualReplyLength);
      send(replyMsg, clientId);
      delete replyMsg;
      clientsManager->removePendingRequestOfClient(clientId);
    }
  }

  if ((lastExecutedSeqNum + 1) % checkpointWindowSize == 0) {
    const uint64_t checkpointNum = (lastExecutedSeqNum + 1) / checkpointWindowSize;
    stateTransfer->createCheckpointOfCurrentState(checkpointNum);
  }

  //////////////////////////////////////////////////////////////////////
  // Phase 3: finalize the execution of lastExecutedSeqNum+1
  // TODO(GG): Explain what happens in recovery mode
  //////////////////////////////////////////////////////////////////////

  LOG_DEBUG(GL, "Finalized execution. " << KVLOG(lastExecutedSeqNum + 1, curView, lastStableSeqNum));

  if (ps_) {
    ps_->beginWriteTran();
    ps_->setLastExecutedSeqNum(lastExecutedSeqNum + 1);
  }

  lastExecutedSeqNum = lastExecutedSeqNum + 1;

  if (config_.debugStatisticsEnabled) {
    DebugStatistics::onLastExecutedSequenceNumberChanged(lastExecutedSeqNum);
  }
  if (lastViewThatTransferredSeqNumbersFullyExecuted < curView &&
      (lastExecutedSeqNum >= maxSeqNumTransferredFromPrevViews)) {
    lastViewThatTransferredSeqNumbersFullyExecuted = curView;
    if (ps_) {
      ps_->setLastViewThatTransferredSeqNumbersFullyExecuted(lastViewThatTransferredSeqNumbersFullyExecuted);
    }
  }

  if (lastExecutedSeqNum % checkpointWindowSize == 0) {
    Digest checkDigest;
    const uint64_t checkpointNum = lastExecutedSeqNum / checkpointWindowSize;
    stateTransfer->getDigestOfCheckpoint(checkpointNum, sizeof(Digest), (char *)&checkDigest);
    CheckpointMsg *checkMsg = new CheckpointMsg(config_.replicaId, lastExecutedSeqNum, checkDigest, false);
    CheckpointInfo &checkInfo = checkpointsLog->get(lastExecutedSeqNum);
    checkInfo.addCheckpointMsg(checkMsg, config_.replicaId);

    if (ps_) ps_->setCheckpointMsgInCheckWindow(lastExecutedSeqNum, checkMsg);

    if (checkInfo.isCheckpointCertificateComplete()) {
      onSeqNumIsStable(lastExecutedSeqNum);
    }
    checkInfo.setSelfExecutionTime(getMonotonicTime());
  }

  if (ps_) ps_->endWriteTran();

  if (numOfRequests > 0) userRequestsHandler->onFinishExecutingReadWriteRequests();

  sendCheckpointIfNeeded();

  bool firstCommitPathChanged = controller->onNewSeqNumberExecution(lastExecutedSeqNum);

  if (firstCommitPathChanged) {
    metric_first_commit_path_.Get().Set(CommitPathToStr(controller->getCurrentFirstPath()));
  }
  // TODO(GG): clean the following logic
  if (mainLog->insideActiveWindow(lastExecutedSeqNum)) {  // update dynamicUpperLimitOfRounds
    const SeqNumInfo &seqNumInfo = mainLog->get(lastExecutedSeqNum);
    const Time firstInfo = seqNumInfo.getTimeOfFisrtRelevantInfoFromPrimary();
    const Time currTime = getMonotonicTime();
    if ((firstInfo < currTime)) {
      const int64_t durationMilli = duration_cast<milliseconds>(currTime - firstInfo).count();
      dynamicUpperLimitOfRounds->add(durationMilli);
    }
  }

  if (config_.debugStatisticsEnabled) {
    DebugStatistics::onRequestCompleted(false);
  }
}

void ReplicaImp::executeNextCommittedRequests(concordUtils::SpanWrapper &parent_span, const bool requestMissingInfo) {
  AssertAND(!isCollectingState(), currentViewIsActive());
  AssertGE(lastExecutedSeqNum, lastStableSeqNum);
  auto span = concordUtils::startChildSpan("bft_execute_next_committed_requests", parent_span);

  while (lastExecutedSeqNum < lastStableSeqNum + kWorkWindowSize) {
    SeqNumInfo &seqNumInfo = mainLog->get(lastExecutedSeqNum + 1);

    PrePrepareMsg *prePrepareMsg = seqNumInfo.getPrePrepareMsg();

    const bool ready = (prePrepareMsg != nullptr) && (seqNumInfo.isCommitted__gg());

    if (requestMissingInfo && !ready) {
      LOG_INFO(GL, "Asking for missing information. " << KVLOG(lastExecutedSeqNum + 1, curView, lastStableSeqNum));

      tryToSendReqMissingDataMsg(lastExecutedSeqNum + 1);
    }

    if (!ready) break;

    AssertEQ(prePrepareMsg->seqNumber(), lastExecutedSeqNum + 1);
    AssertEQ(prePrepareMsg->viewNumber(), curView);  // TODO(GG): TBD

    executeRequestsInPrePrepareMsg(span, prePrepareMsg);
    metric_last_executed_seq_num_.Get().Set(lastExecutedSeqNum);
    metric_total_finished_consensuses_.Get().Inc();
    if (seqNumInfo.slowPathStarted()) {
      metric_total_slowPath_.Get().Inc();
    } else {
      metric_total_fastPath_.Get().Inc();
    }
  }

  if (isCurrentPrimary() && requestsQueueOfPrimary.size() > 0) tryToSendPrePrepareMsg(true);
}

IncomingMsgsStorage &ReplicaImp::getIncomingMsgsStorage() { return *msgsCommunicator_->getIncomingMsgsStorage(); }

// TODO(GG): the timer for state transfer !!!!

// TODO(GG): !!!! view changes and retransmissionsLogic --- check ....

}  // namespace bftEngine::impl
