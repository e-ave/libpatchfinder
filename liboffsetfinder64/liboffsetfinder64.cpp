//
//  offsetfinder64.cpp
//  offsetfinder64
//
//  Created by tihmstar on 10.01.18.
//  Copyright © 2018 tihmstar. All rights reserved.
//

#include <liboffsetfinder64/liboffsetfinder64.hpp>

#define LOCAL_FILENAME "liboffsetfinder.cpp"
#include "all_liboffsetfinder.hpp"

extern "C"{
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "img4.h"
}

using namespace std;
using namespace tihmstar;
using namespace patchfinder64;

#pragma mark liboffsetfinder

#define HAS_BITS(a,b) (((a) & (b)) == (b))
#define _symtab getSymtab()

#define findstr(str,hasNullTerminator) memmem(str, sizeof(str)-(hasNullTerminator == 0))

#pragma mark macho external

__attribute__((always_inline)) struct load_command *find_load_command64(struct mach_header_64 *mh, uint32_t lc){
    struct load_command *lcmd = (struct load_command *)(mh + 1);
    for (uint32_t i=0; i<mh->ncmds; i++, lcmd = (struct load_command *)((uint8_t *)lcmd + lcmd->cmdsize)) {
        if (lcmd->cmd == lc)
            return lcmd;
    }
    
    retcustomerror(lc,load_command_not_found);
    return NULL;
}

__attribute__((always_inline)) struct symtab_command *find_symtab_command(struct mach_header_64 *mh){
    return (struct symtab_command *)find_load_command64(mh, LC_SYMTAB);
}

__attribute__((always_inline)) struct dysymtab_command *find_dysymtab_command(struct mach_header_64 *mh){
    return (struct dysymtab_command *)find_load_command64(mh, LC_DYSYMTAB);
}

__attribute__((always_inline)) struct section_64 *find_section(struct segment_command_64 *seg, const char *sectname){
    struct section_64 *sect = (struct section_64 *)(seg + 1);
    for (uint32_t i=0; i<seg->nsects; i++, sect++) {
        if (strcmp(sect->sectname, sectname) == 0)
            return sect;
    }
    reterror("Failed to find section "+ string(sectname));
    return NULL;
}

offsetfinder64::offsetfinder64(const char* filename) : _freeKernel(true),__symtab(NULL){
    struct stat fs = {0};
    int fd = 0;
    char *img4tmp = NULL;
    auto clean =[&]{
        if (fd>0) close(fd);
    };
    assure((fd = open(filename, O_RDONLY)) != -1);
    assureclean(!fstat(fd, &fs));
    assureclean((_kdata = (uint8_t*)malloc( _ksize = fs.st_size)));
    assureclean(read(fd,_kdata,_ksize)==_ksize);
    
    //check if feedfacf, fat, compressed (lzfse/lzss), img4, im4p
    img4tmp = (char*)_kdata;
    if (sequenceHasName(img4tmp, (char*)"IMG4")){
        img4tmp = getElementFromIMG4((char*)_kdata, (char*)"IM4P");
    }
    if (sequenceHasName(img4tmp, (char*)"IM4P")){
        char *extracted = NULL;
        {
            size_t klen;
            const char* compname;

            extracted = extractPayloadFromIM4P(img4tmp, &compname, &klen);

            if (compname) {
                printf("%s comp detected, uncompressing : %s ...\n", compname, extracted ? "success" : "failure");
            }
        }
        if (extracted != NULL) {
            free(_kdata);
            _kdata = (uint8_t*)extracted;
        }
    }

    if (*(uint32_t*)_kdata == 0xbebafeca || *(uint32_t*)_kdata == 0xcafebabe) {
        bool swap = *(uint32_t*)_kdata == 0xbebafeca;

        uint8_t* tryfat = [=]() -> uint8_t* {
            // just select first slice
            uint32_t* kdata32 = (uint32_t*) _kdata;
            uint32_t narch = kdata32[1];
            if (swap) narch = ntohl(narch);

            if (narch != 1) {
                printf("expected 1 arch in fat file, got %u\n", narch);
                return NULL;
            }

            uint32_t offset = kdata32[2 + 2];
            if (swap) offset = ntohl(offset);

            if (offset != sizeof(uint32_t)*(2 + 5)) {
                printf("wat, file offset not sizeof(fat_header) + sizeof(fat_arch)?!\n");
            }

            uint32_t filesize = kdata32[2 + 3];
            if (swap) filesize = ntohl(filesize);

            uint8_t *ret = (uint8_t*) malloc(filesize);
            if (ret != NULL) {
                memcpy(ret, _kdata + offset, filesize);
            }
            return ret;
        }();

        if (tryfat != NULL) {
            printf("got fat macho with first slice at %u\n", (uint32_t) (tryfat - _kdata));
            free(_kdata);
            _kdata = tryfat;
        } else {
            printf("got fat macho but failed to parse\n");
        }
    }
    
    assureclean(*(uint32_t*)_kdata == 0xfeedfacf);
    
    loadSegments();
    clean();
}

