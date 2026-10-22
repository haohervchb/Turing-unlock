/* Immutable streaming inputs, NOT common buffers. UEFI 2.10 14.2.11.
 * CPU prepares bytes before Map(Read64); each firmware stage has a distinct
 * mapping lifetime. No identity mapping is assumed: a different returned
 * device address is rejected before that stage runs. No IOMMU disabling.
 */
#ifndef TU102_HIGH_DMA_H
#define TU102_HIGH_DMA_H
#include "tu102_dma_services.h"

typedef struct {
    DmaProbe probe;
    EFI_PHYSICAL_ADDRESS host;
    BOOLEAN allocated,mapped,guarded;
    VOID *mapping;
    unsigned phase;
} HighDma;

#ifndef TX_READONLY_DIAGNOSTIC
static EFI_STATUS hd_guard(HighDma *h) {
    EFI_STATUS s=dp_target(&h->probe); if(EFI_ERROR(s)) return s;
    s=dp_root(&h->probe); if(EFI_ERROR(s)) return s;
    VOID *iommu=NULL;
    s=uefi_call_wrapper(BS->LocateProtocol,3,&iommu_guid,NULL,&iommu);
    note(L"High DMA: IOMMU protocol=%r present=%d\r\n",s,iommu!=NULL);
    /* Root Map alone doesn't grant device-specific IOMMU permissions. */
    if(s!=EFI_NOT_FOUND || iommu) return EFI_UNSUPPORTED;
    s=dp_dmar(&h->probe); if(EFI_ERROR(s)) return s;
    if(h->probe.dmar_units!=2 || h->probe.dmar_translation_enabled) return EFI_UNSUPPORTED;
    h->guarded=TRUE;
    return EFI_ERROR(log_status)?log_status:EFI_SUCCESS;
}

static EFI_STATUS hd_allocate(HighDma *h) {
    if(h->allocated || h->mapped) return EFI_ALREADY_STARTED;
    if(EFI_ERROR(log_status)) return log_status;
    UINTN size=0,key=0,stride=0,capacity=0; UINT32 version=0;
    EFI_MEMORY_DESCRIPTOR *map=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->GetMemoryMap,5,&size,NULL,&key,&stride,&version);
    if(s!=EFI_BUFFER_TOO_SMALL) return EFI_ERROR(s)?s:EFI_COMPROMISED_DATA;
    for(UINTN retry=0;retry<3;retry++) {
        if(stride<sizeof(*map) || stride>4096 || size>0x100000) return EFI_COMPROMISED_DATA;
        capacity=size+16*stride;
        map=AllocatePool(capacity); if(!map) return EFI_OUT_OF_RESOURCES;
        size=capacity;
        s=uefi_call_wrapper(BS->GetMemoryMap,5,&size,map,&key,&stride,&version);
        if(s!=EFI_BUFFER_TOO_SMALL) break;
        FreePool(map); map=NULL;
    }
    if(EFI_ERROR(s)) { if(map) FreePool(map); return s; }
    if(!map || size>capacity || stride<sizeof(*map) || stride>4096 || size%stride
#ifdef TX_EXPERIMENT_V4
       || version!=1
#endif
       ) {
        if(map) FreePool(map);
        return EFI_COMPROMISED_DATA;
    }
    UINT64 limit=0x10000000000ULL; /* exclusive 40-bit ceiling */
    s=EFI_OUT_OF_RESOURCES;
    for(UINTN attempt=0;attempt<16;attempt++) {
        UINT64 best=0;
        for(UINTN off=0;off<size;off+=stride) {
            EFI_MEMORY_DESCRIPTOR d; CopyMem(&d,(UINT8 *)map+off,sizeof(d));
            if(d.Type!=EfiConventionalMemory || !(d.Attribute&EFI_MEMORY_WB) ||
               (d.Attribute&(EFI_MEMORY_WP|EFI_MEMORY_RP
#ifdef TX_EXPERIMENT_V4
                             |0x20000ULL /* EFI_MEMORY_RO */
#endif
                             )) || (d.PhysicalStart&4095) ||
               !d.NumberOfPages || d.NumberOfPages>((~(UINT64)0-d.PhysicalStart)>>12)) continue;
            UINT64 end=d.PhysicalStart+(d.NumberOfPages<<12);
            if(end>limit) end=limit;
            UINT64 start=d.PhysicalStart<0x100000000ULL?0x100000000ULL:d.PhysicalStart;
            if(end<start || end-start<TX_POOL_SIZE) continue;
            UINT64 candidate=(end-TX_POOL_SIZE)&~4095ULL;
            if(candidate>=start && candidate>best) best=candidate;
        }
        if(!best) break;
        EFI_PHYSICAL_ADDRESS requested=best;
        note(L"High AllocateAddress 32MiB requested=%lx\r\n",best);
        if(EFI_ERROR(log_status)) { s=log_status; break; }
        s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAddress,EfiBootServicesData,
                           (UINTN)(TX_POOL_SIZE>>12),&requested);
        note(L"High AllocateAddress status=%r returned=%lx\r\n",s,requested);
        if(!EFI_ERROR(s)) {
            h->host=requested; h->allocated=TRUE;
            if(requested!=best) s=EFI_COMPROMISED_DATA;
            else if(EFI_ERROR(log_status)) s=log_status;
            break;
        }
        limit=best;
    }
    FreePool(map);
    return s;
}

