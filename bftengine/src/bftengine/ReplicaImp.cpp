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
#include "MsgHandlersRegistrator.hpp"
#include "ReplicaLoader.hpp"
#include "PersistentStorage.hpp"
#include "OpenTracing.hpp"
#include "diagnostics.h"

#include "messages/ClientRequestMsg.hpp"
#include "messages/PrePrepareMsg.hpp"
#include "messages/CheckpointMsg.hpp"
#include "messages/ClientReplyMsg.hpp"
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

  msgHandlers_->registerInternalMsgHandler([this](InternalMessage &&msg) { onInternalMsg(std::move(msg)); });
}

template <typename T>
void ReplicaImp::messageHandler(MessageBase *msg) {
  if (validateMessage(msg) && !isCollectingState())
    onMessage<T>(static_cast<T *>(msg));//validate message, if valid, go through the process
  else
    delete msg;//delete message if invalid
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
  send(m, id);// increment metric
  counterMetric.Get().Inc();// get metric
}

void ReplicaImp::onReportAboutInvalidMessage(MessageBase *msg, const char *reason) {
  LOG_WARN(GL, "Received invalid message. " << KVLOG(msg->senderId(), msg->type(), reason));

  // TODO(GG): logic that deals with invalid messages (e.g., a node that sends invalid messages may have a problem (old
  // version,bug,malicious,...)).
}

template <>
void ReplicaImp::onMessage<ClientRequestMsg>(ClientRequestMsg *m) {
  // Analyzes the client message, and consider whether or not to operate/memorize it
  metric_received_client_requests_.Get().Inc(); // receive client request
  const NodeIdType senderId = m->senderId(); //store the sender id as constant
  const NodeIdType clientId = m->clientProxyId(); // store the proxy id as constant
  const bool readOnly = m->isReadOnly(); // evaluate whether the request is read-only
  const ReqId reqSeqNum = m->requestSeqNum(); //store the request sequence number
  const uint8_t flags = m->flags(); //NOT SURE

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_CID(m->getCid()); //// append temporary message
  LOG_DEBUG(GL, "Received ClientRequestMsg. " << KVLOG(clientId, reqSeqNum, flags, senderId)); //debug

  const auto &span_context = m->spanContext<std::remove_pointer<decltype(m)>::type>(); //not sure, something related to open tracing
  auto span = concordUtils::startChildSpanFromContext(span_context, "bft_client_request"); //not sure, something related to open tracing
  span.setTag("rid", config_.replicaId); //not sure, something related to open tracing
  span.setTag("cid", m->getCid()); //not sure, something related to open tracing
  span.setTag("seq_num", reqSeqNum); //not sure, something related to open tracing

  if (isCollectingState()) {
    LOG_INFO(GL,
             "ClientRequestMsg is ignored because this replica is collecting missing state from the other replicas. "
                 << KVLOG(reqSeqNum, clientId)); //replica is collecting missing state
    delete m; // ignore this message
    return;
  }

  // check message validity
  const bool invalidClient = !isValidClient(clientId);
  const bool sentFromReplicaToNonPrimary = repsInfo->isIdOfReplica(senderId) && !isCurrentPrimary();

  // TODO(GG): more conditions (make sure that a request of client A cannot be generated by client B!=A)
  if (invalidClient || sentFromReplicaToNonPrimary) { //still evaluate validity
    std::ostringstream oss("ClientRequestMsg is invalid. "); //output invalid
    oss << KVLOG(invalidClient, sentFromReplicaToNonPrimary); //message sending from replica to non-primary is invalid
    onReportAboutInvalidMessage(m, oss.str().c_str()); //report such message
    delete m; //delete message
    return;
  }

  if (readOnly) {
    executeReadOnlyRequest(span, m); //execute the read only request
    delete m; //delete message
    return;
  }

  if (!currentViewIsActive()) {//ignore message if current view is inactive, not in steady state
    LOG_INFO(GL, "ClientRequestMsg is ignored because current view is inactive. " << KVLOG(reqSeqNum, clientId));
    delete m;//delete message
    return;
  }

  const ReqId seqNumberOfLastReply = seqNumberOfLastReplyToClient(clientId); //store the requence number of last reply

  if (seqNumberOfLastReply < reqSeqNum) {//if request is up-to-date
    if (isCurrentPrimary()) {
      // TODO(GG): use config/parameter
      if (requestsQueueOfPrimary.size() >= 700) {//Too many messages, request queue is full
        LOG_WARN(GL,
                 "ClientRequestMsg dropped. Primary reqeust queue is full. "
                     << KVLOG(clientId, reqSeqNum, requestsQueueOfPrimary.size()));
        delete m;//delete message
        return;
      }
      if (clientsManager->noPendingAndRequestCanBecomePending(clientId, reqSeqNum)) {
        LOG_DEBUG(CNSUS,
                  "Pushing to primary queue, request [" << reqSeqNum << "], client [" << clientId
                                                        << "], senderId=" << senderId);
        requestsQueueOfPrimary.push(m);//push request into queue
        primaryCombinedReqSize += m->size(); //update request size
        tryToSendPrePrepareMsg(true); //try to enter T1
        return;
      } else {
        LOG_INFO(GL,
                 "ClientRequestMsg is ignored because: request is old, OR primary is current working on a request from "
                 "the same client. "
                     << KVLOG(clientId, reqSeqNum)); //ignore old message
      }
    } else {  // not the current primary
      if (clientsManager->noPendingAndRequestCanBecomePending(clientId, reqSeqNum)) {
        clientsManager->addPendingRequest(clientId, reqSeqNum);// if not current primary,, get the client id and reqseqnum

        // TODO(GG): add a mechanism that retransmits (otherwise we may start unnecessary view-change)
        send(m, currentPrimary());//send the message to current primary

        LOG_INFO(GL, "Forwarding ClientRequestMsg to the current primary. " << KVLOG(reqSeqNum, clientId));//print update
      } else {
        LOG_INFO(GL,
                 "ClientRequestMsg is ignored because: request is old, OR primary is current working on a request "
                 "from the same client. "
                     << KVLOG(clientId, reqSeqNum));//ignore request if it is old
      }
    }
  } else if (seqNumberOfLastReply == reqSeqNum) {
    LOG_DEBUG(/debug
        GL,
        "ClientRequestMsg has already been executed: retransmitting reply to client. " << KVLOG(reqSeqNum, clientId));

    ClientReplyMsg *repMsg = clientsManager->allocateMsgWithLatestReply(clientId, currentPrimary());// inform the clinet it has been executed
    send(repMsg, clientId);//send reply message to client
    delete repMsg;//delete reply message
  } else {
    LOG_INFO(GL,
             "ClientRequestMsg is ignored because request sequence number is not monotonically increasing. "
                 << KVLOG(clientId, reqSeqNum, seqNumberOfLastReply));//ignored because of invalid sequence number
  }

  delete m;// delete message
}