void offsetfinder64::loadSegments(){
    struct mach_header_64 *mh = (struct mach_header_64*)_kdata;
    struct load_command *lcmd = (struct load_command *)(mh + 1);
    for (uint32_t i=0; i<mh->ncmds; i++, lcmd = (struct load_command *)((uint8_t *)lcmd + lcmd->cmdsize)) {
        if (lcmd->cmd == LC_SEGMENT_64){
            struct segment_command_64* seg = (struct segment_command_64*)lcmd;
            _segments.push_back({_kdata+seg->fileoff,seg->filesize, (loc_t)seg->vmaddr, (seg->maxprot & VM_PROT_EXECUTE) !=0});
        }
        if (lcmd->cmd == LC_UNIXTHREAD) {
            uint32_t *ptr = (uint32_t *)(lcmd + 1);
            uint32_t flavor = ptr[0];
            struct _tread{
                uint64_t x[29];    /* General purpose registers x0-x28 */
                uint64_t fp;    /* Frame pointer x29 */
                uint64_t lr;    /* Link register x30 */
                uint64_t sp;    /* Stack pointer x31 */
                uint64_t pc;     /* Program counter */
                uint32_t cpsr;    /* Current program status register */
            } *thread = (struct _tread*)(ptr + 2);
            if (flavor == 6) {
                _kernel_entry = (patchfinder64::loc_t)(thread->pc);
            }
        }
    }
    
    info("Inited offsetfinder64 %s %s",OFFSETFINDER64_VERSION_COMMIT_COUNT, OFFSETFINDER64_VERSION_COMMIT_SHA);
    try {
        getSymtab();
    } catch (tihmstar::symtab_not_found &e) {
        info("Symtab not found. Assuming we are operating on a dumped kernel");
    }
    printf("\n");
}

offsetfinder64::offsetfinder64(void* buf, size_t size) : _freeKernel(false),_kdata((uint8_t*)buf),_ksize(size),__symtab(NULL){
    loadSegments();
}

const void *offsetfinder64::kdata(){
    return _kdata;
}

loc_t offsetfinder64::find_entry(){
    return _kernel_entry;
}

bool offsetfinder64::haveSymbols(){
    if (_haveSymtab == kuninitialized) {
        try {
            getSymtab();
            _haveSymtab = ktrue;
        } catch (tihmstar::symtab_not_found &e) {
            _haveSymtab = kfalse;
        }
    }
    return _haveSymtab;
}

#pragma mark macho offsetfinder
__attribute__((always_inline)) struct symtab_command *offsetfinder64::getSymtab(){
    if (!__symtab){
        try {
            __symtab = find_symtab_command((struct mach_header_64 *)_kdata);
        } catch (tihmstar::load_command_not_found &e) {
            if (e.cmd() != LC_SYMTAB)
                throw;
            retcustomerror("symtab not found. Is this a dumped kernel?", symtab_not_found);
        }
    }
    return __symtab;
}

#pragma mark offsetfidner

loc_t offsetfinder64::memmem(const void *little, size_t little_len){
    for (auto seg : _segments) {
        if (loc_t rt = (loc_t)::memmem(seg.map, seg.size, little, little_len)) {
            return rt-seg.map+seg.base;
        }
    }
    return 0;
}

loc_t offsetfinder64::find_sym(const char *sym){
    uint8_t *psymtab = _kdata + _symtab->symoff;
    uint8_t *pstrtab = _kdata + _symtab->stroff;

    struct nlist_64 *entry = (struct nlist_64 *)psymtab;
    for (uint32_t i = 0; i < _symtab->nsyms; i++, entry++)
        if (!strcmp(sym, (char*)(pstrtab + entry->n_un.n_strx)))
            return (loc_t)entry->n_value;

    retcustomerror("Failed to find symbol "+string(sym),symbol_not_found);
    return 0;
}

loc_t offsetfinder64::find_syscall0(){
    constexpr char sig_syscall_3[] = "\x06\x00\x00\x00\x03\x00\x0c\x00";
    loc_t sys3 = memmem(sig_syscall_3, sizeof(sig_syscall_3)-1);
    return sys3 - (3 * 0x18) + 0x8;
}


#pragma mark patchfinder64

namespace tihmstar{
    namespace patchfinder64{
        
