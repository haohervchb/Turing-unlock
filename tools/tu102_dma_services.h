#ifndef TU102_DMA_SERVICES_H
#define TU102_DMA_SERVICES_H
/* Shared exact-board PCI/root/ACPI diagnostics; requires preflight logging helpers. */
#define PROBE_BYTES (32u*1024u*1024u)
static EFI_GUID root_guid=EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
static EFI_GUID iommu_guid={0x4e939de9,0xd948,0x4b0f,{0x88,0xed,0xe6,0xe1,0xce,0x51,0x7c,0x1e}};
static EFI_GUID acpi_guid=ACPI_20_TABLE_GUID;

static inline UINT16 dp16(const UINT8 *p) { return p[0]|((UINT16)p[1]<<8); }
static inline UINT32 dp32(const UINT8 *p) { UINT32 v; CopyMem(&v,(VOID *)p,4); return v; }
static inline UINT64 dp64(const UINT8 *p) { UINT64 v; CopyMem(&v,(VOID *)p,8); return v; }
static inline BOOLEAN dp_sum(const UINT8 *p,UINTN n) {
    UINT8 sum=0; for(UINTN i=0;i<n;i++) sum+=p[i]; return sum==0;
}

typedef struct {
    EFI_HANDLE device;
    EFI_PCI_IO_PROTOCOL *pci;
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root;
    VOID *pool,*mapping;
    BOOLEAN mapped;
    UINT64 pci_attributes,root_attributes;
    UINT16 command;
    UINTN dmar_units;
    BOOLEAN dmar_translation_enabled;
} DmaProbe;

static inline EFI_STATUS dp_target(DmaProbe *p) {
    EFI_HANDLE *handles=NULL; UINTN count=0,matches=0;
    EFI_STATUS s=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&pci_guid,NULL,&count,&handles);
    if(EFI_ERROR(s)) return s;
    for(UINTN i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *pci=NULL; UINTN seg,bus,dev,fn;
        s=uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&pci_guid,(VOID **)&pci);
        if(EFI_ERROR(s) || !pci) continue;
        s=uefi_call_wrapper(pci->GetLocation,5,pci,&seg,&bus,&dev,&fn);
        if(!EFI_ERROR(s) && seg==0 && bus==2 && dev==0 && fn==0) {
            p->pci=pci; p->device=handles[i]; matches++;
        }
    }
    FreePool(handles);
    if(matches!=1) { note(L"Exact target absent/ambiguous; no GPU writes.\r\n"); return EFI_NOT_FOUND; }
    UINT32 id=0,sub=0,bar=0,command=0;
    if(EFI_ERROR(read_config(p->pci,0,&id)) || EFI_ERROR(read_config(p->pci,0x2c,&sub)) ||
       EFI_ERROR(read_config(p->pci,0x10,&bar)) || EFI_ERROR(read_config(p->pci,4,&command))) return EFI_DEVICE_ERROR;
    note(L"Target 02:00.0 id=%08x sub=%08x BAR0=%08x command=%04x\r\n",id,sub,bar,command&0xffff);
    if(id!=0x1e0710de || sub!=0x866a1043 || bar!=0xfa000000 || (command&0xffff)!=7) return EFI_UNSUPPORTED;
    p->command=(UINT16)command;
    UINT64 supported=0;
    s=uefi_call_wrapper(p->pci->Attributes,4,p->pci,EfiPciIoAttributeOperationSupported,0,&supported);
    if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(p->pci->Attributes,4,p->pci,EfiPciIoAttributeOperationGet,0,&p->pci_attributes);
    note(L"PCI attrs status=%r current=%lx supported=%lx; no attribute changes.\r\n",s,p->pci_attributes,supported);
    return s;
}

