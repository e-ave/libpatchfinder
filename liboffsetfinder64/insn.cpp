//
//  insn.cpp
//  liboffsetfinder64
//
//  Created by tihmstar on 09.03.18.
//  Copyright © 2018 tihmstar. All rights reserved.
//

#define LOCAL_FILENAME "insn.cpp"

#include "all_liboffsetfinder.hpp"
#include <liboffsetfinder64/insn.hpp>
#include <liboffsetfinder64/exception.hpp>

using namespace tihmstar::patchfinder64;

insn::insn(segment_t segments, loc_t p, segtype segType) : _segments(segments), _segtype(segType){
    std::sort(_segments.begin(),_segments.end(),[ ]( const text_t& lhs, const text_t& rhs){
        return lhs.base < rhs.base;
    });
    if (_segtype != kText_and_Data) {
        _segments.erase(std::remove_if(_segments.begin(), _segments.end(), [&](const text_t obj){
            return (!obj.isExec) == (_segtype == kText_only);
        }));
    }
    if (p == 0) {
        p = _segments.at(0).base;
    }
    for (int i=0; i<_segments.size(); i++){
        auto seg = _segments[i];
        if ((loc_t)seg.base <= p && p < (loc_t)seg.base+seg.size){
            _p = {p,i};
            return;
        }
    }
    reterror("initializing insn with out of range location");
}

insn::insn(const insn &cpy, loc_t p){
    _segments = cpy._segments;
    _segtype = cpy._segtype;
    if (p==0) {
        _p = cpy._p;
    }else{
        for (int i=0; i<_segments.size(); i++){
            auto seg = _segments[i];
            if ((loc_t)seg.base <= p && p < (loc_t)seg.base+seg.size){
                _p = {p,i};
                return;
            }
        }
        reterror("initializing insn with out of range location");
    }
}

insn &insn::operator++(){
    _p.first+=4;
    if (_p.first >=_segments[_p.second].base+_segments[_p.second].size){
        if (_p.second+1 < _segments.size()) {
            _p.first = _segments[++_p.second].base;
        }else{
            _p.first-=4;
            throw out_of_range("overflow");
        }
    }
    return *this;
}

insn &insn::operator--(){
    _p.first-=4;
    if (_p.first < _segments[_p.second].base){
        if (_p.second-1 >0) {
            --_p.second;
            _p.first = _segments[_p.second].base+_segments[_p.second].size;
        }else{
            _p.first+=4;
            throw out_of_range("underflow");
        }
    }
    return *this;
}

insn insn::operator+(int i){
    insn cpy(*this);
    if (i>0) {
        while (i--)
            ++cpy;
    }else{
        while (i++)
            --cpy;
    }
    return cpy;
}

insn insn::operator-(int i){
    return this->operator+(-i);
}

insn &insn::operator+=(int i){
    if (i>0) {
        while (i-->0)
            this->operator++();
    }else{
        while (i++>0)
            this->operator--();
    }
    return *this;
}

insn &insn::operator-=(int i){
    return this->operator+=(-i);
}

insn &insn::operator=(loc_t p){
    for (int i=0; i<_segments.size(); i++){
        auto seg = _segments[i];
        if ((loc_t)seg.base <= p && p < (loc_t)seg.base+seg.size){
            _p = {p,i};
            return *this;
        }
    }
    reterror("initializing insn with out of range location");
}

#pragma mark reference manual helpers
__attribute__((always_inline)) static int64_t signExtend64(uint64_t v, int vSize){
    uint64_t e = (v & 1 << (vSize-1))>>(vSize-1);
    for (int i=vSize; i<64; i++)
        v |= e << i;
    return v;
}

__attribute__((always_inline)) static int highestSetBit(uint64_t x){
    for (int i=63; i>=0; i--) {
        if (x & ((uint64_t)1<<i))
            return i;
    }
    return -1;
}

__attribute__((always_inline)) static int lowestSetBit(uint64_t x){
    for (int i=0; i<=63; i++) {
        if (x & (1<<i))
            return i;
    }
    return 64;
}

__attribute__((always_inline)) static uint64_t replicate(uint8_t val, int bits){
    uint64_t ret = val;
    unsigned shift;
    for (shift = bits; shift < 64; shift += bits) {    // XXX actually, it is either 32 or 64
        ret |= (val << shift);
    }
    return ret;
}

__attribute__((always_inline)) static uint64_t ones(uint64_t n){
    uint64_t ret = 0;
    while (n--) {
        ret <<=1;
        ret |= 1;
    }
    return ret;
}