        loc_t jump_stub_call_ptr_loc(insn bl_insn){
            assure(bl_insn == insn::bl);
            insn fdst(bl_insn,(loc_t)bl_insn.imm());
            insn ldr((fdst+1));
            if (!((fdst == insn::adrp && ldr == insn::ldr && (fdst+2) == insn::br))) {
                retcustomerror("branch destination not jump_stub_call", bad_branch_destination);
            }
            return (loc_t)fdst.imm() + ldr.imm();
        }
        
        bool is_call_to_jump_stub(insn bl_insn){
            try {
                jump_stub_call_ptr_loc(bl_insn);
                return true;
            } catch (tihmstar::bad_branch_destination &e) {
                return false;
            }
        }
        
    }
}

#pragma mark common patchs
constexpr char patch_nop[] = "\x1F\x20\x03\xD5";
constexpr size_t patch_nop_size = sizeof(patch_nop)-1;

uint64_t offsetfinder64::find_register_value(loc_t where, int reg, loc_t startAddr){
    insn functop(_segments, where);
    
    if (!startAddr) {
        //might be functop
        //good enough for my purpose
        while (--functop != insn::stp || (functop+1) != insn::stp || (functop+2) != insn::stp);
    }else{
        functop = startAddr;
    }
    
    uint64_t value[32] = {0};
    
    for (;(loc_t)functop.pc() < where;++functop) {
        
        switch (functop.type()) {
            case patchfinder64::insn::adrp:
                value[functop.rd()] = functop.imm();
//                printf("%p: ADRP X%d, 0x%llx\n", (void*)functop.pc(), functop.rd(), functop.imm());
                break;
            case patchfinder64::insn::add:
                value[functop.rd()] = value[functop.rn()] + functop.imm();
//                printf("%p: ADD X%d, X%d, 0x%llx\n", (void*)functop.pc(), functop.rd(), functop.rn(), (uint64_t)functop.imm());
                break;
            case patchfinder64::insn::adr:
                value[functop.rd()] = functop.imm();
//                printf("%p: ADR X%d, 0x%llx\n", (void*)functop.pc(), functop.rd(), functop.imm());
                break;
            case patchfinder64::insn::ldr:
//                printf("%p: LDR X%d, [X%d, 0x%llx]\n", (void*)functop.pc(), functop.rt(), functop.rn(), (uint64_t)functop.imm());
                value[functop.rt()] = value[functop.rn()] + functop.imm(); // XXX address, not actual value
                break;
            default:
                break;
        }
    }
    return value[reg];
}

#pragma mark v0rtex
loc_t offsetfinder64::find_zone_map(){
    loc_t str = findstr("zone_init",true);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");

    insn ptr(_segments,ref);
    
    loc_t ret = 0;
    
    while (++ptr != insn::adrp);
    ret = (loc_t)ptr.imm();
    
    while (++ptr != insn::add);
    ret += ptr.imm();
    
    return ret;
}

loc_t offsetfinder64::find_kernel_map(){
    return find_sym("_kernel_map");
}

loc_t offsetfinder64::find_kernel_task(){
    return find_sym("_kernel_task");
}

loc_t offsetfinder64::find_realhost(){
    loc_t sym = find_sym("_KUNCExecute");
    
    insn ptr(_segments,sym);
    
    loc_t ret = 0;
    
    while (++ptr != insn::adrp);
    ret = (loc_t)ptr.imm();
    
    while (++ptr != insn::add);
    ret += ptr.imm();
    
    return ret;
}

loc_t offsetfinder64::find_bzero(){
    return find_sym("___bzero");
}

loc_t offsetfinder64::find_bcopy(){
    return find_sym("_bcopy");
}

loc_t offsetfinder64::find_copyout(){
    return find_sym("_copyout");
}

loc_t offsetfinder64::find_copyin(){
    return find_sym("_copyin");
}

loc_t offsetfinder64::find_ipc_port_alloc_special(){
    loc_t sym = find_sym("_KUNCGetNotificationID");
    insn ptr(_segments,sym);
    
    while (++ptr != insn::bl);
    while (++ptr != insn::bl);
    
    return (loc_t)ptr.imm();
}

loc_t offsetfinder64::find_ipc_kobject_set(){
    loc_t sym = find_sym("_KUNCGetNotificationID");
    insn ptr(_segments,sym);
    
    while (++ptr != insn::bl);
    while (++ptr != insn::bl);
    while (++ptr != insn::bl);
    
    return (loc_t)ptr.imm();
}

loc_t offsetfinder64::find_ipc_port_make_send(){
    loc_t sym = find_sym("_convert_task_to_port");
    insn ptr(_segments,sym);
    while (++ptr != insn::bl);
    while (++ptr != insn::bl);
    
    return (loc_t)ptr.imm();
}