void ReplicaImp::tryToSendPrePrepareMsg(bool batchingLogic) {
  // This is the T1 phase
  Assert(isCurrentPrimary());// assert the sender is current primary
  Assert(currentViewIsActive());//asset the current view is active

  if (primaryLastUsedSeqNum + 1 > lastStableSeqNum + kWorkWindowSize) {
    //exceed window size, should be expect workwindowsize to be 1?
    LOG_INFO(GL,
             "Will not send PrePrepare since next sequence number ["
                 << primaryLastUsedSeqNum + 1 << "] exceeds window threshold [" << lastStableSeqNum + kWorkWindowSize
                 << "]");//print out error message
    return;
  }

  if (primaryLastUsedSeqNum + 1 > lastExecutedSeqNum + config_.concurrencyLevel) {
    //exceeds concurrency threshhold
    LOG_INFO(GL,
             "Will not send PrePrepare since next sequence number ["
                 << primaryLastUsedSeqNum + 1 << "] exceeds concurrency threshold ["
                 << lastExecutedSeqNum + config_.concurrencyLevel << "]");//print out error message
    return;  // TODO(GG): should also be checked by the non-primary replicas
  }

  if (requestsQueueOfPrimary.empty()) return;// if no message, then return

  // remove irrelevant requests from the head of the requestsQueueOfPrimary (and update requestsInQueue)
  ClientRequestMsg *first = requestsQueueOfPrimary.front(); //declare a pointer to the front most of queue
  while (first != nullptr && //while the pointer is not null pointer
         !clientsManager->noPendingAndRequestCanBecomePending(first->clientProxyId(), first->requestSeqNum())) {
    SCOPED_MDC_CID(first->getCid());
    primaryCombinedReqSize -= first->size(); // update primaryCombinedReqSize
    delete first; //delete the message
    requestsQueueOfPrimary.pop(); //pop another message
    first = (!requestsQueueOfPrimary.empty() ? requestsQueueOfPrimary.front() : nullptr); //update first
  }

  const size_t requestsInQueue = requestsQueueOfPrimary.size(); //store size of requests in queue

  if (requestsInQueue == 0) return; //if empty, return

  AssertGE(primaryLastUsedSeqNum, lastExecutedSeqNum); //evaluate primaryLastUsedSeqNum, lastExecutedSeqNum whether valid or not

  uint64_t concurrentDiff = ((primaryLastUsedSeqNum + 1) - lastExecutedSeqNum);// set concurrency window?
  uint64_t minBatchSize = 1; //set mininum batch size to 1

  // update maxNumberOfPendingRequestsInRecentHistory (if needed)
  if (requestsInQueue > maxNumberOfPendingRequestsInRecentHistory)
    maxNumberOfPendingRequestsInRecentHistory = requestsInQueue;//update maxNumberOfPendingRequestsInRecentHistory

  // TODO(GG): the batching logic should be part of the configuration - TBD.
  if (batchingLogic && (concurrentDiff >= 2)) {
    minBatchSize = concurrentDiff * batchingFactor; //update minBatchSize

    const size_t maxReasonableMinBatchSize = 350;  // TODO(GG): use param from configuration
    //set maxReasonableMinBatchSize
    if (minBatchSize > maxReasonableMinBatchSize) minBatchSize = maxReasonableMinBatchSize; //update minBatchSize
  }

  if (requestsInQueue < minBatchSize) {
    LOG_INFO(GL,
             "Not enough client requests to fill the batch threshold size. " << KVLOG(minBatchSize, requestsInQueue));
    metric_not_enough_client_requests_event_.Get().Inc();// not enough requests in queue, induce this event
    return;
  }

  // update batchingFactor Do not quite understand what bacthing factor is
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

  CommitPath firstPath = controller->getCurrentFirstPath(); //set commit path?

  AssertOR((config_.cVal != 0), (firstPath != CommitPath::FAST_WITH_THRESHOLD));

  controller->onSendingPrePrepare((primaryLastUsedSeqNum + 1), firstPath);

  ClientRequestMsg *nextRequest = (!requestsQueueOfPrimary.empty() ? requestsQueueOfPrimary.front() : nullptr); //update next request
  const auto &span_context = nextRequest ? nextRequest->spanContext<ClientRequestMsg>() : std::string{};// not sure what span context is

  PrePrepareMsg *pp = new PrePrepareMsg(
      config_.replicaId, curView, (primaryLastUsedSeqNum + 1), firstPath, span_context, primaryCombinedReqSize);
  uint16_t initialStorageForRequests = pp->remainingSizeForRequests(); // set configuration of the preprepare message
  while (nextRequest != nullptr) {//while there is still next request
    if (nextRequest->size() <= pp->remainingSizeForRequests()) {// enough space for next request
      SCOPED_MDC_CID(nextRequest->getCid());
      if (clientsManager->noPendingAndRequestCanBecomePending(nextRequest->clientProxyId(),
                                                              nextRequest->requestSeqNum())) {
        pp->addRequest(nextRequest->body(), nextRequest->size());// add request
        clientsManager->addPendingRequest(nextRequest->clientProxyId(), nextRequest->requestSeqNum());// add request
      }
      primaryCombinedReqSize -= nextRequest->size();// update primaryCombinedReqSize using size of next request
    } else if (nextRequest->size() > initialStorageForRequests) {
      // The message is too big, drop it
      LOG_ERROR(GL,
                "Request was dropped because it exceeds maximum allowed size. "
                    << KVLOG(nextRequest->senderId(), nextRequest->size(), initialStorageForRequests));
    }
    delete nextRequest;// delete request
    requestsQueueOfPrimary.pop();//pop another request
    nextRequest = (!requestsQueueOfPrimary.empty() ? requestsQueueOfPrimary.front() : nullptr);//set popped request to next request if satify condition
  }

  if (pp->numberOfRequests() == 0) {// if no client requests at this time, return
    LOG_WARN(GL, "No client requests added to PrePrepare batch. Nothing to send.");
    return;
  }

  pp->finishAddingRequests();// finish adding request

  if (config_.debugStatisticsEnabled) {/debug
    DebugStatistics::onSendPrePrepareMessage(pp->numberOfRequests(), requestsQueueOfPrimary.size());
  }
  primaryLastUsedSeqNum++;// update primaryLastUsedSeqNum
  SCOPED_MDC_SEQ_NUM(std::to_string(primaryLastUsedSeqNum));// append temporary key-value pairs to the log messages
  SCOPED_MDC_PATH(CommitPathToMDCString(firstPath));// append temporary key-value pairs to the log messages
  {
    LOG_INFO(CNSUS,
             "Sending PrePrepare with the following payload of the following correlation ids ["
                 << pp->getBatchCorrelationIdAsString() << "]");
  }
  SeqNumInfo &seqNumInfo = mainLog->get(primaryLastUsedSeqNum);// get primaryLastUsedSeqNum
  seqNumInfo.addSelfMsg(pp);

  //not sure what this is
  if (ps_) {
    ps_->beginWriteTran();
    ps_->setPrimaryLastUsedSeqNum(primaryLastUsedSeqNum);
    ps_->setPrePrepareMsgInSeqNumWindow(primaryLastUsedSeqNum, pp);
    if (firstPath == CommitPath::SLOW) ps_->setSlowStartedInSeqNumWindow(primaryLastUsedSeqNum, true);
    ps_->endWriteTran();
  }

  for (ReplicaId x : repsInfo->idsOfPeerReplicas()) {
    sendRetransmittableMsgToReplica(pp, x, primaryLastUsedSeqNum);// send message
  }

  if (firstPath == CommitPath::SLOW) {
    seqNumInfo.startSlowPath(); // if slow past, then start slow path
    metric_slow_path_count_.Get().Inc();// get info
    sendPreparePartial(seqNumInfo);// send prepare partial, which is entering T2 phase
  } else {
    sendPartialProof(seqNumInfo);// use fast path instead
  }
}

