//
// Created by George Wang on 7/11/20.
//

#include "VoteMsg.hpp"
#include "assertUtils.hpp"

namespace bftEngine {
namespace impl {
VoteMsg::VoteMsg(ReplicaId sender, int16_t type, const std::string& spanContext, size_t msgSize)
    : MessageBase(sender, type, spanContext.size(), msgSize) {}

VoteMsg* VoteMsg::create(ViewNum v,
                         SeqNum s,
                         ReplicaId senderId,
                         Digest& ppDigest,
                         IThresholdSigner* thresholdSigner,
                         const std::string& spanContext) {

  const size_t sigLen = thresholdSigner->requiredLengthForSignedData();
  size_t size = sizeof(Header) + sigLen;

  VoteMsg* m = new VoteMsg(senderId, type, spanContext, size);

  m->b()->seqNumber = s;
  m->b()->viewNumber = v;
  m->b()->thresSigLength = (uint16_t)sigLen;

  Digest tmpDigest;
  Digest::calcCombination(digest, v, s, tmpDigest);

  auto position = m->body() + sizeof(Header);
  std::memcpy(position, spanContext.data(), spanContext.size());
  position += spanContext.size();

  thresholdSigner->signData((const char*)(&(tmpDigest)), sizeof(Digest), position, sigLen);

  return m;
 // return (VoteMsg*)SignedShareBase::create(
      //MsgCode::PreparePartial, v, s, senderId, ppDigest, thresholdSigner, spanContext);
}

void VoteMsg::_validate(const ReplicasInfo& repInfo, int16_t type_) const {
  Assert(type() == type_);
  if (size() < sizeof(Header) + spanContextSize() ||
      size() < sizeof(Header) + signatureLen() + spanContextSize() ||  // size
      senderId() == repInfo.myId() ||                                  // sent from another replica
      !repInfo.isIdOfReplica(senderId()))
    throw std::runtime_error(__PRETTY_FUNCTION__);
}

  if (repInfo.myId() != repInfo.primaryOfView(viewNumber()))
    throw std::runtime_error(__PRETTY_FUNCTION__ + std::string(": the primary is the collector of VoteMsg"));
}

}//impl namespace
}// bftEngine namespace
