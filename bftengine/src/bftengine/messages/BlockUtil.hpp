#pragma once

#include <queue>

#include "PrimitiveTypes.hpp"
#include "Digest.hpp"
#include "Logger.hpp"

//TODO: figure out size, span_context

namespace bftEngine{
namespace impl{

class BlockRequestsIterator;

class Block{
    
    public:

        size_t size() {return }

        void addRequest(const char* pRequest, unit32_t requestSize);
        
        void finishAddingRequests();

        uint32_t remainingSizeForRequests() const;

        uint32_t payloadShift() const {return sizeof(flags) + sizeof(numberOfRequests) +sizeof(endLocationOfLastRequest);}
        
        Digest& digestOfRequests() const {return digestOfRequests;}
        
        uint16_t numberOfRequests() const {return numberOfRequests;}

        bool isNull() const { return ((b()->flags & 0x1) == 0); }

        bool isReady() const { return (((b()->flags >> 1) & 0x1) == 1); }

    protected:

        uint8_t flags = (uint8_t)0;
        uint16_t numberOfRequests;
        uint32_t endLocationOfLastRequest;
        Digest digestOfRequests;

        // bits in flags
        // bit 0: 0=null , 1=non-null
        // bit 1: 0=not ready , 1=ready
        // bits 2-7: zero

        friend class BlockRequestsIterator;

};

class CertifiedBlock : public Block{
    public:
        CertifiedBlock(Block& b, ViewNum v, SeqNum s);

        ViewNum viewNum() const {return viewNum;}
        SeqNum seqNum() const {return seqNum;}

        bool operator<(const CertifiedBlock& other) const {
            return (viewNum < other.viewNum() && seqNum < other.seqNum);
        }

        bool operator>(const CertifiedBlock& other) const {
            return (viewNum > other.viewNum() && seqNum > other.seqNum);
        }

        bool operator==(const CertifiedBlock& other) const {
            if (viewNum == other.viewNum() && seqNum == other.seqNum){
                if (digestOfRequests != other.digestOfRequests()){
                    LOG_ERROR(GL, "Equivocation is detected. Two blocks with the same view number ["<<
                                   viewNum << "] and the same sequence number (height) [" <<<
                                   seqNum << "].");
                    return false;
                }
                else return true;
            }
            return false;
        }

    protected:
        ViewNum viewNum;
        SeqNum seqNum; // height
};

class CertifiedBlockContainer: {
    public:
        CertifiedBlockContainer(bool isAscending = false){
            auto comp = auto comp = []( CertifiedBlock lb, CertifiedBlock rb ) { return isAscending?lb<rb:lb>rb; };
            priority_queue<CertifiedBlock, decltype(comp)> q;
        };
        void push(CertifiedBlock& b) {q.push(b)};
        CertifiedBlock& top() {return q.top();};
        bool empty() {return q.empty();};
        size_t size() {return q.size();};
        void pop() {q.pop();};

    protected:
        priority_queue<CertifiedBlock> q;
}

class BlockRequestsIterator {
    public:
        BlockRequestsIterator(const Block* const b);

        void restart();

        bool getCurrent(char*& pRequest) const;

        bool end() const;

        void gotoNext();

        bool getAndGoToNext(char*& pRequest);

    protected:
        const Block* const block;
        unit32_t currLoc;
};
        
}}