static inline EFI_STATUS dp_root(DmaProbe *p) {
    EFI_DEVICE_PATH *path=NULL; EFI_HANDLE handle=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->HandleProtocol,3,p->device,&path_guid,(VOID **)&path);
    if(EFI_ERROR(s) || !path) return EFI_NOT_FOUND;
    EFI_DEVICE_PATH *remaining=path;
    s=uefi_call_wrapper(BS->LocateDevicePath,3,&root_guid,&remaining,&handle);
    if(EFI_ERROR(s) || !handle || !remaining || remaining==path) return EFI_NOT_FOUND;
    s=uefi_call_wrapper(BS->HandleProtocol,3,handle,&root_guid,(VOID **)&p->root);
    if(EFI_ERROR(s) || !p->root) return EFI_NOT_FOUND;
    note(L"Parent root-bridge resolved from GPU device path, segment=%d\r\n",p->root->SegmentNumber);
    if(p->root->SegmentNumber!=0) return EFI_UNSUPPORTED;
    const UINT32 offsets[]={0,0x2c,0x10,4};
    const UINT32 expected[]={0x1e0710de,0x866a1043,0xfa000000,7};
    for(UINTN i=0;i<4;i++) {
        UINT32 v=0;
        /* UEFI root-bridge PCI address: bus<<24, device<<16, function<<8. */
        s=uefi_call_wrapper(p->root->Pci.Read,5,p->root,EfiPciIoWidthUint32,
                            (UINT64)(0x02000000u+offsets[i]),1,&v);
        note(L"Root config offset=%02x value=%08x status=%r\r\n",offsets[i],v,s);
        if(EFI_ERROR(s)) return s;
        if((i==3?(v&0xffff):v)!=expected[i]) return EFI_SECURITY_VIOLATION;
    }
    UINT64 supported=0;
    s=uefi_call_wrapper(p->root->GetAttributes,3,p->root,&supported,&p->root_attributes);
    note(L"Root attrs status=%r current=%lx supported=%lx\r\n",s,p->root_attributes,supported);
    return s;
}

/* Parse/check the entire DMAR before touching either known, read-only VT-d
 * status register. Reject different hardware addresses and duplicate units.
 * Bases were observed in this board's Linux ACPI table/kernel log. */
static inline EFI_STATUS dp_dmar_table(DmaProbe *p,const UINT8 *table,UINTN capacity) {
    p->dmar_units=0; p->dmar_translation_enabled=FALSE;
    if(!table || capacity<48 || capacity>65536 || CompareMem((VOID *)table,"DMAR",4)) return EFI_COMPROMISED_DATA;
    UINT32 length=dp32(table+4);
    if(length<48 || length>capacity || !dp_sum(table,length) ||
       CompareMem((VOID *)(table+10),"ALWARE",6) || CompareMem((VOID *)(table+16),"ALIENWRE",8)) return EFI_COMPROMISED_DATA;
    UINT64 bases[2]={0}; UINTN count=0;
    for(UINTN off=48;off<length;) {
        if(length-off<4) return EFI_COMPROMISED_DATA;
        UINT16 type=dp16(table+off),n=dp16(table+off+2);
        if(n<4 || n>length-off) return EFI_COMPROMISED_DATA;
        if(type==0) {
            if(n<16 || count>=2 || dp16(table+off+6)!=0) return EFI_UNSUPPORTED;
            UINT64 base=dp64(table+off+8);
            if((base!=0xfbffc000ULL && base!=0xfbffd000ULL) || (count && bases[0]==base)) return EFI_UNSUPPORTED;
            bases[count++]=base;
        }
        off+=n;
    }
    if(count!=2) return EFI_UNSUPPORTED;
    for(UINTN i=0;i<count;i++) {
        UINT32 version=0,status=0;
        if(EFI_ERROR(log_status)) return log_status;
        EFI_STATUS s=uefi_call_wrapper(p->root->Mem.Read,5,p->root,EfiPciIoWidthUint32,bases[i],1,&version);
        if(EFI_ERROR(s) || version!=0x10) return EFI_DEVICE_ERROR;
        s=uefi_call_wrapper(p->root->Mem.Read,5,p->root,EfiPciIoWidthUint32,bases[i]+0x1c,1,&status);
        note(L"VT-d base=%lx version=%08x GSTS=%08x translation_enabled=%d status=%r (READ ONLY)\r\n",
             bases[i],version,status,(status>>31)&1,s);
        if(EFI_ERROR(s) || status==0xffffffff) return EFI_DEVICE_ERROR;
        p->dmar_units++;
        if(status&0x80000000u) p->dmar_translation_enabled=TRUE;
    }
    return EFI_SUCCESS;
}

