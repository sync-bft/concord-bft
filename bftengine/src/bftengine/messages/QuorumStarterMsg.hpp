#pragma once

#include "QuorumVoteMsg.hpp"
#include "MessageBase.hpp"
#include "QuorumVoteCollection.hpp"

namespace bftEngine{
namespace impl{

class QuorumStarterMsg : public MessageBase{
    public:
        QuorumStarterMsg(SeqNum s, ViewNum v, ReplicaId senderId);
        bool addVoteMsg(QuorumVoteMsg *voteMsg);
        bool isReady(const ReplicasInfo *repsInfo) const;
        bool isCollected() const;
        void setCollected(bool status);
        void freeCollection();

        ViewNum viewNumber() const { return b()->viewNum; }

        SeqNum seqNumber() const { return b()->seqNum; }

    protected:
        QuorumVoteCollection voteCollection;
        friend class QuorumVoteCollection;  // TODO(QY): is the friendship needed

        template <typename MessageT>
        friend size_t sizeOfHeader();

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

}  // namespace bftEngine
}  // namespace impl