template <typename T>
bool ReplicaImp::relevantMsgForActiveView(const T *msg) {
  const SeqNum msgSeqNum = msg->seqNumber(); //obtain sequence number
  const ViewNum msgViewNum = msg->viewNumber(); //obtain view number

  const bool isCurrentViewActive = currentViewIsActive();// evaluate whether current view is active
  if (isCurrentViewActive && (msgViewNum == curView) && (msgSeqNum > strictLowerBoundOfSeqNums) &&
      (mainLog->insideActiveWindow(msgSeqNum))) {// current view active, message relevant and valid
    AssertGT(msgSeqNum, lastStableSeqNum);//evaluate message validity
    AssertLE(msgSeqNum, lastStableSeqNum + kWorkWindowSize);// evaluate message validity

    return true;
  } else if (!isCurrentViewActive) {// current view is not active, ignore message
    LOG_INFO(GL,
             "My current view is not active, ignoring msg."
                 << KVLOG(curView, isCurrentViewActive, msg->senderId(), msgSeqNum, msgViewNum));// print error
    return false;
  } else {
    const SeqNum activeWindowStart = mainLog->currentActiveWindow().first; //obtain sequence number of activeWindowStart
    const SeqNum activeWindowEnd = mainLog->currentActiveWindow().second;// obtain sequence number of activeWindowEnd
    const bool myReplicaMayBeBehind = (curView < msgViewNum) || (msgSeqNum > activeWindowEnd);// is the replica behind?
    if (myReplicaMayBeBehind) {
      onReportAboutAdvancedReplica(msg->senderId(), msgSeqNum, msgViewNum);// if replica is behind, report to other replica
      LOG_INFO(GL,
               "Msg is not relevant for my current view. The sending replica may be in advance."
                   << KVLOG(curView,
                            isCurrentViewActive,
                            msg->senderId(),
                            msgSeqNum,
                            msgViewNum,
                            activeWindowStart,
                            activeWindowEnd));// print error
    } else {
      const bool msgReplicaMayBeBehind = (curView > msgViewNum) || (msgSeqNum + kWorkWindowSize < activeWindowStart);//is message is behind?

      if (msgReplicaMayBeBehind) {
        onReportAboutLateReplica(msg->senderId(), msgSeqNum, msgViewNum);// report message is behind to late replicas
        LOG_INFO(// print error message
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
  metric_received_pre_prepares_.Get().Inc();// replicas receive preprepare message from leader
  const SeqNum msgSeqNum = msg->seqNumber();// obtain message sequence number

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary key-value pairs to the log messages
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum)); // append temporary key-value pairs to the log messages
  LOG_DEBUG(GL, KVLOG(msg->senderId(), msg->size())); //debug
  // do not know what this is doing though
  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "handle_bft_preprepare");// initialize span
  span.setTag("rid", config_.replicaId);// set span tag
  span.setTag("seq_num", msgSeqNum);// set span tag.

  if (!currentViewIsActive() && viewsManager->waitingForMsgs() && msgSeqNum > lastStableSeqNum) {
    Assert(!msg->isNull());  // we should never send (and never accept) null PrePrepare message

    if (viewsManager->addPotentiallyMissingPP(msg, lastStableSeqNum)) {
      LOG_INFO(GL, "PrePrepare added to views manager. " << KVLOG(lastStableSeqNum));
      tryToEnterView();// enter current view
    } else {
      LOG_INFO(GL, "PrePrepare discarded.");// ignore preprepare message
    }

    return;  // TODO(GG): memory deallocation is confusing .....
  }

  bool msgAdded = false;// set message added to false

  if (relevantMsgForActiveView(msg) && (msg->senderId() == currentPrimary())) {
    sendAckIfNeeded(msg, msg->senderId(), msgSeqNum);// send acknowledgement if needed
    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);// obtain sequence number info
    const bool slowStarted = (msg->firstPath() == CommitPath::SLOW || seqNumInfo.slowPathStarted());// evaluate if using slow path

    // For MDC it doesn't matter which type of fast path
    SCOPED_MDC_PATH(CommitPathToMDCString(slowStarted ? CommitPath::SLOW : CommitPath::OPTIMISTIC_FAST));
    if (seqNumInfo.addMsg(msg)) {// about fast path? not fully understood
      {// what are correlation ids?
        LOG_INFO(CNSUS,
                 "PrePrepare with the following correlation IDs [" << msg->getBatchCorrelationIdAsString() << "]");
      }
      msgAdded = true;//update msgAdded

      if (ps_) {// still don't understand what ps_ is
        ps_->beginWriteTran();
        ps_->setPrePrepareMsgInSeqNumWindow(msgSeqNum, msg);
        if (slowStarted) ps_->setSlowStartedInSeqNumWindow(msgSeqNum, true);
        ps_->endWriteTran();
      }

      if (!slowStarted)  // TODO(GG): make sure we correctly handle a situation where StartSlowCommitMsg is handled
                         // before PrePrepareMsg
      {// if not slow started, like entering t4 before t2 or sth.
        sendPartialProof(seqNumInfo);// send the partial proof
      } else {
        seqNumInfo.startSlowPath();// start slow path
        metric_slow_path_count_.Get().Inc();//initialize slow path
        sendPreparePartial(seqNumInfo);//enter t2
        ;
      }
    }
  }

  if (!msgAdded) delete msg;//delete message
}

