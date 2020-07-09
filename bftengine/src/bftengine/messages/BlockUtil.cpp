#include "Crypto.hpp"

namespace bftEngine{
namespace impl{

void Block::addRequest(const char* pRequest, unit32_t requestSize){

}

void Block::finishAddingRequests(){

}

uint32_t Block::remainingSizeForRequests() const{

}

CertifiedBlock::CertifiedBlock(Block& b, ViewNum v, SeqNum s){
    // check request digest
    Digest d;
    const char* requestBuffer = (char*)&(b.numberOfRequests);
    const uint32_t requestSize = b.endLocationOfLastRequest;  //TODO(QF): check

    DigestUtil::compute(requestBuffer, requestSize, (char*)&d, sizeof(Digest));

    if (d != b.digestOfRequests) throw std::runtime_error(__PRETTY_FUNCTION__ + std::string(": digest"));

    flags = b.flags;
    numberOfRequests = b.numberOfRequests;
    endLocationOfLastRequest = b.endLocationOfLastRequest;
    digestOfRequests = b.digestOfRequests;
    viewNum = v;
    seqNum = s;
}

BlockRequestsIterator::BlockRequestsIterator(const Block* const b){
    Assert(b->isReady())
}

void BlockRequestsIterator::restart(){

}

bool BlockRequestsIterator::getCurrent(char*& pRequest) const{
    if 
}

bool BlockRequestsIterator::end() const{

}

void BlockRequestsIterator::gotoNext(){

}

bool BlockRequestsIterator::getAndGoToNext(char*& pRequest){

}
}
}