loc_t offsetfinder64::find_chgproccnt(){
    loc_t str = findstr("\"chgproccnt: lost user\"",true);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");
    
    insn functop(_segments,ref);
    
    while (--functop != insn::stp);
    while (--functop == insn::stp);
    ++functop;
    
    return (loc_t)functop.pc();
}

loc_t offsetfinder64::find_kauth_cred_ref(){
    return find_sym("_kauth_cred_ref");
}

loc_t offsetfinder64::find_osserializer_serialize(){
    return find_sym("__ZNK12OSSerializer9serializeEP11OSSerialize");
}

uint32_t offsetfinder64::find_vtab_get_external_trap_for_index(){
    loc_t sym = find_sym("__ZTV12IOUserClient");
    sym += 2*sizeof(uint64_t);
    
    loc_t nn = find_sym("__ZN12IOUserClient23getExternalTrapForIndexEj");
    
    insn data(_segments,sym,insn::kText_and_Data);
    --data;
    for (int i=0; i<0x200; i++) {
        if ((++data).doublevalue() == (uint64_t)nn)
            return i;
        ++data;
    }
    return 0;
}

uint32_t offsetfinder64::find_vtab_get_retain_count(){
    loc_t sym = find_sym("__ZTV12IOUserClient");
    sym += 2*sizeof(uint64_t);
    
    loc_t nn = find_sym("__ZNK8OSObject14getRetainCountEv");
    
    insn data(_segments,sym,insn::kText_and_Data);
    --data;
    for (int i=0; i<0x200; i++) {
        if ((++data).doublevalue() == (uint64_t)nn)
            return i;
        ++data;
    }
    return 0;
}

uint32_t offsetfinder64::find_proc_ucred(){
    loc_t sym = find_sym("_proc_ucred");
    return (uint32_t)insn(_segments,sym).imm();
}

uint32_t offsetfinder64::find_task_bsd_info(){
    loc_t sym = find_sym("_get_bsdtask_info");
    return (uint32_t)insn(_segments,sym).imm();
}

uint32_t offsetfinder64::find_vm_map_hdr(){
    loc_t sym = find_sym("_vm_map_create");
    
    insn stp(_segments, sym);
    
    while (++stp != insn::bl);

    while (++stp != insn::cbz);
    
    while (++stp != insn::stp || stp.rt() != stp.other());
    
    return (uint32_t)stp.imm();
}

typedef struct mig_subsystem_struct {
    uint32_t min;
    uint32_t max;
    char *names;
} mig_subsys;

mig_subsys task_subsys ={ 0xd48, 0xd7a , NULL};
uint32_t offsetfinder64::find_task_itk_self(){
    loc_t task_subsystem=memmem(&task_subsys, 4);
    assure(task_subsystem);
    task_subsystem += 4*sizeof(uint64_t); //index0 now
    
    insn mach_ports_register(_segments, (loc_t)insn::deref(_segments, task_subsystem+3*5*8));
    
    while (++mach_ports_register != insn::bl || mach_ports_register.imm() != (uint64_t)find_sym("_lck_mtx_lock"));
    
    insn ldr(mach_ports_register);
    
    while (++ldr != insn::ldr || (ldr+1) != insn::cbz);
    
    return (uint32_t)ldr.imm();
}

uint32_t offsetfinder64::find_task_itk_registered(){
    loc_t task_subsystem=memmem(&task_subsys, 4);
    assure(task_subsystem);
    task_subsystem += 4*sizeof(uint64_t); //index0 now
    
    insn mach_ports_register(_segments, (loc_t)insn::deref(_segments, task_subsystem+3*5*8));
    
    while (++mach_ports_register != insn::bl || mach_ports_register.imm() != (uint64_t)find_sym("_lck_mtx_lock"));
    
    insn ldr(mach_ports_register);
    
    while (++ldr != insn::ldr || (ldr+1) != insn::cbz);
    while (++ldr != insn::ldr);
    
    return (uint32_t)ldr.imm();
}