void ReplicaImp::tryToStartSlowPaths() {
  if (!isCurrentPrimary() || isCollectingState() || !currentViewIsActive())// invalid, stop
    return;  // TODO(GG): consider to stop the related timer when this method is not needed (to avoid useless
             // invocations)

  const SeqNum minSeqNum = lastExecutedSeqNum + 1;// set minimum sequence number
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message to log

  if (minSeqNum > lastStableSeqNum + kWorkWindowSize) {// start slow path if this is the case
    LOG_INFO(GL,
             "Try to start slow path: minSeqNum > lastStableSeqNum + kWorkWindowSize."
                 << KVLOG(minSeqNum, lastStableSeqNum, kWorkWindowSize));
    return;
  }

  const SeqNum maxSeqNum = primaryLastUsedSeqNum;// set maximum sequence number

  AssertLE(maxSeqNum, lastStableSeqNum + kWorkWindowSize);//assert less than
  AssertLE(minSeqNum, maxSeqNum + 1);//assert less than

  if (minSeqNum > maxSeqNum) return;

  sendCheckpointIfNeeded();  // TODO(GG): TBD - do we want it here ?

  const Time currTime = getMonotonicTime();//set current time

  for (SeqNum i = minSeqNum; i <= maxSeqNum; i++) {
    SeqNumInfo &seqNumInfo = mainLog->get(i);// incrementally get sequence number info

    if (seqNumInfo.partialProofs().hasFullProof() ||                          // already has a full proof
        seqNumInfo.slowPathStarted() ||                                       // slow path has already  started
        seqNumInfo.partialProofs().getSelfPartialCommitProof() == nullptr ||  // did not start a fast path
        (!seqNumInfo.hasPrePrepareMsg()))
      continue;  // slow path is not needed

    const Time timeOfPartProof = seqNumInfo.partialProofs().getTimeOfSelfPartialProof();// what is time of part proof?

    if (currTime - timeOfPartProof < milliseconds(controller->timeToStartSlowPathMilli())) break;
    SCOPED_MDC_SEQ_NUM(std::to_string(i));
    SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));
    LOG_INFO(CNSUS,
             "Primary initiates slow path for seqNum="
                 << i << " (currTime=" << duration_cast<microseconds>(currTime.time_since_epoch()).count()
                 << " timeOfPartProof=" << duration_cast<microseconds>(timeOfPartProof.time_since_epoch()).count()
                 << " threshold for degradation [" << controller->timeToStartSlowPathMilli() << "ms]");

    controller->onStartingSlowCommit(i);

    seqNumInfo.startSlowPath();// initiate slow path
    metric_slow_path_count_.Get().Inc();

    if (ps_) {
      ps_->beginWriteTran();
      ps_->setSlowStartedInSeqNumWindow(i, true);
      ps_->endWriteTran();
    }

    // send StartSlowCommitMsg to all replicas

    StartSlowCommitMsg *startSlow = new StartSlowCommitMsg(config_.replicaId, curView, i);// compose start slow path message

    for (ReplicaId x : repsInfo->idsOfPeerReplicas()) {// send the message incrementally to all other replicas
      sendRetransmittableMsgToReplica(startSlow, x, i);
    }

    delete startSlow;// delete the message

    sendPreparePartial(seqNumInfo);// send prepare partial, enter T2
  }
}

void ReplicaImp::tryToAskForMissingInfo() {
  if (!currentViewIsActive() || isCollectingState()) return;// if already updating or not active, return

  AssertLE(maxSeqNumTransferredFromPrevViews, lastStableSeqNum + kWorkWindowSize);// assert less than

  const bool recentViewChange = (maxSeqNumTransferredFromPrevViews > lastStableSeqNum);//if there is a recent view change

  SeqNum minSeqNum = 0;// set minimum sequence number
  SeqNum maxSeqNum = 0;// set maximum sequence number

  if (!recentViewChange) {// if no recent view change
    const int16_t searchWindow = 4;  // TODO(GG): TBD - read from configuration
    minSeqNum = lastExecutedSeqNum + 1;// update minimum sequence number
    maxSeqNum = std::min(minSeqNum + searchWindow - 1, lastStableSeqNum + kWorkWindowSize);// update maximum sequence number
  } else {// if there was a recent view change
    const int16_t searchWindow = 32;  // TODO(GG): TBD - read from configuration
    minSeqNum = lastStableSeqNum + 1;// update minimum sequence number
    while (minSeqNum <= lastStableSeqNum + kWorkWindowSize) {
      SeqNumInfo &seqNumInfo = mainLog->get(minSeqNum);// get sequence number info
      if (!seqNumInfo.isCommitted__gg()) break;
      minSeqNum++;// update minimum sequence number
    }
    maxSeqNum = std::min(minSeqNum + searchWindow - 1, lastStableSeqNum + kWorkWindowSize);// update maximum sequence number
  }

  if (minSeqNum > lastStableSeqNum + kWorkWindowSize) return;// updated, then return

  const Time curTime = getMonotonicTime();// what is this ?

  SeqNum lastRelatedSeqNum = 0; // set lastRelatedSeqNum

  // TODO(GG): improve/optimize the following loops

  for (SeqNum i = minSeqNum; i <= maxSeqNum; i++) {
    Assert(mainLog->insideActiveWindow(i));// assert inside active window is true

    const SeqNumInfo &seqNumInfo = mainLog->get(i);// get sequence number info

    Time t = seqNumInfo.getTimeOfFisrtRelevantInfoFromPrimary();// get Time Of Fisrt Relevant Info From Primary

    const Time lastInfoRequest = seqNumInfo.getTimeOfLastInfoRequest();// get last info request

    if ((t < lastInfoRequest)) t = lastInfoRequest;// update first relevant info

    if (t != MinTime && (t < curTime)) {// do not know what this is doing
      auto diffMilli = duration_cast<milliseconds>(curTime - t);
      if (diffMilli.count() >= dynamicUpperLimitOfRounds->upperLimit()) lastRelatedSeqNum = i;
    }
  }

  for (SeqNum i = minSeqNum; i <= lastRelatedSeqNum; i++) {
    if (!recentViewChange)
      tryToSendReqMissingDataMsg(i);// send request missing data message
    else
      tryToSendReqMissingDataMsg(i, true, currentPrimary());// send to current primary
  }
}

