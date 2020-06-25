#pragma once

#include "MessageBase.hpp"
#include "QuorumVoteMsg.hpp"
#include <vector>

namespace bftEngine {
namespace impl {
class QuorumVoteCollection{
    public:
        QuorumVoteCollection();
        QuorumVoteCollection(ReplicaId owner);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady(const ReplicasInfo *repsInfo) const;
        bool isCollected() const;
        void setCollected(bool status);
        void free();

    protected:
        ReplicaId ownerId;
        int16_t voteCnt = 0;
        bool collected = false;
        
        std::vector<QuorumVoteMsg *> votes;
        int16_t calcMajorityNum(const ReplicasInfo *repsInfo) const;
        bool isVoteValid(QuorumVoteMsg *) const;
};
}
}