__attribute__((always_inline)) static uint64_t ROR(uint64_t x, int shift, int len){
    while (shift--) {
        x |= (x & 1) << len;
        x >>=1;
    }
    return x;
}

__attribute__((always_inline)) static std::pair<int64_t, int64_t> DecodeBitMasks(uint64_t immN, uint8_t imms, uint8_t immr, bool immediate){
    int64_t tmask = 0, wmask = 0;
    int8_t levels = 0;

    int len = highestSetBit( (uint64_t)((immN<<6) | ((~imms) & 0b111111)) );
    assure(len != -1); //reserved value
    levels = ones(len);

    assure(immediate && (imms & levels) != levels); //reserved value

    uint8_t S = imms & levels;
    uint8_t R = immr & levels;

    uint8_t esize = 1 << len;

    uint8_t welem = ones(S + 1);
    wmask = replicate(ROR(welem, R, 32),esize);
#warning TODO incomplete function implementation!
    return {wmask,0};
}

#pragma mark bridges
uint64_t insn::pc(){
    return (uint64_t)_p.first;
}

uint32_t insn::value(){
    return *(uint32_t*)(_p.first - _segments[_p.second].base + _segments[_p.second].map);
}

uint64_t insn::doublevalue(){
    return *(uint64_t*)(_p.first - _segments[_p.second].base + _segments[_p.second].map);
}

#pragma mark static type determinition

uint64_t insn::deref(segment_t segments, loc_t p){
    return insn(segments, p, insn::kText_and_Data).doublevalue();
}

bool insn::is_adrp(uint32_t i){
    return BIT_RANGE(i, 24, 28) == 0b10000 && (i>>31);
}

bool insn::is_adr(uint32_t i){
    return BIT_RANGE(i, 24, 28) == 0b10000 && !(i>>31);
}

bool insn::is_add(uint32_t i){
    return BIT_RANGE(i, 24, 28) == 0b10001;
}

bool insn::is_bl(uint32_t i){
    return (i>>26) == 0b100101;
}

bool insn::is_cbz(uint32_t i){
    return BIT_RANGE(i, 24, 30) == 0b0110100;
}

bool insn::is_ret(uint32_t i){
    return ((0b11111 << 5) | i) == 0b11010110010111110000001111100000;
}

bool insn::is_tbnz(uint32_t i){
    return BIT_RANGE(i, 24, 30) == 0b0110111;
}

bool insn::is_br(uint32_t i){
    return ((0b11111 << 5) | i) == 0b11010110000111110000001111100000;
}

bool insn::is_ldr(uint32_t i){
#warning TODO recheck this mask
    return (((i>>22) | 0b0100000000) == 0b1111100001 && ((i>>10) % 4)) || ((i>>22 | 0b0100000000) == 0b1111100101) || ((i>>23) == 0b00011000);
}

bool insn::is_cbnz(uint32_t i){
    return BIT_RANGE(i, 24, 30) == 0b0110101;
}

bool insn::is_movk(uint32_t i){
    return BIT_RANGE(i, 23, 30) == 0b11100101;
}

bool insn::is_orr(uint32_t i){
    return BIT_RANGE(i, 23, 30) == 0b01100100;
}

bool insn::is_tbz(uint32_t i){
    return BIT_RANGE(i, 24, 30) == 0b0110110;
}

bool insn::is_ldxr(uint32_t i){
    return (BIT_RANGE(i, 24, 29) == 0b001000) && (i >> 31) && BIT_AT(i, 22);
}

bool insn::is_ldrb(uint32_t i){
    return BIT_RANGE(i, 21, 31) == 0b00111000010 || //Immediate post/pre -indexed
           BIT_RANGE(i, 22, 31) == 0b0011100101  || //Immediate unsigned offset
           (BIT_RANGE(i, 21, 31) == 0b00111000011 && BIT_RANGE(i, 10, 11) == 0b10); //Register
}

bool insn::is_str(uint32_t i){
#warning TODO redo this! currently only recognises STR (immediate)
    return (BIT_RANGE(i, 22, 29) == 0b11100100) && (i >> 31);
}

bool insn::is_stp(uint32_t i){
#warning TODO redo this! currently only recognises STR (immediate)
    return (BIT_RANGE(i, 25, 30) == 0b010100) && !BIT_AT(i, 22);
}