template <>
void ReplicaImp::onMessage<StartSlowCommitMsg>(StartSlowCommitMsg *msg) {
  metric_received_start_slow_commits_.Get().Inc();// receive data for starting slow commit
  const SeqNum msgSeqNum = msg->seqNumber();// get message sequence number
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));// append temporary data to log
  LOG_INFO(GL, " ");

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_start_slow_commit_msg");// what is span?
  if (relevantMsgForActiveView(msg)) {
    sendAckIfNeeded(msg, currentPrimary(), msgSeqNum);// send acknowledgement if needed

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);// get sequence number info

    if (!seqNumInfo.slowPathStarted() && !seqNumInfo.isPrepared()) {
      LOG_INFO(GL, "Start slow path.");// if not started, then start

      seqNumInfo.startSlowPath();/ start
      metric_slow_path_count_.Get().Inc();/ get metric

      if (ps_) {// what is ps_???
        ps_->beginWriteTran();
        ps_->setSlowStartedInSeqNumWindow(msgSeqNum, true);
        ps_->endWriteTran();
      }

      if (seqNumInfo.hasPrePrepareMsg() == false)
        tryToSendReqMissingDataMsg(msgSeqNum);//send request missing data, since already has message
      else
        sendPreparePartial(seqNumInfo);// enter t2
    }
  }

  delete msg;
}

void ReplicaImp::sendPartialProof(SeqNumInfo &seqNumInfo) {
  PartialProofsSet &partialProofs = seqNumInfo.partialProofs();// get partial proofs

  if (!seqNumInfo.hasPrePrepareMsg()) return;// if no attempt to send preprepare, then return

  PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();// get preprepare message
  Digest &ppDigest = pp->digestOfRequests();// get digest of request
  const SeqNum seqNum = pp->seqNumber();// get sequence number of preprepare

  if (!partialProofs.hasFullProof()) {
    // send PartialCommitProofMsg to all collectors

    PartialCommitProofMsg *part = partialProofs.getSelfPartialCommitProof();// pointer to partial proof

    if (part == nullptr) {
      IThresholdSigner *commitSigner = nullptr;// what is this?
      CommitPath commitPath = pp->firstPath();// commit to first path

      AssertOR((config_.cVal != 0), (commitPath != CommitPath::FAST_WITH_THRESHOLD));// assert or

      if ((commitPath == CommitPath::FAST_WITH_THRESHOLD) && (config_.cVal > 0))// if fast path
        commitSigner = config_.thresholdSignerForCommit;// what is this?
      else
        commitSigner = config_.thresholdSignerForOptimisticCommit;// do not understand

      Digest tmpDigest;// initiate temporaray digest
      Digest::calcCombination(ppDigest, curView, seqNum, tmpDigest);// digest

      const auto &span_context = pp->spanContext<std::remove_pointer<decltype(pp)>::type>();
      part = new PartialCommitProofMsg(// create new partial commit message with info
          config_.replicaId, curView, seqNum, commitPath, tmpDigest, commitSigner, span_context);
      partialProofs.addSelfMsgAndPPDigest(part, tmpDigest);// make partial proof
    }

    partialProofs.setTimeOfSelfPartialProof(getMonotonicTime());// set time?

    // send PartialCommitProofMsg (only if, from my point of view, at most MaxConcurrentFastPaths are in progress)
    if (seqNum <= lastExecutedSeqNum + MaxConcurrentFastPaths) {
      // TODO(GG): improve the following code (use iterators instead of a simple array)
      int8_t numOfRouters = 0;// number of routers
      ReplicaId routersArray[2];// initialize replica id

      repsInfo->getCollectorsForPartialProofs(curView, seqNum, &numOfRouters, routersArray);
      // don't quite understand what's going on here
      for (int i = 0; i < numOfRouters; i++) {
        ReplicaId router = routersArray[i];
        if (router != config_.replicaId) {
          sendRetransmittableMsgToReplica(part, router, seqNum);
        }
      }
    }
  }
}

void ReplicaImp::sendPreparePartial(SeqNumInfo &seqNumInfo) {
  Assert(currentViewIsActive());// assert current view is active

  if (seqNumInfo.getSelfPreparePartialMsg() == nullptr && seqNumInfo.hasPrePrepareMsg() && !seqNumInfo.isPrepared()) {
    PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();// initialize prepare message if conditions met. exits message, and not prepared

    AssertNE(pp, nullptr);// assert message is not null

    LOG_DEBUG(GL, "Sending PreparePartialMsg. " << KVLOG(pp->seqNumber()));// debug

    const auto &span_context = pp->spanContext<std::remove_pointer<decltype(pp)>::type>();//dereference?
    PreparePartialMsg *p = PreparePartialMsg::create(curView,
                                                     pp->seqNumber(),
                                                     config_.replicaId,
                                                     pp->digestOfRequests(),
                                                     config_.thresholdSignerForSlowPathCommit,
                                                     span_context);// create the preparepartial message
    seqNumInfo.addSelfMsg(p);// leader add the message to itself

    if (!isCurrentPrimary()) sendRetransmittableMsgToReplica(p, currentPrimary(), pp->seqNumber());//if not primary, then send message to current primary
  }
}

void ReplicaImp::sendCommitPartial(const SeqNum s) {//T4
  Assert(currentViewIsActive());// assert current view active
  Assert(mainLog->insideActiveWindow(s));// assert in active windows
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(s));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));// append temporary message

  SeqNumInfo &seqNumInfo = mainLog->get(s);// retrieve sequence number info
  PrePrepareMsg *pp = seqNumInfo.getPrePrepareMsg();//retrieve preprepare message

  Assert(seqNumInfo.isPrepared());// assert prepared
  AssertNE(pp, nullptr);// assert message is not null
  AssertEQ(pp->seqNumber(), s);// assert sequence number is equal to message's sequence number

  if (seqNumInfo.committedOrHasCommitPartialFromReplica(config_.replicaId)) return;  // not needed

  LOG_DEBUG(CNSUS, "Sending CommitPartialMsg");//debug

  Digest d;//declare digest
  Digest::digestOfDigest(pp->digestOfRequests(), d);// update digest

  auto prepareFullMsg = seqNumInfo.getValidPrepareFullMsg();// prepare full message

  CommitPartialMsg *c =//create commit partial message
      CommitPartialMsg::create(curView,
                               s,
                               config_.replicaId,
                               d,
                               config_.thresholdSignerForSlowPathCommit,
                               prepareFullMsg->spanContext<std::remove_pointer<decltype(prepareFullMsg)>::type>());
  seqNumInfo.addSelfCommitPartialMsgAndDigest(c, d);// add commit partial to primary

  if (!isCurrentPrimary()) sendRetransmittableMsgToReplica(c, currentPrimary(), s);// if this is not primary, send message to primary
}

