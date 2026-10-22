/* Explicit, user-authorized bypass of A14's software-only 32-bit Map limit.
 * This is NOT a successful UEFI DMA mapping and does NOT patch firmware.
 * CPU addresses are physical during x64 boot services (UEFI 2.10 2.3.4).
 * GPU identity addressing is a board-specific experimental assumption, gated
 * by absent IOMMU protocol, disabled VT-d translation AND disabled PMRs.
 * No chipset/IOMMU/Setup writes; no global protocol replacement; no fallback.
 */
#ifndef TU102_DIRECT_DMA_H
#define TU102_DIRECT_DMA_H
#include <cpuid.h>

/* All 91 bytes of A14 PciRootBridge.Map, RVA2a98..2af2. Relocation-free.
 * Original PE SHA256 eac8ed469fcb4056ec4197bf5506dba2bc163b83866c314bf6570726cf1408ca. */
static const UINT8 dd_a14_map[]={
  0x4d,0x8b,0xd8,0x4d,0x85,0xc0,0x74,0x48,0x4d,0x85,0xc9,0x74,
  0x43,0x4c,0x8b,0x54,0x24,0x28,0x4d,0x85,0xd2,0x74,0x39,0x4c,
  0x8b,0x44,0x24,0x30,0x4d,0x85,0xc0,0x74,0x2f,0x83,0xfa,0x06,
  0x73,0x2a,0x49,0x83,0x20,0x00,0x49,0x8b,0x01,0x49,0x8d,0x0c,
  0x03,0xb8,0xff,0xff,0xff,0xff,0x48,0x3b,0xc8,0x76,0x0f,0x49,
  0x83,0x20,0x00,0x48,0xb8,0x03,0x00,0x00,0x00,0x00,0x00,0x00,
  0x80,0xc3,0x4d,0x89,0x1a,0x33,0xc0,0xc3,0x48,0xb8,0x02,0x00,
  0x00,0x00,0x00,0x00,0x00,0x80,0xc3
};
#ifndef TX_READONLY_DIAGNOSTIC
static EFI_GUID dd_setup_guid={0xec87d643,0xeba4,0x4bb5,{0xa1,0xe5,0x3f,0x3e,0x36,0xb2,0x0d,0xa9}};
#endif

static BOOLEAN dd_cpu_profile(UINT32 b,UINT32 c,UINT32 d,UINT32 sig,UINT32 flags,UINT32 bits) {
    return b==0x756e6547 && c==0x6c65746e && d==0x49656e69 &&
           sig==0x306f2 && !(flags&0x80000000u) && bits==46;
}
static EFI_STATUS dd_cpu(void) {
    UINT32 a,b,c,d,sig,flags,bits;
    if(!__get_cpuid(0,&a,&b,&c,&d)) return EFI_UNSUPPORTED;
    UINT32 vb=b,vc=c,vd=d;
    if(!__get_cpuid(1,&sig,&b,&flags,&d) ||
       !__get_cpuid(0x80000008,&bits,&b,&c,&d)) return EFI_UNSUPPORTED;
    bits&=255;
    note(L"Direct DMA CPU signature=%08x physbits=%d hypervisor=%d\r\n",sig,bits,flags>>31);
    return dd_cpu_profile(vb,vc,vd,sig,flags,bits)?EFI_SUCCESS:EFI_UNSUPPORTED;
}

#ifndef TX_READONLY_DIAGNOSTIC
static EFI_STATUS dd_setup(void) {
    UINT8 data[0x2e1]; UINTN bytes=sizeof(data); UINT32 attrs=0;
    EFI_STATUS s=uefi_call_wrapper(RT->GetVariable,5,L"Setup",&dd_setup_guid,&attrs,&bytes,data);
    note(L"Setup GET only: status=%r size=%lx attrs=%x\r\n",s,(UINT64)bytes,attrs);
    if(EFI_ERROR(s)) return s;
    if(bytes!=sizeof(data) || (attrs&3)!=3 || (attrs&~7u)) return EFI_UNSUPPORTED;
    note(L"Setup Above4G[24D]=%d CSM[277]=%d; no variable writes\r\n",data[0x24d],data[0x277]);
#ifdef TX_EXPERIMENT_V4
    /* This profile does not relocate BARs or use a high MMIO aperture.
     * Above4G controls resource placement, not proof of device DMA reachability.
     * A14's live Map implementation and all translation/protection/owned-RAM
     * checks remain mandatory. This explicitly departs from upstream's
     * Above4G-on recipe; successful high DMA is still NOT established. */
    note(L"Above4G is NOT a DMA permission check; v4 accepts 0/1 without changing it.\r\n");
    return data[0x24d]<=1 && data[0x277]<=1?EFI_SUCCESS:EFI_UNSUPPORTED;
#else
    return data[0x24d]==1 && data[0x277]<=1?EFI_SUCCESS:EFI_UNSUPPORTED;
#endif
}