//IOUSERCLIENT_IPC
mig_subsys host_priv_subsys = { 400, 426 } ;
uint32_t offsetfinder64::find_iouserclient_ipc(){
    loc_t host_priv_subsystem=memmem(&host_priv_subsys, 8);
    assure(host_priv_subsystem);

    insn memiterator(_segments,host_priv_subsystem,insn::kData_only);
    loc_t thetable = 0;
    while (1){
        --memiterator;--memiterator; //dec 8 byte
        struct _anon{
            uint64_t ptr;
            uint64_t z0;
            uint64_t z1;
            uint64_t z2;
        } *obj = (struct _anon*)(void*)memiterator;
        
        if (!obj->z0 && !obj->z1 &&
            !memcmp(&obj[0], &obj[1], sizeof(struct _anon)) &&
            !memcmp(&obj[0], &obj[2], sizeof(struct _anon)) &&
            !memcmp(&obj[0], &obj[3], sizeof(struct _anon)) &&
            !memcmp(&obj[0], &obj[4], sizeof(struct _anon)) &&
            !obj[-1].ptr && obj[-1].z0 == 1 && !obj[-1].z1) {
            thetable = (loc_t)memiterator.pc();
            break;
        }
    }
    
    loc_t iokit_user_client_trap_func = (loc_t)insn::deref(_segments, thetable + 100*4*8 - 8);
    
    insn bl_to_iokit_add_connect_reference(_segments,iokit_user_client_trap_func);
    while (++bl_to_iokit_add_connect_reference != insn::bl);
    
    insn iokit_add_connect_reference(bl_to_iokit_add_connect_reference,(loc_t)bl_to_iokit_add_connect_reference.imm());
    
    while (++iokit_add_connect_reference != insn::add || iokit_add_connect_reference.rd() != 8 || ++iokit_add_connect_reference != insn::ldxr || iokit_add_connect_reference.rn() != 8);

    return (uint32_t)((--iokit_add_connect_reference).imm());
}

uint32_t offsetfinder64::find_ipc_space_is_task(){
    loc_t str = findstr("\"ipc_task_init\"",true);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");
    
    loc_t bref = 0;
    bool do_backup_plan = false;

    try {
        bref = find_rel_branch_source(insn(_segments,ref), true, 2, 0x2000);
    } catch (tihmstar::limit_reached &e) {
        try {
            //previous attempt doesn't work on some 10.0.2 devices, trying something else...
            do_backup_plan = bref = find_rel_branch_source(insn(_segments,ref), true, 1, 0x2000);
        } catch (tihmstar::limit_reached &ee) {
            //this seems to be good for iOS 9.3.3
            do_backup_plan = bref = find_rel_branch_source(insn(_segments,ref-4), true, 1, 0x2000);
        }
        
    }
    
    insn istr(_segments,bref);
    
    if (!do_backup_plan) {
        while (++istr != insn::str);
    }else{
        while (--istr != insn::str);
    }

    return (uint32_t)istr.imm();
}

uint32_t offsetfinder64::find_sizeof_task(){
    loc_t str = findstr("\0tasks",true)+1;
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");
    
    insn thebl(_segments, ref);
   
    loc_t zinit = 0;
    try {
        zinit = find_sym("_zinit");
    } catch (tihmstar::symbol_not_found &e) {
        loc_t str = findstr("zlog%d",true);
        retassure(str, "Failed to find str2");
        
        loc_t ref = find_literal_ref(_segments, str);
        retassure(ref, "literal ref to str2");
        
        insn functop(_segments,ref);
        while (--functop != insn::stp || (functop+1) != insn::stp || (functop+2) != insn::stp || (functop-1) != insn::ret);
        zinit = (loc_t)functop.pc();
    }
    
    while (++thebl != insn::bl || (loc_t)thebl.imm() != zinit);
    
    --thebl;
    
    return (uint32_t)thebl.imm();
}

loc_t offsetfinder64::find_rop_add_x0_x0_0x10(){
    constexpr char ropbytes[] = "\x00\x40\x00\x91\xC0\x03\x5F\xD6";
    return [](const void *little, size_t little_len, vector<text_t>segments)->loc_t{
        for (auto seg : segments) {
            if (!seg.isExec)
                continue;
            
            if (loc_t rt = (loc_t)::memmem(seg.map, seg.size, little, little_len)) {
                return rt-seg.map+seg.base;
            }
        }
        return 0;
    }(ropbytes,sizeof(ropbytes)-1,_segments);
}

loc_t offsetfinder64::find_rop_ldr_x0_x0_0x10(){
    constexpr char ropbytes[] = "\x00\x08\x40\xF9\xC0\x03\x5F\xD6";
    return [](const void *little, size_t little_len, vector<text_t>segments)->loc_t{
        for (auto seg : segments) {
            if (!seg.isExec)
                continue;
            
            if (loc_t rt = (loc_t)::memmem(seg.map, seg.size, little, little_len)) {
                return rt-seg.map+seg.base;
            }
        }
        return 0;
    }(ropbytes,sizeof(ropbytes)-1,_segments);
}

#pragma mark patch_finders
void slide_ptr(class patch *p,uint64_t slide){
    slide += *(uint64_t*)p->_patch;
    memcpy((void*)p->_patch, &slide, 8);
}

