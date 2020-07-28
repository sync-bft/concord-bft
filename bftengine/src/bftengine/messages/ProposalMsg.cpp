#include <bftengine/ClientMsgs.hpp>
#include "ProposalMsg.hpp"
#include "SysConsts.hpp"
#include "Crypto.hpp"
#include "ClientRequestMsg.hpp"
#include "ReplicaConfig.hpp"

namespace bftEngine {
namespace impl {

static Digest nullDigest(0x18);

void ProposalMsg::validate(const ReplicasInfo& repInfo) const{
    Assert(senderId() != repInfo.myId());

    if (size() < sizeof(Header) + spanContextSize() || 
        !repInfo.isIdOfReplica(senderId()))
        throw std::runtime_error(__PRETTY_FUNCTION__ + std::string(": basic"));

    // check flags
    const uint16_t flags = b()->flags;
    const bool isNull = ((flags & 0x1) == 0);
    const bool isReady = (((flags >> 1) & 0x1) == 1);

     if (b()->seqNum == 0 || isNull ||  // we don't send null requests
      !isReady ||                    // not ready
      b()->endLocationOfLastRequest > size() || b()->numberOfRequests == 0 ||
      b()->numberOfRequests >= b()->endLocationOfLastRequest)
    throw std::runtime_error(__PRETTY_FUNCTION__ + std::string(": advanced"));

    // check digest of client request buffer
    Digest d;
    const char* buffer = (char*)&(b()->seqNumDigestFill);
    const uint32_t bufferSize = (b()->endLocationOfLastRequest - proposalHeaderPrefix);

    DigestUtil::compute(buffer, bufferSize, (char*)&d, sizeof(Digest));

    if (d != b()->digestOfRequestsSeqNum) throw std::runtime_error(__PRETTY_FUNCTION__ + std::string(": digest"));

    // TODO(QF): check certificates

}

ProposalMsg::ProposalMsg(ReplicaId sender, ViewNum v, SeqNum s, const char* combinedSigBody, size_t combinedSigLength, size_t size)
    : ProposalMsg(sender, v, s, combinedSigBody, combinedSigLength, "", size){}

ProposalMsg::ProposalMsg(ReplicaId sender, ViewNum v, SeqNum s, const char* combinedSigBody, size_t combinedSigLength, const std::string& spanContext, size_t size)
    : MessageBase(sender,
                  MsgCode::Proposal,
                  spanContext.size(),
                  (((size + sizeof(Header)) < maxMessageSize<ProposalMsg>())
                       ? (size + sizeof(Header))
                       : maxMessageSize<ProposalMsg>() - spanContext.size()))
{
    bool ready = size == 0;
    if (!ready) {
        b()->digestOfRequestsSeqNum.makeZero();
    } else {
        b()->digestOfRequestsSeqNum = nullDigest;
    }

    b()->endLocationOfLastRequest = requestsPayloadShift();
    b()->flags = computeFlagsForProposalMsg(ready, ready);
    b()->numberOfRequests = 0;
    b()->seqNum = s;
    b()->viewNum = v;
    b()->seqNumDigestFill = s;
    b()->combinedSigLen = combinedSigLength;

    char* position = body() + sizeof(Header);
    memcpy(position, spanContext.data(), b()->header.spanContextSize);
    position = body() + sizeof(Header) +  b()->header.spanContextSize;
    memcpy(position, combinedSigBody, b()->combinedSigLen);
}

int32_t ProposalMsg::remainingSizeForRequests() const {
  Assert(!isReady());
  Assert(!isNull());
  Assert(b()->endLocationOfLastRequest >= requestSPayloadShift());

  return (internalStorageSize() - b()->endLocationOfLastRequest);
}

void ProposalMsg::addRequest(const char* pRequest, uint32_t requestSize) {
  Assert(getRequestSizeTemp(pRequest) == requestSize);
  Assert(!isNull());
  Assert(!isReady());
  Assert(remainingSizeForRequests() >= requestSize);

  char* insertPtr = body() + b()->endLocationOfLastRequest;

  memcpy(insertPtr, pRequest, requestSize);

  b()->endLocationOfLastRequest += requestSize;
  b()->numberOfRequests++;
}

void ProposalMsg::finishAddingRequests() {
  Assert(!isNull());
  Assert(!isReady());
  Assert(b()->numberOfRequests > 0);
  Assert(b()->endLocationOfLastRequest > requestsPayloadShift());
  Assert(b()->digestOfRequests.isZero());

  // check requests (for debug - consider to remove)
  // Assert(checkRequests());

  // mark as ready
  b()->flags |= 0x2;
  Assert(isReady());

  // compute and set digest
  Digest d;
  const char* buffer = (char*)&(b()->seqNumDigestFill);
  const uint32_t bufferSize = (b()->endLocationOfLastRequest - proposalHeaderPrefix);
  DigestUtil::compute(buffer, bufferSize, (char*)&d, sizeof(Digest));
  b()->digestOfRequestsSeqNum = d;

  // size
  setMsgSize(b()->endLocationOfLastRequest);
  shrinkToFit();
}

int16_t ProposalMsg::computeFlagsForProposalMsg(bool isNull, bool isReady) {
  int16_t retVal = 0;

  Assert(!isNull || isReady);  // isNull --> isReady

  retVal |= ((isReady ? 1 : 0) << 1);
  retVal |= (isNull ? 0 : 1);

  return retVal;
}

const std::string ProposalMsg::getClientCorrelationIdForMsg(int index) const {
  auto it = ContentsIterator(this);
  int req_num = 0;
  while (!it.end() && req_num < index) {
    it.gotoNext();
    req_num++;
  }
  if (it.end()) return std::string();
  char* requestBody = nullptr;
  it.getCurrent(requestBody);
  return ClientRequestMsg((ClientRequestMsgHeader*)requestBody).getCid();
}

const std::string ProposalMsg::getBatchCorrelationIdAsString() const {
  std::string ret;
  auto it = RequestsIterator(this);
  char* requestBody = nullptr;
  while (it.getAndGoToNext(requestBody)) {
    ClientRequestMsg req((ClientRequestMsgHeader*)requestBody);
    ret += req.getCid() + ";";
  }
  return ret;
}

uint32_t ProposalMsg::requestPayloadShift() const { return sizeof(Header) + b()->header.spanContextSize + b()->combinedSigLen; }


///////////////////////////////////////////////////////////////////////////////
// ContentsIterator
///////////////////////////////////////////////////////////////////////////////

ContentIterator::ContentIterator(const ProposalMsg* const m) : msg{m}, currLoc{?m->requestsPayloadShift()} {
  Assert(msg->isReady());
}

void ContentIterator::restart() { currLoc = msg->requestsPayloadShift();}

bool ContentIterator::getCurrent(char*& pContent) const {
  if (end()) return false;

  char* p = msg->body() + currLoc;
  pContent = p;

  return true;
}

bool ContentIterator::end() const {
  Assert(currLoc <= msg->b()->endLocationOfLastRequest;

  return (currLoc = msg->b()->endLocationOfLastRequest;
}

void ContentIterator::gotoNext() {
  Assert(!end());
  char* p = msg->body() + currLoc;
  uint32_t size = getRequestSizeTemp(p); // TODO(QF): to be implemented - signature size
  currLoc += size;
  Assert(currLoc <= msg->b()->endLocationOfLastRequest;
}

bool ContentIterator::getAndGoToNext(char*& pContent) {
  bool atEnd = !getCurrent(pContent);

  if (atEnd) return false;

  gotoNext();

  return true;
}


}}