#endif

/* Validate code before reading it and owned allocation before CPU preparation.
 * GetMemoryMap is used only for bounds/type/attributes, not as a DMA API. */
static EFI_STATUS dd_memory(HighDma *h) {
    UINTN size=0,key=0,stride=0,capacity=0; UINT32 version=0;
    EFI_MEMORY_DESCRIPTOR *map=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->GetMemoryMap,5,&size,NULL,&key,&stride,&version);
    if(s!=EFI_BUFFER_TOO_SMALL) return EFI_ERROR(s)?s:EFI_COMPROMISED_DATA;
    for(UINTN retry=0;retry<3;retry++) {
        if(stride<sizeof(*map) || stride>4096 || size>0x100000) return EFI_COMPROMISED_DATA;
        capacity=size+16*stride; map=AllocatePool(capacity);
        if(!map) return EFI_OUT_OF_RESOURCES;
        size=capacity;
        s=uefi_call_wrapper(BS->GetMemoryMap,5,&size,map,&key,&stride,&version);
        if(s!=EFI_BUFFER_TOO_SMALL) break;
        FreePool(map);map=NULL;
    }
    if(EFI_ERROR(s)) { if(map)FreePool(map);return s; }
    if(!map || size>capacity || stride<sizeof(*map) || stride>4096 || size%stride || version!=1) {
        if(map)FreePool(map);
        return EFI_COMPROMISED_DATA;
    }
    UINT64 code=(UINTN)h->probe.root->Map;
    BOOLEAN code_ok=FALSE,ram_ok=!h->allocated;
#ifdef TX_READONLY_DIAGNOSTIC
    UINT64 largest_start=0,largest_end=0;
#endif
    if(!code || code>~(UINT64)0-sizeof(dd_a14_map) ||
       (h->allocated && (h->host<0x100000000ULL || (h->host&4095) ||
                        h->host>0x10000000000ULL-TX_POOL_SIZE))) {
        FreePool(map);return EFI_UNSUPPORTED;
    }
    for(UINTN off=0;off<size;off+=stride) {
        EFI_MEMORY_DESCRIPTOR d; CopyMem(&d,(UINT8 *)map+off,sizeof(d));
        if((d.PhysicalStart&4095) || !d.NumberOfPages ||
           d.NumberOfPages>((~(UINT64)0-d.PhysicalStart)>>12)) continue;
        UINT64 end=d.PhysicalStart+(d.NumberOfPages<<12);
#ifdef TX_READONLY_DIAGNOSTIC
        if(d.Type==EfiConventionalMemory && (d.Attribute&EFI_MEMORY_WB) &&
           !(d.Attribute&(EFI_MEMORY_WP|EFI_MEMORY_RP|0x20000ULL))) {
            UINT64 start=d.PhysicalStart<0x100000000ULL?0x100000000ULL:d.PhysicalStart;
            UINT64 ceiling=end>0x10000000000ULL?0x10000000000ULL:end;
            if(ceiling>start && ceiling-start>largest_end-largest_start) {
                largest_start=start;largest_end=ceiling;
            }
        }
#endif
        if(d.Type==EfiBootServicesCode && !(d.Attribute&EFI_MEMORY_RP) &&
           code>=d.PhysicalStart && code+sizeof(dd_a14_map)<=end) code_ok=TRUE;
        if(h->allocated && d.Type==EfiBootServicesData && (d.Attribute&EFI_MEMORY_WB) &&
           !(d.Attribute&(EFI_MEMORY_WP|EFI_MEMORY_RP|0x20000ULL)) &&
           h->host>=d.PhysicalStart && h->host+TX_POOL_SIZE<=end) ram_ok=TRUE;
    }
    FreePool(map);
#ifdef TX_READONLY_DIAGNOSTIC
    note(L"Largest eligible high WB RAM span=%lx..%lx bytes=%lx; NO allocation or DMA test.\r\n",
         largest_start,largest_end,largest_end-largest_start);
#endif
    if(!code_ok || !ram_ok) {
        note(L"Direct memory descriptor refusal: code=%d owned_RAM=%d address=%lx\r\n",code_ok,ram_ok,code);
        return EFI_UNSUPPORTED;
    }
#ifdef TX_READONLY_DIAGNOSTIC
    CHAR16 hex[183];const CHAR16 digits[]=L"0123456789ABCDEF";
    const UINT8 *live=(const UINT8 *)(UINTN)code;
    for(UINTN i=0;i<sizeof(dd_a14_map);i++) {
        hex[2*i]=digits[live[i]>>4];hex[2*i+1]=digits[live[i]&15];
    }
    hex[182]=0;
    note(L"Live root Map address=%lx first91bytes=%s (READ ONLY; NOT called)\r\n",code,hex);
    if(EFI_ERROR(log_status)) return log_status;
