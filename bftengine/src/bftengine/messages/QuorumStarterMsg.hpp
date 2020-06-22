#include <queue>
#include "QuorumVoteMsg.hpp"

namespace bftEngine{
namespace impl{

class QuorumVoteCollection{
    public:
        QuorumVoteCollection(ReplicaId owner, int16_t size);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady();

    protected:
        int16_t size;
        int16_t voteCnt = 0;  
        
        std::queue<QuorumVoteMsg *> votes;
        static int16_t calcMajorityNum();
};

class QuorumStarterMsg : public MessageBase{
    public:
        bool QuorumStarterMsg(SeqNum s, ViewNum v, ReplicaId senderId,  int16_t replicaNum);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady();

    protected:
        QuorumVoteCollection *voteCollection;
};

}  // namespace bftEngine
}  // namespace impl