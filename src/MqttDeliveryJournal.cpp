#include "MqttDeliveryJournal.h"
#include "MramStorage.h"
#include <cstring>

uint32_t MqttDeliveryJournal::crc32(const void* data, size_t len) {
  const uint8_t* p=static_cast<const uint8_t*>(data); uint32_t c=0xFFFFFFFFUL;
  while(len--){ c^=*p++; for(uint8_t i=0;i<8;++i)c=(c&1U)?(c>>1)^0xEDB88320UL:c>>1; }
  return ~c;
}
bool MqttDeliveryJournal::begin() {
  ready_=false; generation_=0; activeBank_=0; std::memset(entries_,0,sizeof(entries_));
  MramStorage& m=MramStorage::shared(); if(!m.begin()) return false;
  uint32_t bestGen=0; uint8_t best=0; bool have=false;
  for(uint8_t bank=0;bank<2;++bank){
    DiskEntry disk[MAX_ENTRIES]{}; const uint16_t base=bank?BANK1:BANK0;
    if(!m.read(base,disk,sizeof(disk))) continue;
    bool validBank=true; uint32_t maxGen=0;
    for(size_t i=0;i<MAX_ENTRIES;++i){
      if(!disk[i].entry.packetId && !disk[i].entry.sampleId && !disk[i].generation) continue;
      if(disk[i].crc!=crc32(&disk[i],offsetof(DiskEntry,crc))){validBank=false;break;}
      if(disk[i].generation>maxGen)maxGen=disk[i].generation;
    }
    if(validBank && (!have || static_cast<int32_t>(maxGen-bestGen)>0)){best=bank;bestGen=maxGen;have=true;}
  }
  if(have){
    DiskEntry disk[MAX_ENTRIES]{}; if(!m.read(best?BANK1:BANK0,disk,sizeof(disk))) return false;
    for(size_t i=0;i<MAX_ENTRIES;++i){entries_[i].entry=disk[i].entry;entries_[i].generation=disk[i].generation;entries_[i].valid=disk[i].entry.packetId||disk[i].entry.sampleId;}
    generation_=bestGen; activeBank_=best;
  }
  ready_=true; return true;
}
bool MqttDeliveryJournal::persistAll(){
  if(!ready_) return false; DiskEntry disk[MAX_ENTRIES]{}; uint32_t next=generation_+1U; if(!next)next=1;
  for(size_t i=0;i<MAX_ENTRIES;++i){if(!entries_[i].valid)continue;disk[i].entry=entries_[i].entry;disk[i].generation=next;disk[i].crc=crc32(&disk[i],offsetof(DiskEntry,crc));}
  MramStorage& m=MramStorage::shared(); const uint8_t target=activeBank_^1U; const uint16_t base=target?BANK1:BANK0;
  if(!m.write(base,disk,sizeof(disk)))return false; activeBank_=target;generation_=next;return true;
}
bool MqttDeliveryJournal::pending(uint16_t packetId,uint32_t sampleId){
  if(!ready_||!packetId||!sampleId)return false;
  size_t slot=MAX_ENTRIES;
  for(size_t i=0;i<MAX_ENTRIES;++i){if(entries_[i].valid&&entries_[i].entry.packetId==packetId){slot=i;break;}if(slot==MAX_ENTRIES&&!entries_[i].valid)slot=i;}
  if(slot==MAX_ENTRIES)return false;
  entries_[slot].entry={packetId,sampleId,static_cast<uint8_t>(State::PENDING),{0,0,0}};entries_[slot].valid=true;
  return persistAll();
}
bool MqttDeliveryJournal::complete(uint16_t packetId,bool delivered){
  if(!ready_||!packetId)return false;
  for(auto& e:entries_)if(e.valid&&e.entry.packetId==packetId){
    if(delivered){e.valid=false;e.entry={};}else e.entry.state=static_cast<uint8_t>(State::PENDING_RETRY);
    return persistAll();
  }
  return false;
}
