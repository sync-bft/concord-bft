#include <queue>
#include "QuorumVoteMsg.hpp"

namespace bftEngine{
namespace impl{

class QuorumVoteCollection{
    public:
        QuorumVoteCollection(ReplicaId owner, int16_t size);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady(const ReplicasInfo *repsInfo) const;

    protected:
        ReplicaId ownerId;
        int16_t voteCnt = 0;  
        
        std::queue<QuorumVoteMsg *> votes;
        int16_t calcMajorityNum(const ReplicasInfo *repsInfo) const;
        bool isVoteValid(QuorumVoteMsg *) const;
};

class QuorumStarterMsg : public MessageBase{
    public:
        bool QuorumStarterMsg(SeqNum s, ViewNum v, ReplicaId senderId);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady(const ReplicasInfo *repsInfo) const;

    protected:
        QuorumVoteCollection *voteCollection;
        friend class QuorumVoteCollection;  // TODO(QY): is the friendship needed
};

}  // namespace bftEngine
}  // namespace impl