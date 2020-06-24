#include "MessageBase.hpp"

namespace bftEngine{
namespace impl{

class QuorumVoteMsg : public MessageBase{
    public:
        QuorumVoteMsg(SeqNum s, ViewNum v, ReplicaId senderId);

        ViewNum viewNumber() const { return b()->viewNum; }

        SeqNum seqNumber() const { return b()->seqNum; }
    
    #pragma pack(push, 1)
        struct Header {
            MessageBase::Header header;
            ViewNum viewNum;
            SeqNum seqNum;
            // uint16_t thresholSignatureLength;
            };
    
    #pragma pack(pop)
        static_assert(sizeof(Header) == (6 + 8 + 8), "Header is 22B");

        Header* b() const { return (Header*)msgBody_; }
};

}  // impl namespace
}  // bftEngine namespace