patch offsetfinder64::find_sandbox_patch(){
    loc_t str = findstr("process-exec denied while updating label",false);
    retassure(str, "Failed to find str");

    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");

    insn bdst(_segments, ref);
    for (int i=0; i<4; i++) {
        while (--bdst != insn::bl){
        }
    }
    --bdst;
    
    loc_t cbz = find_rel_branch_source(bdst, true);
    
    return patch(cbz, patch_nop, patch_nop_size);
}


patch offsetfinder64::find_amfi_substrate_patch(){
    loc_t str = findstr("AMFI: hook..execve() killing pid %u: %s",false);
    retassure(str, "Failed to find str");

    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");

    insn funcend(_segments, ref);
    while (++funcend != insn::ret);
    
    insn tbnz(funcend);
    while (--tbnz != insn::tbnz);
    
    constexpr char mypatch[] = "\x1F\x20\x03\xD5\x08\x79\x16\x12\x1F\x20\x03\xD5\x00\x00\x80\x52\xE9\x01\x80\x52";
    return {(loc_t)tbnz.pc(),mypatch,sizeof(mypatch)-1};
}

patch offsetfinder64::find_cs_enforcement_disable_amfi(){
    loc_t str = findstr("csflags",true);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");

    insn cbz(_segments, ref);
    while (--cbz != insn::cbz);
    
    insn movz(cbz);
    while (++movz != insn::movz);
    --movz;

    int anz = static_cast<int>((movz.pc()-cbz.pc())/4 +1);
    
    char mypatch[anz*4];
    for (int i=0; i<anz; i++) {
        ((uint32_t*)mypatch)[i] = *(uint32_t*)patch_nop;
    }
    
    return {(loc_t)cbz.pc(),mypatch,static_cast<size_t>(anz*4)};
}

patch offsetfinder64::find_i_can_has_debugger_patch_off(){
    loc_t str = findstr("Darwin Kernel",false);
    retassure(str, "Failed to find str");
    
    str -=4;
    
    return {str,"\x01",1};
}

patch offsetfinder64::find_amfi_patch_offsets(){
    loc_t str = findstr("int _validateCodeDirectoryHashInDaemon",false);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");

    insn bl_amfi_memcp(_segments, ref);

    loc_t memcmp = 0;
    
    loc_t jscpl = 0;
    while (1) {
        while (++bl_amfi_memcp != insn::bl);
        
        try {
            jscpl = jump_stub_call_ptr_loc(bl_amfi_memcp);
        } catch (tihmstar::bad_branch_destination &e) {
            continue;
        }
        if (haveSymbols()) {
            if (insn::deref(_segments, jscpl) == (uint64_t)(memcmp = find_sym("_memcmp")))
                break;
        }else{
            //check for _memcmp function signature
            insn checker(_segments, memcmp = (loc_t)insn::deref(_segments, jscpl));
            if (checker == insn::cbz
                && (++checker == insn::ldrb && checker.rn() == 0)
                && (++checker == insn::ldrb && checker.rn() == 1)
//                ++checker == insn::sub //i'm too lazy to implement this now, first 3 instructions should be good enough though.
                ) {
                break;
            }
        }
        
    }
    
    /* find*/
    //movz w0, #0x0
    //ret
    insn ret0(_segments, memcmp);
    for (;; --ret0) {
        if (ret0 == insn::movz && ret0.rd() == 0 && ret0.imm() == 0 && (ret0+1) == insn::ret) {
            break;
        }
    }
    
    uint64_t gadget = ret0.pc();
    return {jscpl,&gadget,sizeof(gadget),slide_ptr};
}

patch offsetfinder64::find_proc_enforce(){
    loc_t str = findstr("Enforce MAC policy on process operations", false);
    retassure(str, "Failed to find str");
    
    loc_t valref = memmem(&str, sizeof(str));
    retassure(valref, "Failed to find val ref");
    
    loc_t proc_enforce_ptr = valref - (5 * sizeof(uint64_t));
    
    loc_t proc_enforce_val_loc = (loc_t)insn::deref(_segments, proc_enforce_ptr);
    
    uint8_t mypatch = 1;
    return {proc_enforce_val_loc,&mypatch,1};
}

