#include "../bftengine/ReplicasInfo.hpp"
#include "QuorumVoteCollection.hpp"

#include <iostream>


namespace bftEngine {
namespace impl {

///////////////////////////////////////////////////////////////////////////////
// QuorumVoteCollection
///////////////////////////////////////////////////////////////////////////////
QuorumVoteCollection::QuorumVoteCollection(){
    ownerId = NULL;
}

QuorumVoteCollection::QuorumVoteCollection(ReplicaId owner){
    ownerId = owner;
}

bool QuorumVoteCollection::addVoteMsg(QuorumVoteMsg *voteMsg){
    bool status = isVoteValid(voteMsg);
    if (status) {
        votes.push_back(voteMsg);
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
    votes.clear();
}

bool QuorumVoteCollection::isVoteValid(QuorumVoteMsg *newVoteMsg) const{
    if (votes.empty()) return true;
    bool identicalSenderFlag = false;
    bool identicalMsgFlag = false;  // TODO(QF): same flags?
    for(auto it=votes.begin(); it!=votes.end(); ++it){
        if (newVoteMsg->equals(*it)){
            identicalMsgFlag = true;
            std::cout<<"Primary "<<ownerId<<" received a repetitive quorum vote msg from sender replica "<<newVoteMsg->senderId()<<std::endl;  //TODO(QF): is (NodeIdType) senderId printable?
            break;  // TODO(QF): change cout to LOG_DEBUG
        }
        if (newVoteMsg->senderId() == *it->senderId()){  // TODO(QF): or equals
            identicalSenderFlag = true;
            std::cout<<"Primary "<<ownerId<<" received a quorum vote msg from the same sender replica but different content "<<newVoteMsg->senderId()<<std::endl;  //TODO(QF): is (NodeIdType) senderId printable?
            break;
        }
    }
    return !(identicalMsgFlag||identicalSenderFlag);
}

}
}