static inline EFI_STATUS hd_map(HighDma *h,unsigned phase) {
    if(!h->allocated || h->mapped || (phase!=1 && phase!=2) ||
       (phase==1 && h->phase!=0) || (phase==2 && h->phase!=1) ||
       h->host<0x100000000ULL || (h->host&4095) ||
       h->host>0x10000000000ULL-TX_POOL_SIZE) return EFI_INVALID_PARAMETER;
    if(EFI_ERROR(log_status)) return log_status;
    UINTN off=phase==1?TX_FWSEC_OFF:TX_SIGNATURE_OFF;
    UINTN expected=phase==1?TX_FWSEC_SIZE:TX_POOL_SIZE-TX_SIGNATURE_OFF;
    UINTN bytes=expected; EFI_PHYSICAL_ADDRESS device=0;
    note(L"BEGIN Read64 map phase=%d host=%lx bytes=%lx; input already prepared\r\n",phase,h->host+off,(UINT64)bytes);
    if(EFI_ERROR(log_status)) return log_status;
    EFI_STATUS s=uefi_call_wrapper(h->probe.root->Map,6,h->probe.root,EfiPciOperationBusMasterRead64,
                     (VOID *)(UINTN)(h->host+off),&bytes,&device,&h->mapping);
    if(!EFI_ERROR(s)) { h->mapped=TRUE; h->phase=phase; }
    note(L"Read64 map phase=%d status=%r device=%lx bytes=%lx token=%lx\r\n",
         phase,s,device,(UINT64)bytes,(UINT64)(UINTN)h->mapping);
    if(EFI_ERROR(s)) return s;
    if(bytes!=expected || device!=h->host+off) return EFI_UNSUPPORTED;
    return EFI_ERROR(log_status)?log_status:EFI_SUCCESS;
}

static EFI_STATUS hd_unmap(HighDma *h) {
    if(!h->mapped) return EFI_SUCCESS;
    EFI_STATUS s=uefi_call_wrapper(h->probe.root->Unmap,2,h->probe.root,h->mapping);
    note(L"Read64 Unmap phase=%d status=%r\r\n",h->phase,s);
    if(!EFI_ERROR(s)) { h->mapped=FALSE; h->mapping=NULL; }
    return s;
}

static EFI_STATUS hd_release(HighDma *h) {
    EFI_STATUS s=hd_unmap(h); if(EFI_ERROR(s)) return s;
    if(h->allocated) {
        s=uefi_call_wrapper(BS->FreePages,2,h->host,(UINTN)(TX_POOL_SIZE>>12));
        note(L"High FreePages status=%r\r\n",s);
        if(EFI_ERROR(s)) return s;
        h->allocated=FALSE; h->host=0;
    }
    return EFI_SUCCESS;
}
#endif /* !TX_READONLY_DIAGNOSTIC */
#endif