template <>
void ReplicaImp::onMessage<PartialCommitProofMsg>(PartialCommitProofMsg *msg) {
  metric_received_partial_commit_proofs_.Get().Inc();//get metric for partial commit proofs
  const SeqNum msgSeqNum = msg->seqNumber();// retrieve sequence number
  const SeqNum msgView = msg->viewNumber();// retrieve message view
  const NodeIdType msgSender = msg->senderId();// retrieve sender id
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(msg->commitPath()));// append temporary message
  Assert(repsInfo->isIdOfPeerReplica(msgSender)); // assert id is peer replica
  Assert(repsInfo->isCollectorForPartialProofs(msgView, msgSeqNum));// assert id is collector/leader

  LOG_DEBUG(GL, KVLOG(msgSender, msg->size()));//debug

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_partial_commit_proof_msg");// what is span?
  if (relevantMsgForActiveView(msg)) {
    sendAckIfNeeded(msg, msgSender, msgSeqNum);// send acknowledgement if message relevant and in current view

    if (msgSeqNum > lastExecutedSeqNum) {// if message up to date
      SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);// retrieve sequence number info
      PartialProofsSet &pps = seqNumInfo.partialProofs();//create partial proof

      if (pps.addMsg(msg)) {// add partial proof
        return;
      }
    }
  }

  delete msg;// delete message
  return;
}

template <>
void ReplicaImp::onMessage<FullCommitProofMsg>(FullCommitProofMsg *msg) {
  metric_received_full_commit_proofs_.Get().Inc();//retrieve metric for full commit proofs

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_full_commit_proof_msg");//what is span?
  const SeqNum msgSeqNum = msg->seqNumber();// retrieve sequence number
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(msg->seqNumber()));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::OPTIMISTIC_FAST));// append temporary message

  LOG_DEBUG(CNSUS, "Reached consensus, Received FullCommitProofMsg message");//debug

  if (relevantMsgForActiveView(msg)) {
    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);// retrieve sequence number info
    PartialProofsSet &pps = seqNumInfo.partialProofs();//retrieve partial proof

    if (!pps.hasFullProof() && pps.addMsg(msg))  // TODO(GG): consider to verify the signature in another thread
    {
      Assert(seqNumInfo.hasPrePrepareMsg());// assert this sequence number info has prepare message

      seqNumInfo.forceComplete();  // TODO(GG): remove forceComplete() (we know that  seqNumInfo is committed because
                                   // of the  FullCommitProofMsg message)

      if (ps_) {//what is ps?
        ps_->beginWriteTran();
        ps_->setFullCommitProofMsgInSeqNumWindow(msgSeqNum, msg);
        ps_->setForceCompletedInSeqNumWindow(msgSeqNum, true);
        ps_->endWriteTran();
      }

      if (msg->senderId() == config_.replicaId) sendToAllOtherReplicas(msg);// send message to all other replicas

      const bool askForMissingInfoAboutCommittedItems =
          (msgSeqNum > lastExecutedSeqNum + config_.concurrencyLevel);  // TODO(GG): check/improve this logic

      auto execution_span = concordUtils::startChildSpan("bft_execute_committed_reqs", span);// what is span?
      executeNextCommittedRequests(execution_span, askForMissingInfoAboutCommittedItems);// execute next committed request
      return;
    } else {
      LOG_INFO(GL, "Failed to satisfy full proof requirements");//requirement not met
    }
  }

  delete msg;// delete message
  return;
}

void ReplicaImp::onInternalMsg(InternalMessage &&msg) {
  metric_received_internal_msgs_.Get().Inc();//retrieve metric for internal message

  // Handle a full commit proof sent by self
  if (auto *fcp = std::get_if<FullCommitProofMsg *>(&msg)) {
    return onInternalMsg(*fcp);
  }

  // Handle prepare related internal messages
  if (auto *csf = std::get_if<CombinedSigFailedInternalMsg>(&msg)) {//if failed
    return onPrepareCombinedSigFailed(csf->seqNumber, csf->view, csf->replicasWithBadSigs);
  }
  if (auto *css = std::get_if<CombinedSigSucceededInternalMsg>(&msg)) {// if succedded
    return onPrepareCombinedSigSucceeded(
        css->seqNumber, css->view, css->combinedSig.data(), css->combinedSig.size(), css->span_context_);
  }
  if (auto *vcs = std::get_if<VerifyCombinedSigResultInternalMsg>(&msg)) {// what is this?
    return onPrepareVerifyCombinedSigResult(vcs->seqNumber, vcs->view, vcs->isValid);
  }

  // Handle Commit related internal messages
  if (auto *ccss = std::get_if<CombinedCommitSigSucceededInternalMsg>(&msg)) {
    return onCommitCombinedSigSucceeded(// commit succeeded
        ccss->seqNumber, ccss->view, ccss->combinedSig.data(), ccss->combinedSig.size(), ccss->span_context_);
  }
  if (auto *ccsf = std::get_if<CombinedCommitSigFailedInternalMsg>(&msg)) {// commit failed
    return onCommitCombinedSigFailed(ccsf->seqNumber, ccsf->view, ccsf->replicasWithBadSigs);
  }
  if (auto *vccs = std::get_if<VerifyCombinedCommitSigResultInternalMsg>(&msg)) {//what is verified?
    return onCommitVerifyCombinedSigResult(vccs->seqNumber, vccs->view, vccs->isValid);
  }

  // Handle a response from a RetransmissionManagerJob
  if (auto *rpr = std::get_if<RetranProcResultInternalMsg>(&msg)) {
    onRetransmissionsProcessingResults(rpr->lastStableSeqNum, rpr->view, rpr->suggestedRetransmissions);
    return retransmissionsManager->OnProcessingComplete();
  }

  assert(false);
}

void ReplicaImp::onInternalMsg(FullCommitProofMsg *msg) {
  if (isCollectingState() || (!currentViewIsActive()) || (curView != msg->viewNumber()) ||
      (!mainLog->insideActiveWindow(msg->seqNumber()))) {
    delete msg;// delete message if not leader, collecting, message view number not match, not inside active window
    return;
  }
  onMessage(msg);// treat it as new received new full commit proof message
}