vector<patch> offsetfinder64::find_nosuid_off(){
    loc_t str = findstr("\"mount_common(): mount of %s filesystem failed with %d, but vnode list is not empty.\"", false);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");

    insn ldr(_segments,ref);
    
    while (--ldr != insn::ldr);
    
    loc_t cbnz = find_rel_branch_source(ldr, 1);
    
    insn bl_vfs_context_is64bit(ldr,cbnz);
    while (--bl_vfs_context_is64bit != insn::bl || bl_vfs_context_is64bit.imm() != (uint64_t)find_sym("_vfs_context_is64bit"));
    
    //patch1
    insn movk(bl_vfs_context_is64bit);
    while (--movk != insn::movk || movk.imm() != 8);
    
    //patch2
    insn orr(bl_vfs_context_is64bit);
    while (--orr != insn::orr || movk.imm() != 8);
    
    return {{(loc_t)movk.pc(),patch_nop,patch_nop_size},{(loc_t)orr.pc(),"\xE9\x03\x08\x2A",4}}; // mov w9, w8
}

patch offsetfinder64::find_remount_patch_offset(){
    loc_t off = find_syscall0();
    
    loc_t syscall_mac_mount = (off + 3*(424-1)*sizeof(uint64_t));

    loc_t __mac_mount = (loc_t)insn::deref(_segments, syscall_mac_mount);
    
    insn patchloc(_segments, __mac_mount);
    
    while (++patchloc != insn::tbz || patchloc.rt() != 8 || patchloc.other() != 6);
    
    --patchloc;
    
    constexpr char mypatch[] = "\xC8\x00\x80\x52"; //movz w8, #0x6
    return {(loc_t)patchloc.pc(),mypatch,sizeof(mypatch)-1};
}

patch offsetfinder64::find_lwvm_patch_offsets(){
    loc_t str = findstr("_mapForIO", false);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");
    
    insn functop(_segments,ref);
    
    while (--functop != insn::stp || (functop+1) != insn::stp || (functop+2) != insn::stp || (functop-2) != insn::ret);
    
    insn dstfunc(functop);
    loc_t destination = 0;
    while (1) {
        while (++dstfunc != insn::bl);
        
        try {
            destination = jump_stub_call_ptr_loc(dstfunc);
        } catch (tihmstar::bad_branch_destination &e) {
            continue;
        }
        
        if (haveSymbols()) {
            if (insn::deref(_segments, destination) == (uint64_t)find_sym("_PE_i_can_has_kernel_configuration"))
                break;
        }else{
            //check for _memcmp function signature
            insn checker(_segments, (loc_t)insn::deref(_segments, destination));
            uint8_t reg = 0;
            if ((checker == insn::adrp && (static_cast<void>(reg = checker.rd()),true))
                && (++checker == insn::add && checker.rd() == reg)
                && ++checker == insn::ldr
                && ++checker == insn::ret
                ) {
                break;
            }
        }
        
    }
    
    while (++dstfunc != insn::bcond || dstfunc.other() != insn::cond::NE);
    
    loc_t target = (loc_t)dstfunc.imm();
    
    return {destination,&target,sizeof(target),slide_ptr};
}

loc_t offsetfinder64::find_sbops(){
    loc_t str = findstr("Seatbelt sandbox policy", false);
    retassure(str, "Failed to find str");
    
    loc_t ref = memmem(&str, sizeof(str));
    retassure(ref, "Failed to find ref");
    
    return (loc_t)insn::deref(_segments, ref+0x18);
}

enum OFVariableType : uint32_t{
    kOFVariableTypeBoolean = 1,
    kOFVariableTypeNumber,
    kOFVariableTypeString,
    kOFVariableTypeData
} ;

enum OFVariablePerm : uint32_t{
    kOFVariablePermRootOnly = 0,
    kOFVariablePermUserRead,
    kOFVariablePermUserWrite,
    kOFVariablePermKernelOnly
};
struct OFVariable {
    const char *variableName;
    OFVariableType     variableType;
    OFVariablePerm     variablePerm;
    uint32_t           _padding;
    uint32_t           variableOffset;
};


patch offsetfinder64::find_nonceEnabler_patch(){
    if (!haveSymbols()){
        info("Falling back to find_nonceEnabler_patch_nosym, because we don't have symbols");
        return find_nonceEnabler_patch_nosym();
    }
    
    loc_t str = findstr("com.apple.System.boot-nonce",true);
    retassure(str, "Failed to find str");
    
    loc_t sym = find_sym("_gOFVariables");
    
    insn ptr(_segments,sym, insn::kText_and_Data);
    
#warning TODO: doublecast works, but is still kinda ugly
    OFVariable *varp = (OFVariable*)(void*)ptr;
    OFVariable nullvar = {0};
    for (OFVariable *vars = varp;memcmp(vars, &nullvar, sizeof(OFVariable)) != 0; vars++) {
        
        if ((loc_t)vars->variableName == str) {
            uint8_t mypatch = (uint8_t)kOFVariablePermUserWrite;
            loc_t location =  sym + ((uint8_t*)&vars->variablePerm - (uint8_t*)varp);
            return {location,&mypatch,1};
        }
    }
    
    reterror("failed to find \"com.apple.System.boot-nonce\"");
    return {0,0,0};
}