bool insn::is_movz(uint32_t i){
    return (BIT_RANGE(i, 23, 30) == 0b10100101);
}

bool insn::is_bcond(uint32_t i){
    return (BIT_RANGE(i, 24, 31) == 0b01010100) && !BIT_AT(i, 4);
}

bool insn::is_b(uint32_t i){
    return (BIT_RANGE(i, 26, 31) == 0b000101);
}

bool insn::is_nop(uint32_t i){
    return (BIT_RANGE(i, 12, 31) == 0b11010101000000110010) && (0b11111 % (1<<5));
}


enum insn::type insn::type(){
    uint32_t val = value();
    if (is_adrp(val))
        return adrp;
    else if (is_adr(val))
        return adr;
    else if (is_add(val))
        return add;
    else if (is_bl(val))
        return bl;
    else if (is_cbz(val))
        return cbz;
    else if (is_ret(val))
        return ret;
    else if (is_tbnz(val))
        return tbnz;
    else if (is_br(val))
        return br;
    else if (is_ldr(val))
        return ldr;
    else if (is_cbnz(val))
        return cbnz;
    else if (is_movk(val))
        return movk;
    else if (is_orr(val))
        return orr;
    else if (is_tbz(val))
        return tbz;
    else if (is_ldxr(val))
        return ldxr;
    else if (is_ldrb(val))
        return ldrb;
    else if (is_str(val))
        return str;
    else if (is_stp(val))
        return stp;
    else if (is_movz(val))
        return movz;
    else if (is_bcond(val))
        return bcond;
    else if (is_b(val))
        return b;
    else if (is_nop(val))
        return nop;

    return unknown;
}

enum insn::subtype insn::subtype(){
    uint32_t i = value();
    if (is_ldr(i)) {
        if ((((i>>22) | (1 << 8)) == 0b1111100001) && BIT_RANGE(i, 10, 11) == 0b10)
            return st_register;
        else if (i>>31)
            return st_immediate;
        else
            return st_literal;
    }else if (is_ldrb(i)){
        if (BIT_RANGE(i, 21, 31) == 0b00111000011 && BIT_RANGE(i, 10, 11) == 0b10)
            return st_register;
        else
            return st_immediate;
    }
    return st_general;
}

enum insn::supertype insn::supertype(){
    switch (type()) {
        case bl:
        case cbz:
        case cbnz:
        case tbnz:
        case bcond:
        case b:
            return sut_branch_imm;

        default:
            return sut_general;
    }
}

#pragma mark register

int64_t insn::imm(){
    switch (type()) {
        case unknown:
            reterror("can't get imm value of unknown instruction");
            break;
        case adrp:
            return ((pc()>>12)<<12) + signExtend64(((((value() % (1<<24))>>5)<<2) | BIT_RANGE(value(), 29, 30))<<12,32);
        case adr:
            return pc() + signExtend64((BIT_RANGE(value(), 5, 23)<<2) | (BIT_RANGE(value(), 29, 30)), 21);
        case add:
            return BIT_RANGE(value(), 10, 21) << (((value()>>22)&1) * 12);
        case bl:
            return pc() + (signExtend64(value() % (1<<26), 25) << 2); //untested
        case cbz:
        case cbnz:
        case tbnz:
        case bcond:
            return pc() + (signExtend64(BIT_RANGE(value(), 5, 23), 19)<<2); //untested
        case movk:
        case movz:
            return BIT_RANGE(value(), 5, 20);
        case ldr:
            if(subtype() != st_immediate){
                reterror("can't get imm value of ldr that has non immediate subtype");
                break;
            }
            if(BIT_RANGE(value(), 24, 25)){
                // Unsigned Offset
                return BIT_RANGE(value(), 10, 21) << (value()>>30);
            }else{
                // Signed Offset
                return signExtend64(BIT_RANGE(value(), 12, 21), 9); //untested
            }
        case ldrb:
            if (st_immediate) {
                if (BIT_RANGE(value(), 22, 31) == 0b0011100101) { //unsigned
                    return BIT_RANGE(value(), 10, 21) << BIT_RANGE(value(), 30, 31);
                }else{  //pre/post indexed
                    return BIT_RANGE(value(), 12, 20) << BIT_RANGE(value(), 30, 31);
                }
            }else{
                reterror("ldrb must be st_immediate for imm to be defined!");
            }
        case str:
#warning TODO rewrite this! currently only unsigned offset supported
            // Unsigned Offset
            return BIT_RANGE(value(), 10, 21) << (value()>>30);
        case orr:
            return DecodeBitMasks(BIT_AT(value(), 22),BIT_RANGE(value(), 10, 15),BIT_RANGE(value(), 16,21), true).first;
        case tbz:
            return BIT_RANGE(value(), 5, 18);
        case stp:
            return signExtend64(BIT_RANGE(value(), 15, 21),7) << (2+(value()>>31));
        case b:
            return pc() + ((value() % (1<< 26))<<2);
        default:
            reterror("failed to get imm value");
            break;
    }
    return 0;
}