template <>
void ReplicaImp::onMessage<PreparePartialMsg>(PreparePartialMsg *msg) {
  metric_received_prepare_partials_.Get().Inc();// retrieve metric for prepare partial
  const SeqNum msgSeqNum = msg->seqNumber();// retrieve message sequence number
  const ReplicaId msgSender = msg->senderId();// retrieve message sender id

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));// append temporary message

  bool msgAdded = false;// set messaged added to false

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_prepare_partial_msg");// what is span?

  if (relevantMsgForActiveView(msg)) {
    Assert(isCurrentPrimary());// assert the replica is the current primary

    sendAckIfNeeded(msg, msgSender, msgSeqNum);// send acknowledgement is needed

    LOG_DEBUG(GL, "Received relevant PreparePartialMsg." << KVLOG(msgSender));//debug

    controller->onMessage(msg);// retrieve message

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);// get message sequence number

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();// get full commit proof (t5)

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();// get valid commit full message (t4)

    PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg();// get prepare full message

    if (fcp != nullptr) {
      send(fcp, msgSender);// send if full commit message is not null
    } else if (commitFull != nullptr) {
      send(commitFull, msgSender);// send commitfull if commitfull is not null
    } else if (preFull != nullptr) {
      send(preFull, msgSender);// send prepare full message
    } else {
      msgAdded = seqNumInfo.addMsg(msg);// add message
    }
  }

  if (!msgAdded) {
    LOG_DEBUG(GL,
              "Node " << config_.replicaId << " ignored the PreparePartialMsg from node " << msgSender << " (seqNumber "
                      << msgSeqNum << ")");// ignore message
    delete msg;//delete it
  }
}

template <>
void ReplicaImp::onMessage<CommitPartialMsg>(CommitPartialMsg *msg) {
  metric_received_commit_partials_.Get().Inc();// retrieve metric for commit partial
  const SeqNum msgSeqNum = msg->seqNumber();// retrieve message sequence number
  const ReplicaId msgSender = msg->senderId();// retrieve sender id

  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));// append temporary message

  bool msgAdded = false; //set message added to false since haven't added yet

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_commit_partial_msg");//?
  if (relevantMsgForActiveView(msg)) {
    Assert(isCurrentPrimary());//assert this replica is current primary

    sendAckIfNeeded(msg, msgSender, msgSeqNum);// send acknowledgement if needed

    LOG_DEBUG(GL, "Received CommitPartialMsg from node " << msgSender);//debug print out

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);// retrieve sequence number info

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();// get full commit proof message (T5)

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();// T4

    if (fcp != nullptr) {
      send(fcp, msgSender);// send full commit proof if fcp not null
    } else if (commitFull != nullptr) {
      send(commitFull, msgSender);// enter t4
    } else {
      msgAdded = seqNumInfo.addMsg(msg);//add message
    }
  }

  if (!msgAdded) {
    LOG_INFO(GL, "Ignored CommitPartialMsg. " << KVLOG(msgSender));
    delete msg;//delete message if it is ignored
  }
}

template <>
void ReplicaImp::onMessage<PrepareFullMsg>(PrepareFullMsg *msg) {
  metric_received_prepare_fulls_.Get().Inc();// get prepare full message metric
  const SeqNum msgSeqNum = msg->seqNumber();// get message sequence number
  const ReplicaId msgSender = msg->senderId();// get sender id
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));// append temporary message
  bool msgAdded = false;// set messaged added to false since not added yet

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_preprare_full_msg");//?
  if (relevantMsgForActiveView(msg)) {
    sendAckIfNeeded(msg, msgSender, msgSeqNum);//send acknowledge if needed since message relevant

    LOG_DEBUG(GL, "received PrepareFullMsg");//debug print

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);//get sequence number info

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();// get full commit proof if t5

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();// get valid commmit fulll message if t4

    PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg();// prepare full, I think it means t3

    if (fcp != nullptr) {
      send(fcp, msgSender);// send message if fcp is not null
    } else if (commitFull != nullptr) {
      send(commitFull, msgSender);// send commit message if t4
    } else if (preFull != nullptr) {
      // do nothing
    } else {
      msgAdded = seqNumInfo.addMsg(msg);//add message
    }
  }

  if (!msgAdded) {
    LOG_DEBUG(GL, "Ignored PrepareFullMsg." << KVLOG(msgSender));
    delete msg;//delete message if ignored
  }
}
template <>
void ReplicaImp::onMessage<CommitFullMsg>(CommitFullMsg *msg) {
  metric_received_commit_fulls_.Get().Inc();// get commit full metric
  const SeqNum msgSeqNum = msg->seqNumber();// retrieve sequence number of message
  const ReplicaId msgSender = msg->senderId();// retrieve sender id
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(msgSeqNum));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));// append temporary message
  bool msgAdded = false;//set message added to false

  auto span = concordUtils::startChildSpanFromContext(msg->spanContext<std::remove_pointer<decltype(msg)>::type>(),
                                                      "bft_handle_commit_full_msg");//what is this span?
  if (relevantMsgForActiveView(msg)) {
    sendAckIfNeeded(msg, msgSender, msgSeqNum);//send acknowledegment if relevant

    LOG_DEBUG(GL, "Received CommitFullMsg" << KVLOG(msgSender));//debug print out

    SeqNumInfo &seqNumInfo = mainLog->get(msgSeqNum);//get sequence number info

    FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();//reference to full proof

    CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();// reference to valid commit full

    if (fcp != nullptr) {
      send(fcp,
           msgSender);  // TODO(GG): do we really want to send this message ? (msgSender already has a CommitFullMsg
                        // for the same seq number)
    } else if (commitFull != nullptr) {
      // nop
    } else {
      msgAdded = seqNumInfo.addMsg(msg);//simply add message
    }
  }

  if (!msgAdded) {
    LOG_DEBUG(GL,
              "Node " << config_.replicaId << " ignored the CommitFullMsg from node " << msgSender << " (seqNumber "
                      << msgSeqNum << ")");//if ignore
    delete msg;//delete message
  }
}

void ReplicaImp::onPrepareCombinedSigFailed(SeqNum seqNumber,
                                            ViewNum view,
                                            const std::set<uint16_t> &replicasWithBadSigs) {
  LOG_DEBUG(GL, KVLOG(seqNumber, view));//debug print out

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_DEBUG(GL, "Dropping irrelevant signature." << KVLOG(seqNumber, view));//drop irrelevant signature

    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);//reference to sequence number

  seqNumInfo.onCompletionOfPrepareSignaturesProcessing(seqNumber, view, replicasWithBadSigs);
  //dealing bad replicas?
  // TODO(GG): add logic that handles bad replicas ...
}

void ReplicaImp::onPrepareCombinedSigSucceeded(//T3
    SeqNum seqNumber, ViewNum view, const char *combinedSig, uint16_t combinedSigLen, const std::string &span_context) {
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(seqNumber));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));// append temporary message
  LOG_DEBUG(GL, KVLOG(view));//debug printout

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_INFO(GL, "Not sending prepare full: Invalid state, view, or sequence number." << KVLOG(view, curView));
    return;//invalid request, quit
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);//reference sequence number

  seqNumInfo.onCompletionOfPrepareSignaturesProcessing(seqNumber, view, combinedSig, combinedSigLen, span_context);