#endif
    if(CompareMem((VOID *)(UINTN)code,(VOID *)dd_a14_map,sizeof(dd_a14_map))) {
        note(L"Live root Map code differs from audited A14; refusing bypass.\r\n");
        return EFI_SECURITY_VIOLATION;
    }
    note(L"Live root Map matches audited A14 (91 bytes); code is NOT patched. RAM_owned=%d\r\n",h->allocated);
    return EFI_ERROR(log_status)?log_status:EFI_SUCCESS;
}

static EFI_STATUS dd_bridge(HighDma *h) {
    const UINT32 offsets[]={0,4,8,0x18}; UINT32 values[4]={0};
    for(UINTN i=0;i<4;i++) {
        EFI_STATUS s=uefi_call_wrapper(h->probe.root->Pci.Read,5,h->probe.root,
             EfiPciIoWidthUint32,(UINT64)(0x00020000+offsets[i]),1,&values[i]);
        if(EFI_ERROR(s)) return s;
    }
    note(L"Parent port00:02.0 id=%08x command=%04x class=%08x buses=%08x (READ ONLY)\r\n",
         values[0],values[1]&0xffff,values[2],values[3]);
    if(values[0]!=0x2f048086 || (values[1]&6)!=6 ||
       (values[2]&0xffffff00)!=0x06040000 || (values[3]&0xffffff)!=0x020200)
        return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}

/* Linux v6.14 intel/iommu.h: GSTS.TES bit31, CAP.PLMR/PHMR bits5/6,
 * PMEN at64 with EPM bit31 and PRS bit0. Even pending protection refuses.
 * No disabling registers, page-table edits or device permission grants. */
static EFI_STATUS dd_protection(HighDma *h) {
    if(EFI_ERROR(log_status)) return log_status;
    VOID *iommu=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->LocateProtocol,3,&iommu_guid,NULL,&iommu);
    if(s!=EFI_NOT_FOUND || iommu) return EFI_UNSUPPORTED;
    s=dp_dmar(&h->probe); if(EFI_ERROR(s)) return s;
    if(h->probe.dmar_units!=2 || h->probe.dmar_translation_enabled) return EFI_UNSUPPORTED;
    const UINT64 bases[]={0xfbffc000ULL,0xfbffd000ULL};
    for(UINTN i=0;i<2;i++) {
        UINT32 cap=0,pmen=0;
        s=uefi_call_wrapper(h->probe.root->Mem.Read,5,h->probe.root,EfiPciIoWidthUint32,bases[i]+8,1,&cap);
        if(EFI_ERROR(s)) return s;
        if(!cap || cap==0xffffffff) return EFI_DEVICE_ERROR;
        if(cap&0x60) {
            s=uefi_call_wrapper(h->probe.root->Mem.Read,5,h->probe.root,EfiPciIoWidthUint32,bases[i]+0x64,1,&pmen);
            if(EFI_ERROR(s)) return s;
        }
        note(L"Direct VT-d protection base=%lx CAPlo=%08x PMEN=%08x supported=%d (READ ONLY)\r\n",
             bases[i],cap,pmen,(cap&0x60)!=0);
        if(pmen&0x80000001u) return EFI_ACCESS_DENIED;
    }
    s=dd_bridge(h); if(EFI_ERROR(s)) return s;
    return EFI_ERROR(log_status)?log_status:EFI_SUCCESS;
}

#ifndef TX_READONLY_DIAGNOSTIC
static EFI_STATUS dd_guard(HighDma *h) {
    EFI_STATUS s=hd_guard(h); h->guarded=FALSE;
    if(EFI_ERROR(s)) return s;
    s=dd_cpu(); if(EFI_ERROR(s)) return s;
    s=dd_setup(); if(EFI_ERROR(s)) return s;
    s=dd_memory(h); if(EFI_ERROR(s)) return s;
    s=dd_protection(h); if(EFI_ERROR(s)) return s;
    h->guarded=TRUE;
    return EFI_SUCCESS;
}
static EFI_STATUS dd_begin(HighDma *h,unsigned phase) {
    if(!h->guarded || !h->allocated || h->mapped || h->mapping ||
       (phase!=1 && phase!=2) || h->phase!=phase-1 ||
       h->host<0x100000000ULL || (h->host&4095) ||
       h->host>0x10000000000ULL-TX_POOL_SIZE) return EFI_INVALID_PARAMETER;
    EFI_STATUS s=dd_protection(h); if(EFI_ERROR(s)) return s;
    note(L"DIRECT PHYSICAL DMA phase=%d host/device=%lx bytes=%x; UEFI Map/Unmap deliberately NOT used.\r\n",
         phase,h->host,TX_POOL_SIZE);
    if(EFI_ERROR(log_status)) return log_status;
    h->phase=phase;
    return EFI_SUCCESS;
}
#endif /* !TX_READONLY_DIAGNOSTIC */
#endif
