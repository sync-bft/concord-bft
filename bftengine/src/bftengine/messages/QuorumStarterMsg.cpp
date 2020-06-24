// QuorumStarter
//
// This implements the message that initiates a round of quorum. 
// Only sent by the primary and used in the context of a 3 round protocol.
// TODO(QF): implements a waiting timeout mechanism

#include <bftengine/ReplicaInfo.hpp>
#include "QuorumStarterMsg.hpp"
#include "QuorumVoteMsg.hpp"
#include <queue>
#include <iostream>


namespace bftEngine {
namespace impl {

///////////////////////////////////////////////////////////////////////////////
// QuorumVoteCollection
///////////////////////////////////////////////////////////////////////////////


QuorumVoteCollection::QuorumVoteCollection(ReplicaId owner){
    ownerId = owner;
    votes = new std::queue<QuorumVoteMsg *>;
    collected = false;
    voteCnt = 0;
}

bool QuorumVoteCollection::addVoteMsg(QuorumVoteMsg *voteMsg){
    bool status = isVoteValid(voteMsg);
    if (status) {
        votes.push(voteMsg);
        voteCnt++;
    }
    return status;
}

bool QuorumVoteCollection::isReady(const ReplicasInfo *repsInfo) const{
    return !collected && voteCnt>=calcMajorityNum(repsInfo);
}

int16_t QuorumVoteCollection::calcMajorityNum(const ReplicasInfo *repsInfo) const{
    return repsInfo->numberOfReplicas()/2;
}

bool QuorumVoteCollection::isCollected() const{
    return collected;
}

void QuorumVoteCollection::setCollected(bool status){
    collected = status;
}

void QuorumVoteCollection::free(){
    while(!votes.empty()){votes.pop();}
}

bool QuorumVoteCollection::isVoteValid(QuorumVoteMsg *newVoteMsg) const{
    if (votes.empty()) return true;
    bool identicalSenderFlag = false;
    bool identicalMsgFlag = false;  // TODO(QF): same flags?
    for(auto it=votes.begin(); it!=votes.end();++it){
        if (newVoteMsg->equals(*it)){
            identicalMsgFlag = true;
            std::cout<<"Primary "<<ownerId<<" received a repetitive quorum vote msg from sender replica "<<newVoteMsg->senderId()<<endl;  //TODO(QF): is (NodeIdType) senderId printable?
            break;  // TODO(QF): change cout to LOG_DEBUG
        }
        if (newVoteMsg->senderId() == *it->senderId()){  // TODO(QF): or equals
            identicalSenderFlag = true;
            std::cout<<"Primary "<<ownerId<<" received a quorum vote msg from the same sender replica but different content "<<newVoteMsg->senderId()<<endl;  //TODO(QF): is (NodeIdType) senderId printable?
            break;
        }
    }
    return !(identialMsgFlag||identicalSenderFlag);
}

///////////////////////////////////////////////////////////////////////////////
// QuorumStarterMsg
///////////////////////////////////////////////////////////////////////////////

QuorumStarterMsg::QuorumStarterMsg(SeqNum s, ViewNum v, ReplicaId senderId) // TODO(QF): do we need spanContext and msgSize as param
        : MessageBase(senderId,
                      MsgCode:QuorumStarter, //TODO(QF): needs to implement msgCode?
                      sizeof(Header)){ // do we need to send any content in the msg?
    b()->viewNum = v;
    b()->seqNum = s;
    voteCollection = new QuorumVoteCollection(senderId);
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