static inline EFI_STATUS dp_dmar(DmaProbe *p) {
    UINT8 *rsdp=NULL;
    for(UINTN i=0;i<ST->NumberOfTableEntries;i++) {
        if(!CompareMem(&ST->ConfigurationTable[i].VendorGuid,&acpi_guid,sizeof(acpi_guid))) {
            if(rsdp) return EFI_COMPROMISED_DATA;
            rsdp=ST->ConfigurationTable[i].VendorTable;
        }
    }
    if(!rsdp) return EFI_NOT_FOUND;
    if(CompareMem(rsdp,"RSD PTR ",8) || !dp_sum(rsdp,20) || rsdp[15]<2) return EFI_COMPROMISED_DATA;
    UINT32 length=dp32(rsdp+20);
    if(length<36 || length>4096 || !dp_sum(rsdp,length)) return EFI_COMPROMISED_DATA;
    UINT8 *xsdt=(VOID *)(UINTN)dp64(rsdp+24);
    if(!xsdt || CompareMem(xsdt,"XSDT",4)) return EFI_COMPROMISED_DATA;
    length=dp32(xsdt+4);
    if(length<36 || length>65536 || (length-36)%8 || !dp_sum(xsdt,length)) return EFI_COMPROMISED_DATA;
    UINT8 *dmar=NULL;
    for(UINTN off=36;off<length;off+=8) {
        UINT8 *table=(VOID *)(UINTN)dp64(xsdt+off);
        if(table && !CompareMem(table,"DMAR",4)) {
            if(dmar) return EFI_COMPROMISED_DATA;
            dmar=table;
        }
    }
    if(!dmar) return EFI_NOT_FOUND;
    return dp_dmar_table(p,dmar,dp32(dmar+4));
}

#ifndef TX_READONLY_DIAGNOSTIC
static inline EFI_STATUS dp_release(DmaProbe *p) {
    if(p->mapped) {
        EFI_STATUS s=uefi_call_wrapper(p->root->Unmap,2,p->root,p->mapping);
        note(L"Root Unmap status=%r\r\n",s);
        if(EFI_ERROR(s)) return s; /* retain backing if unmap failed */
        p->mapped=FALSE;
    }
    if(p->pool) {
        EFI_STATUS s=uefi_call_wrapper(p->root->FreeBuffer,3,p->root,(UINTN)(PROBE_BYTES>>12),p->pool);
        note(L"Root FreeBuffer status=%r\r\n",s);
        if(EFI_ERROR(s)) return s;
        p->pool=NULL;
    }
    return EFI_SUCCESS;
}

static inline EFI_STATUS dp_map(DmaProbe *p) {
    if(EFI_ERROR(log_status)) return log_status;
    note(L"BEGIN root AllocateBuffer: 32MiB, DAC allocation flag; no root/device attribute enable.\r\n");
    if(EFI_ERROR(log_status)) return log_status;
    EFI_STATUS s=uefi_call_wrapper(p->root->AllocateBuffer,6,p->root,AllocateAnyPages,
                 EfiBootServicesData,(UINTN)(PROBE_BYTES>>12),&p->pool,EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE);
    note(L"Root AllocateBuffer status=%r host=%lx\r\n",s,(UINT64)(UINTN)p->pool);
    if(EFI_ERROR(s)) { p->pool=NULL; return s; }
    if(!p->pool || ((UINTN)p->pool&4095)) return EFI_COMPROMISED_DATA;
    if(EFI_ERROR(log_status)) return log_status;
    UINTN bytes=PROBE_BYTES; EFI_PHYSICAL_ADDRESS device=0;
    s=uefi_call_wrapper(p->root->Map,6,p->root,EfiPciOperationBusMasterCommonBuffer64,
                        p->pool,&bytes,&device,&p->mapping);
    if(!EFI_ERROR(s)) p->mapped=TRUE;
    note(L"Root CommonBuffer64 status=%r host=%lx device=%lx bytes=%lx mapping=%lx\r\n",
         s,(UINT64)(UINTN)p->pool,device,(UINT64)bytes,(UINT64)(UINTN)p->mapping);
    if(EFI_ERROR(s)) return s;
    BOOLEAN suitable=bytes==PROBE_BYTES && device>=0x100000000ULL && !(device&4095) &&
                     device<=0x10000000000ULL-PROBE_BYTES;
    note(L"ADDRESS_GEOMETRY_SUITABLE=%d; GPU access NOT tested or permitted by this probe.\r\n",suitable);
    return suitable?EFI_SUCCESS:EFI_UNSUPPORTED;
}

#endif /* !TX_READONLY_DIAGNOSTIC */

static inline EFI_STATUS dp_unchanged(DmaProbe *p) {
    UINT32 command=0; UINT64 attrs=0,root_attrs=0,support=0;
    EFI_STATUS s=read_config(p->pci,4,&command); if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(p->pci->Attributes,4,p->pci,EfiPciIoAttributeOperationGet,0,&attrs);
    if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(p->root->GetAttributes,3,p->root,&support,&root_attrs);
    if(EFI_ERROR(s)) return s;
    BOOLEAN same=(UINT16)command==p->command && attrs==p->pci_attributes && root_attrs==p->root_attributes;
    note(L"PCI command=%04x PCI attrs=%lx root attrs=%lx UNCHANGED=%d\r\n",command&0xffff,attrs,root_attrs,same);
    return same?EFI_SUCCESS:EFI_DEVICE_ERROR;
}

#endif
