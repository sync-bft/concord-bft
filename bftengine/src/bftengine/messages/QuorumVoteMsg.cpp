// QuorumVote
//
// This implements the message that responds to a received quorum starter method
// The message could be sent by any replica (except the primary) and used in the context of a 3 round protocol
#include "QuorumVoteMsg.hpp"

namespace bftEngine{
namespace impl{

QuorumVoteMsg::QuorumVoteMsg(SeqNum s, ViewNum v, ReplicaId senderId) // TODO(QF): do we need spanContext and msgSize as param
        : MessageBase(senderId,
                      MsgCode::QuorumVote, 
                      sizeof(Header)){ 
    b()->viewNum = v;
    b()->seqNum = s;
}
}  // impl namespace
}  // bftEngine namespace