patch offsetfinder64::find_nonceEnabler_patch_nosym(){
    loc_t str = findstr("com.apple.System.boot-nonce",true);
    retassure(str, "Failed to find str");
    
    loc_t valref = memmem(&str, sizeof(str));
    retassure(valref, "Failed to find val ref");
    
    loc_t str2 = findstr("com.apple.System.sep.art",true);
    retassure(str2, "Failed to find str2");
    
    loc_t valref2 = memmem(&str2, sizeof(str2));
    retassure(valref2, "Failed to find val ref2");
    
    auto diff = abs(valref - valref2);
    
    assure(diff % sizeof(OFVariable) == 0 && diff < 0x50); //simple sanity check
    
    insn ptr(_segments, valref, insn::kText_and_Data);

    OFVariable *vars = (OFVariable*)(void*)ptr;
    if ((loc_t)vars->variableName == str) {
        uint8_t mypatch = (uint8_t)kOFVariablePermUserWrite;
        loc_t location = valref + offsetof(OFVariable, variablePerm);
        return {location,&mypatch,1};
    }
    
    reterror("failed to find \"com.apple.System.boot-nonce\"");
    return {0,0,0};
}

#pragma mark KPP bypass
loc_t offsetfinder64::find_gPhysBase(){
    loc_t str = findstr("\"pmap_map_high_window_bd: area too large", false);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str);
    retassure(ref, "literal ref to str");
    
    insn tgtref(_segments, ref);

    loc_t gPhysBase = 0;
    
    while (++tgtref != insn::adrp);
    gPhysBase = (loc_t)tgtref.imm();
    
    while (++tgtref != insn::ldr);
    gPhysBase += tgtref.imm();
    
    return gPhysBase;
}

loc_t offsetfinder64::find_kernel_pmap(){
    return find_sym("_kernel_pmap");
}

loc_t offsetfinder64::find_kernel_pmap_nosym(){
    loc_t str = findstr("\"pmap_map_bd\"", true);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, str, 1);
    retassure(ref, "literal ref to str");
    
    insn btm(_segments,ref);
    while (++btm != insn::ret);
    
    insn kerne_pmap_ref(btm);
    while (--kerne_pmap_ref != insn::adrp);
    
    uint8_t reg = kerne_pmap_ref.rd();
    loc_t kernel_pmap = (loc_t)kerne_pmap_ref.imm();
    
    while (++kerne_pmap_ref != insn::ldr || kerne_pmap_ref.rn() != reg);
    assure(kerne_pmap_ref.pc()<btm.pc());
    
    kernel_pmap += kerne_pmap_ref.imm();
    
    return kernel_pmap;
}

loc_t offsetfinder64::find_cpacr_write(){
    return memmem("\x40\x10\x18\xD5", 4);
}

loc_t offsetfinder64::find_idlesleep_str_loc(){
    loc_t entryp = find_entry();
    
    insn finder(_segments,entryp);
    assure(finder == insn::b);
    
    insn deepsleepfinder(finder, (loc_t)finder.imm());
    while (--deepsleepfinder != insn::nop);
    
    loc_t fref = find_literal_ref(_segments, (loc_t)(deepsleepfinder.pc())+4+0xC);
    
    insn str(finder,fref);
    while (++str != insn::str);
    while (++str != insn::str);
    
    loc_t idlesleep_str_loc = (loc_t)str.imm();
    int rn = str.rn();
    while (--str != insn::adrp || str.rd() != rn);
    idlesleep_str_loc += str.imm();
    
    return idlesleep_str_loc;
}

loc_t offsetfinder64::find_deepsleep_str_loc(){
    loc_t entryp = find_entry();
    
    insn finder(_segments,entryp);
    assure(finder == insn::b);
    
    insn deepsleepfinder(finder, (loc_t)finder.imm());
    while (--deepsleepfinder != insn::nop);
    
    loc_t fref = find_literal_ref(_segments, (loc_t)(deepsleepfinder.pc())+4+0xC);
    
    insn str(finder,fref);
    while (++str != insn::str);
    
    loc_t idlesleep_str_loc = (loc_t)str.imm();
    int rn = str.rn();
    while (--str != insn::adrp || str.rd() != rn);
    idlesleep_str_loc += str.imm();
    
    return idlesleep_str_loc;
}

loc_t offsetfinder64::find_rootvnode() {
    return find_sym("_rootvnode");
}


offsetfinder64::~offsetfinder64(){
    if (_freeKernel) safeFree(_kdata);
}










//