uint8_t insn::rd(){
    switch (type()) {
        case unknown:
            reterror("can't get rd of unknown instruction");
            break;
        case adrp:
        case adr:
        case add:
        case movk:
        case orr:
        case movz:
            return (value() % (1<<5));

        default:
            reterror("failed to get rd");
            break;
    }
}

uint8_t insn::rn(){
    switch (type()) {
        case unknown:
            reterror("can't get rn of unknown instruction");
            break;
        case add:
        case ret:
        case br:
        case orr:
        case ldxr:
        case ldrb:
        case str:
        case ldr:
        case stp:
            return BIT_RANGE(value(), 5, 9);

        default:
            reterror("failed to get rn");
            break;
    }
}

uint8_t insn::rt(){
    switch (type()) {
        case unknown:
            reterror("can't get rt of unknown instruction");
            break;
        case cbz:
        case cbnz:
        case tbnz:
        case tbz:
        case ldxr:
        case ldrb:
        case str:
        case ldr:
        case stp:
            return (value() % (1<<5));

        default:
            reterror("failed to get rt");
            break;
    }
}

uint8_t insn::other(){
    switch (type()) {
        case unknown:
            reterror("can't get other of unknown instruction");
            break;
        case tbz:
            return ((value() >>31) << 5) | BIT_RANGE(value(), 19, 23);
        case stp:
            return BIT_RANGE(value(), 10, 14); //Rt2
        case bcond:
            return 0; //condition
        case ldrb:
            if (subtype() == st_register)
                reterror("ERROR: unimplemented!");
            else
                reterror("ldrb must be st_register for this to be defined!");
        default:
            reterror("failed to get other");
            break;
    }
}

#pragma mark cast operators
insn::operator void*(){
    return (void*)(_p.first - _segments[_p.second].base + _segments[_p.second].map);
}

insn::operator loc_t(){
    return (loc_t)pc();
}

insn::operator enum type(){
    return type();
}

#pragma mark additional functions
loc_t tihmstar::patchfinder64::find_literal_ref(segment_t segemts, loc_t pos, int ignoreTimes){
    insn adrp(segemts);
    uint8_t rd = 0xff;
    uint64_t imm = 0;
    
    try {
        while (1){
            if (adrp == insn::adr) {
                if (adrp.imm() == (uint64_t)pos){
                    if (ignoreTimes) {
                        ignoreTimes--;
                        rd = 0xff;
                        imm = 0;
                        continue;
                    }
                    return (loc_t)adrp.pc();
                }
            }else if (adrp == insn::adrp) {
                rd = adrp.rd();
                imm = adrp.imm();
            }else if (adrp == insn::add && rd == adrp.rd()){
                if (imm + adrp.imm() == (int64_t)pos){
                    if (ignoreTimes) {
                        ignoreTimes--;
                        rd = 0xff;
                        imm = 0;
                        continue;
                    }
                    return (loc_t)adrp.pc();
                }
            }
            ++adrp;
        }
    } catch (std::out_of_range &e) {
        return 0;
    }
    return 0;
}

loc_t tihmstar::patchfinder64::find_rel_branch_source(insn bdst, bool searchUp, int ignoreTimes, int limit){
    insn bsrc(bdst);

    bool hasLimit = (limit);
    while (true) {
        if (searchUp){
            while ((--bsrc).supertype() != insn::sut_branch_imm){
                if (hasLimit && !limit--)
                    retcustomerror("find_rel_branch_source: limit reached!",limit_reached);
            }
        }else{
            while ((++bsrc).supertype() != insn::sut_branch_imm){
                if (hasLimit && !limit--)
                    retcustomerror("find_rel_branch_source: limit reached!",limit_reached);
            }
        }

        if (bsrc.imm() == bdst.pc()) {
            if (ignoreTimes) {
                ignoreTimes--;
                continue;
            }
            return (loc_t)bsrc.pc();
        }
    }
    
    //this return is never reached
    return 0;
}

