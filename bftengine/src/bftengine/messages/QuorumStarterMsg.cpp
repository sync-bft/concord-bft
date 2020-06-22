// QuorumStarter
//
// This implements the message that initiates a round of quorum. 
// Only sent by the primary and used in the context of a 3 round protocol.
// TODO(QF): implements a waiting timeout mechanism

#include <bftengine/ClientMsg.hpp>
#include "QuorumStarterMsg.hpp"
#include "QuorumVoteMsg.hpp"
#include <queue>
#include <iostream>


namespace bftEngine {
namespace impl {


QuorumVoteCollection::QuorumVoteCollection(ReplicaId owner, int16_t size){
    ownerId = owner;
    replicaSize = size;
    votes = new std::queue<QuorumVoteMsg *>;
}

bool QuorumVoteCollection::addVoteMsg(QuorumVoteMsg *voteMsg){
    bool status = isVoteValid(voteMsg);
    if (status) {
        votes.push(voteMsg);
        voteCnt++;
    }
    return status;
}

bool QuorumVoteCollection::isReady() const{
    return voteCnt>calcMajorityNum();
}

int16_t QuorumVoteCollection::calcMajorityNum() const{
    // TODO(QF)
    return 0;
}

bool QuorumVoteCollection::isVoteValid(QuorumVoteMsg *newVoteMsg) const{
    if (votes.empty()) return true;
    bool identicalSenderFlag = false;
    bool identicalMsgFlag = false;  // TODO(QF): same flags?
    for(auto it=votes.begin(); it!=votes.end();++it){
        if (newVoteMsg->equals(*it)){
            identicalMsgFlag = true;
            std::cout<<"Primary "<<ownerId<<" received a repetitive quorum vote msg from sender replica "<<newVoteMsg->senderId()<<endl;  //TODO(QF): is (NodeIdType) senderId printable?
            break;
        }
        if (newVoteMsg->senderId()->equals(*it->senderId())){  // TODO(QF): or ==?
            identicalSenderFlag = true;
            std::cout<<"Primary "<<ownerId<<" received a quorum vote msg from the same sender replica but different content "<<newVoteMsg->senderId()<<endl;  //TODO(QF): is (NodeIdType) senderId printable?
            break;
        }
    }
    return !(identialMsgFlag||identicalSenderFlag);
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

bool QuorumStarterMsg::isReady() const{
    return voteCollection->isReady();
}

}  // namespace impl
}  // namespace bftEngine