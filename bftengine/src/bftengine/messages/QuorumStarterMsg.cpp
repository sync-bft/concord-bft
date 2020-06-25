// QuorumStarter
//
// This implements the message that initiates a round of quorum. 
// Only sent by the primary and used in the context of a 3 round protocol.
// TODO(QF): implements a waiting timeout mechanism

#include "QuorumStarterMsg.hpp"


namespace bftEngine {
namespace impl {

QuorumStarterMsg::QuorumStarterMsg(SeqNum s, ViewNum v, ReplicaId senderId) // TODO(QF): do we need spanContext and msgSize as param
        : MessageBase(senderId,
                      MsgCode::QuorumStarter,
                      sizeof(Header)) { // do we need to send any content in the msg?
    b()->viewNum = v;
    b()->seqNum = s;
    // voteCollection = new QuorumVoteCollection(senderId);
}

bool QuorumStarterMsg::addVoteMsg(QuorumVoteMsg *voteMsg){
    return voteCollection.addVoteMsg(voteMsg);
}

bool QuorumStarterMsg::isReady(const ReplicasInfo *repsInfo) const{
    return voteCollection.isReady(repsInfo);
}

bool QuorumStarterMsg::isCollected() const{
    return voteCollection.isCollected();
}

void QuorumStarterMsg::setCollected(bool status){
    voteCollection.setCollected(status);
}

void QuorumStarterMsg::freeCollection(){
    voteCollection.free();
}

}  // namespace impl
}  // namespace bftEngine