//prepare signature?
  FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();//pointer to full proof

  PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg();//get pointer to valid prepare full message

  AssertNE(preFull, nullptr);//assert preFull is not null

  if (fcp != nullptr) return;  // don't send if we already have FullCommitProofMsg
  LOG_DEBUG(CNSUS, "Sending prepare full" << KVLOG(view));//debug print out
  if (ps_) {//what is ps?
    ps_->beginWriteTran();
    ps_->setPrepareFullMsgInSeqNumWindow(seqNumber, preFull);
    ps_->endWriteTran();
  }

  for (ReplicaId x : repsInfo->idsOfPeerReplicas()) sendRetransmittableMsgToReplica(preFull, x, seqNumber);
//incrementally send message to peer replicas
  Assert(seqNumInfo.isPrepared());//assert sequence number info prepared is true

  sendCommitPartial(seqNumber);//send commit partial message
}

void ReplicaImp::onPrepareVerifyCombinedSigResult(SeqNum seqNumber, ViewNum view, bool isValid) {
  LOG_DEBUG(GL, KVLOG(view));

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_INFO(GL,
             "Not sending commit partial: Invalid state, view, or sequence number."
                 << KVLOG(seqNumber, view, curView, mainLog->insideActiveWindow(seqNumber)));
    return;
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);

  seqNumInfo.onCompletionOfCombinedPrepareSigVerification(seqNumber, view, isValid);

  if (!isValid) return;  // TODO(GG): we should do something about the replica that sent this invalid message

  FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();

  if (fcp != nullptr) return;  // don't send if we already have FullCommitProofMsg

  Assert(seqNumInfo.isPrepared());// assert sequence number info is prepared

  if (ps_) {// what is ps?
    PrepareFullMsg *preFull = seqNumInfo.getValidPrepareFullMsg();
    AssertNE(preFull, nullptr);
    ps_->beginWriteTran();
    ps_->setPrepareFullMsgInSeqNumWindow(seqNumber, preFull);
    ps_->endWriteTran();
  }

  sendCommitPartial(seqNumber);//send commit partial T4
}

void ReplicaImp::onCommitCombinedSigFailed(SeqNum seqNumber,
                                           ViewNum view,
                                           const std::set<uint16_t> &replicasWithBadSigs) {
  LOG_DEBUG(GL, KVLOG(seqNumber, view));//debug printout

  if ((isCollectingState()) || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_DEBUG(GL, "Invalid state, view, or sequence number." << KVLOG(seqNumber, view, curView));
    return;// invalid state, view, or sequence number, quit.
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);//get a reference to sequence number info

  seqNumInfo.onCompletionOfCommitSignaturesProcessing(seqNumber, view, replicasWithBadSigs);//retrieve commmit with signature?

  // TODO(GG): add logic that handles bad replicas ...
}

void ReplicaImp::onCommitCombinedSigSucceeded(
    SeqNum seqNumber, ViewNum view, const char *combinedSig, uint16_t combinedSigLen, const std::string &span_context) {
  SCOPED_MDC_PRIMARY(std::to_string(currentPrimary()));// append temporary message
  SCOPED_MDC_SEQ_NUM(std::to_string(seqNumber));// append temporary message
  SCOPED_MDC_PATH(CommitPathToMDCString(CommitPath::SLOW));// append temporary message
  LOG_DEBUG(GL, KVLOG(view));//debug printout

  if (isCollectingState() || (!currentViewIsActive()) || (curView != view) ||
      (!mainLog->insideActiveWindow(seqNumber))) {
    LOG_INFO(GL,
             "Not sending full commit: Invalid state, view, or sequence number."
                 << KVLOG(view, curView, mainLog->insideActiveWindow(seqNumber)));//debug printout
    return;//invalid state, view, or sequence number, quit
  }

  SeqNumInfo &seqNumInfo = mainLog->get(seqNumber);//retrieve sequence number info

  seqNumInfo.onCompletionOfCommitSignaturesProcessing(seqNumber, view, combinedSig, combinedSigLen, span_context);//retrieve signature

  FullCommitProofMsg *fcp = seqNumInfo.partialProofs().getFullProof();//pointer to full commit proof? T5
  CommitFullMsg *commitFull = seqNumInfo.getValidCommitFullMsg();//pointer to valid commit full? T4

  AssertNE(commitFull, nullptr);//assert commitfull is not null
  if (fcp != nullptr) return;  // ignore if we already have FullCommitProofMsg
  LOG_DEBUG(CNSUS, "Sending full commit" << KVLOG(view));//debug output
  if (ps_) {
    ps_->beginWriteTran();//what is ps?
    ps_->setCommitFullMsgInSeqNumWindow(seqNumber, commitFull);
    ps_->endWriteTran();
  }

  for (ReplicaId x : repsInfo->idsOfPeerReplicas()) sendRetransmittableMsgToReplica(commitFull, x, seqNumber);//send retrainsmittable message for each peer replica

  Assert(seqNumInfo.isCommitted__gg());//assert sequence number info is committed

  bool askForMissingInfoAboutCommittedItems = (seqNumber > lastExecutedSeqNum + config_.concurrencyLevel);//evaluate is there are missing commited items

  auto span = concordUtils::startChildSpanFromContext(//what is span?
      commitFull->spanContext<std::remove_pointer<decltype(commitFull)>::type>(), "bft_execute_committed_reqs");
  executeNextCommittedRequests(span, askForMissingInfoAboutCommittedItems);//execute next committed requests (evaluate T4)
}

void ReplicaImp::onCommitVerifyCombinedSigResult(SeqNum seqNumber, ViewNum view, bool isValid) {
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
  executeNextCommittedRequests(span, askForMissingInfoAboutCommittedItems);
}

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

void ReplicaImp::onRetransmissionsProcessingResults(SeqNum relatedLastStableSeqNum,
                                                    const ViewNum relatedViewNumber,
                                                    const std::forward_list<RetSuggestion> &suggestedRetransmissions) {
  Assert(retransmissionsLogicEnabled);

  if (isCollectingState() || (relatedViewNumber != curView) || (!currentViewIsActive())) return;
  if (relatedLastStableSeqNum + kWorkWindowSize <= lastStableSeqNum) return;

  const uint16_t myId = config_.replicaId;
  const uint16_t primaryId = currentPrimary();

  for (const RetSuggestion &s : suggestedRetransmissions) {
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
        new RetransmissionsManager(&internalThreadPool, &getIncomingMsgsStorage(), kWorkWindowSize, 0);
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
