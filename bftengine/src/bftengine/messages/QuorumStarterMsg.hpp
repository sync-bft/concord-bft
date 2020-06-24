#include <queue>
#include "QuorumVoteMsg.hpp"
#include "MessageBase.hpp"

namespace bftEngine{
namespace impl{

class QuorumVoteCollection{
    public:
        QuorumVoteCollection(ReplicaId owner, int16_t size);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady(const ReplicasInfo *repsInfo) const;
        bool isCollected() const;
        void setCollected(bool status);
        void free();

    protected:
        ReplicaId ownerId;
        int16_t voteCnt;
        bool collected;
        
        std::queue<QuorumVoteMsg *> votes;
        int16_t calcMajorityNum(const ReplicasInfo *repsInfo) const;
        bool isVoteValid(QuorumVoteMsg *) const;
};

class QuorumStarterMsg : public MessageBase{
    public:
        QuorumStarterMsg(SeqNum s, ViewNum v, ReplicaId senderId);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady(const ReplicasInfo *repsInfo);
        bool isCollected() const;
        void setCollected(bool status);
        void freeCollection();

    protected:
        QuorumVoteCollection voteCollection;
        friend class QuorumVoteCollection;  // TODO(QY): is the friendship needed
};

}  // namespace bftEngine
}  // namespace impl