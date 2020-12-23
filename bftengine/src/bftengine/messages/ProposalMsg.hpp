#pragma once

#include <cstdint>

#include "MessageBase.hpp"
#include "PrimitiveTypes.hpp"
#include "assertUtils.hpp"
// #include "SignedShareMsgs.hpp"
#include "Digest.hpp"
#include "ReplicaConfig.hpp"

namespace bftEngine{
namespace impl{
class ContentIterator;

class ProposalMsg : public MessageBase{

    protected:
        template <typename MessageT>
        friend size_t sizeOfHeader();
    
    #pragma pack(push, 1)
        struct Header {
            MessageBase::Header header;
            ViewNum viewNum;
            SeqNum seqNum;
            uint16_t flags;
            bool isFirstMsg;
            bool isForwardedMsg;
            uint16_t combinedSigLen;
            Digest digestOfRequests;

            //SeqNum seqNumDigestFill;  // used to calculate digest
            uint16_t numberOfRequests;
            uint32_t endLocationOfLastRequest;

            // bits in flags
            // bit 0: 0=null , 1=non-null
            // bit 1: 0=not ready , 1=ready
            // bits 2-15: zeros
        };

    #pragma pack(pop)
      static_assert(sizeof(Header) == (6 + 8 + 8 + 2 + 1 + 1 + 2 + DIGEST_SIZE + 2 + 4), "Header is 74B");

      static const size_t proposalHeaderPrefix =
        sizeof(Header) - sizeof(Header::numberOfRequests) - sizeof(Header::endLocationOfLastRequest);

    
    public:

        void validate(const ReplicasInfo&) const override;

        ProposalMsg(ReplicaId sender, ViewNum v, SeqNum s, const char* combinedSigBody, size_t combinedSigLength, size_t size, bool isFirst);

        ProposalMsg(ReplicaId sender, ViewNum v, SeqNum s, const char* combinedSigBody, size_t combinedSigLength, const std::string& spanContext, size_t size, bool isFirst);

        uint32_t remainingSizeForRequests() const;

        void addRequest(const char* pRequest, uint32_t requestSize);

        void finishAddingRequests();

        bool isNull() const { return ((b()->flags & 0x1) == 0); }

        const std::string getClientCorrelationIdForMsg(int index) const;

        const std::string getBatchCorrelationIdAsString() const;

        // getter methods

        ViewNum viewNumber() const {return b()->viewNum;}

        SeqNum seqNumber() const {return b()->seqNum;}
        
        uint16_t numberOfRequests() const { return b()->numberOfRequests; }

        Digest& digestOfRequestsSeqNum() const {return b()->digestOfRequests;}
    
        size_t combinedSigLength() const {return combinedSigLen;}

        char* combinedSigBody() const { return body() + sizeof(Header) +  b()->header.spanContextSize; }
    
        bool isFirstMsg() const {return b()->isFirstMsg;}

        bool isForwardedMsg() const {return b()->isForwardedMsg;}

        void setForwardedMsg(bool status) {b()->isForwardedMsg = status;}

    protected:

        size_t combinedSigLen;

        static int16_t computeFlagsForProposalMsg(bool isNull, bool isReady);

        bool isReady() const { return (((b()->flags >> 1) & 0x1) == 1); }

        // bool checkRequests() const;

        Header* b() const { return (Header*)msgBody_; }

        uint32_t requestsPayloadShift() const;

        friend class ContentIterator;

};

class ContentIterator {
 public:
  ContentIterator(const ProposalMsg* const m);

  void restart();

  bool getCurrent(char*& pContent) const;

  bool end() const;

  void gotoNext();

  bool getAndGoToNext(char*& pContent);

 protected:
  const ProposalMsg* const msg;
  uint32_t currLoc;
};

template <>
inline MsgSize maxMessageSize<ProposalMsg>() {
  return ReplicaConfigSingleton::GetInstance().GetMaxExternalMessageSize() + MessageBase::SPAN_CONTEXT_MAX_SIZE;
}

}  //  namespace impl
}  //  namespace bftEngine