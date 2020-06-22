// QuorumStarter
//
// This implements the message that initiates a round of quorum. 
// Only sent by the primary and used in the context of a 3 round protocol.
// TODO(QF): implements a waiting timeout mechanism

#include <bftengine/ClientMsg.hpp>
#include "QuorumStarterMsg.hpp"
#include "QuorumVoteMsg.hpp"
#include <queue>


namespace bftEngine {
namespace impl {


QuorumVoteCollection::QuorumVoteCollection(ReplicaId owner, int16_t size){
    
}

QuorumVoteCollection::QuorumVoteCollection(ReplicaId owner, int16_t size){

}

bool QuorumVoteCollection::addVoteMsg(QuorumVoteMsg *voteMsg){

}

bool QuorumVoteCollection::isReady(){

}

static int16_t QuorumVoteCollection::calcMajorityNum(){

}

QuorumStarterMsg::QuorumStarterMsg(SeqNum s, ViewNum v, ReplicaId senderId, int16_t replicaNum) // TODO(QF): do we need spanContext and msgSize as param
        : MessageBase(senderId,
                      MsgCode:QuorumStarter, //TODO(QF): needs to implement quorum starter in messagebase  
                      sizeof(Header)) // do we need to send any content in the msg?
    b()->viewNum = v;
    b()->seqNum = s;
    voteCollection = new QuorumVoteCollection(senderId, replicaNum);
}

bool QuorumStarterMsg::addVoteMsg(QuorumVoteMsg *voteMsg){
    return voteCollection->addVoteMsg(voteMsg);
}

bool QuorumStarterMsg::isReady(){
    return voteCollection->isReady();
}

}  // namespace impl
}  // namespace bftEngine