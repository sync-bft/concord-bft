#include "MessageBase.hpp"

namespace bftEngine{
namespace impl{

class QuorumVoteMsg : public MessageBase{
    public:
        QuorumVoteMsg(SeqNum s, ViewNum v, ReplicaId senderId);
};

}  // impl namespace
}  // bftEngine namespace