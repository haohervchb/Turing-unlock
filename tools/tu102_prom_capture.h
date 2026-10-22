/* Diagnostic ROM capture only. Included after note()/read_bar().
 * No BAR0 writes, firmware execution, resets, or PCI configuration writes.
 */
#define PROM_CAPTURE_SIZE 0x80000u
static struct {
    const char *name;
    UINT8 *data;
    UINTN size;
} prom_captures[]={{"bulk",NULL,0},{"scalar",NULL,0},{"repeat",NULL,0}};

static UINT32 crc32_ieee(const UINT8 *data, UINTN size) {
    UINT32 crc=0xffffffffu;
    for(UINTN i=0;i<size;i++) {
        crc^=data[i];
        for(UINTN bit=0;bit<8;bit++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}

static void free_prom_captures(void) {
    for(UINTN i=0;i<3;i++) {
        if(prom_captures[i].data) FreePool(prom_captures[i].data);
        prom_captures[i].data=NULL; prom_captures[i].size=0;
    }
}

static void report_prom(UINTN pass) {
    const UINT8 *data=prom_captures[pass].data;
    UINTN size=prom_captures[pass].size;
    UINT32 software=crc32_ieee(data,size),firmware=0;
    EFI_STATUS s=uefi_call_wrapper(BS->CalculateCrc32,3,(VOID *)data,size,&firmware);
    note(L"PROM %a bytes=%x software_crc=%08x firmware_crc=%08x status=%r\r\n",
         prom_captures[pass].name,size,software,firmware,s);
    note(L"Expected Linux 512KiB CRC=41FDE4D3; match=%d\r\n",
         size==PROM_CAPTURE_SIZE && software==0x41fde4d3);
    if(size>=0x3f060+0x9a10) {
        note(L"PROM %a FWSEC prod range 0003F060+00009A10 software_crc=%08x\r\n",
             prom_captures[pass].name,crc32_ieee(data+0x3f060,0x9a10));
    }
    for(UINTN off=0;off<size;off+=0x10000) {
        UINTN length=size-off; if(length>0x10000) length=0x10000;
        note(L"PROM %a chunk=%05x bytes=%x crc=%08x\r\n",
             prom_captures[pass].name,off,length,crc32_ieee(data+off,length));
    }
}

static EFI_STATUS capture_prom(EFI_PCI_IO_PROTOCOL *pci) {
    free_prom_captures();
    for(UINTN pass=0;pass<3;pass++) {
        prom_captures[pass].data=AllocatePool(PROM_CAPTURE_SIZE);
        if(!prom_captures[pass].data) return EFI_OUT_OF_RESOURCES;
        // Distinct fills expose incomplete-success reads during comparison.
        SetMem(prom_captures[pass].data,PROM_CAPTURE_SIZE,(UINT8)(0xa5u+pass*0x11u));
    }
    for(UINTN pass=0;pass<3;pass++) {
        note(L"BEGIN PROM %a read; 512 KiB\r\n",prom_captures[pass].name);
        if(EFI_ERROR(log_status)) return log_status;
        EFI_STATUS s=EFI_SUCCESS;
        if(pass!=1) {
            s=uefi_call_wrapper(pci->Mem.Read,6,pci,EfiPciIoWidthUint32,0,
                               (UINT64)0x300000,(UINTN)(PROM_CAPTURE_SIZE/4),prom_captures[pass].data);
            if(!EFI_ERROR(s)) prom_captures[pass].size=PROM_CAPTURE_SIZE;
        } else {
            for(UINTN off=0;off<PROM_CAPTURE_SIZE;off+=4) {
                UINT32 value=0;
                s=read_bar(pci,0x300000+(UINT32)off,&value);
                if(EFI_ERROR(s)) break;
                CopyMem(prom_captures[pass].data+off,&value,sizeof(value));
                prom_captures[pass].size=off+4;
                if((off&0xffff)==0xfffc) note(L"PROM scalar progress=%x bytes\r\n",off+4);
            }
        }
        note(L"END PROM %a read bytes=%x status=%r\r\n",prom_captures[pass].name,prom_captures[pass].size,s);
        if(prom_captures[pass].size) report_prom(pass);
        if(EFI_ERROR(s)) return s;
        if(EFI_ERROR(log_status)) return log_status;
    }
    for(UINTN pass=1;pass<3;pass++) {
        UINTN differing=0,first=PROM_CAPTURE_SIZE;
        for(UINTN i=0;i<PROM_CAPTURE_SIZE;i++) {
            if(prom_captures[0].data[i]!=prom_captures[pass].data[i]) {
                if(!differing) first=i;
                differing++;
            }
        }
        note(L"PROM bulk vs %a differing_bytes=%x first_offset=%x\r\n",
             prom_captures[pass].name,differing,first);
    }
    note(L"Capture completed; checksum matches do NOT authorize firmware execution.\r\n");
    return EFI_SUCCESS;
}

static EFI_STATUS save_capture(const char *label, const UINT8 *data, UINTN size) {
    if(!log_device || !capture_prefix[0]) return EFI_NOT_READY;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
    EFI_FILE_PROTOCOL *root=NULL,*file=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->HandleProtocol,3,log_device,&fs_guid,(VOID **)&fs);
    if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(fs->OpenVolume,2,fs,&root);
    if(EFI_ERROR(s)) return s;
    CHAR16 name[180]; SPrint(name,sizeof(name),L"%s-%a.bin",capture_prefix,label);
    s=uefi_call_wrapper(root->Open,5,root,&file,name,EFI_FILE_MODE_READ,0);
    if(!EFI_ERROR(s)) {
        uefi_call_wrapper(file->Close,1,file);
        s=EFI_ACCESS_DENIED;
    } else if(s==EFI_NOT_FOUND) {
        s=uefi_call_wrapper(root->Open,5,root,&file,name,
                           EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
        if(!EFI_ERROR(s)) {
            UINTN written=size;
            s=uefi_call_wrapper(file->Write,3,file,&written,(VOID *)data);
            if(!EFI_ERROR(s) && written!=size) s=EFI_DEVICE_ERROR;
            EFI_STATUS flushed=uefi_call_wrapper(file->Flush,1,file);
            if(!EFI_ERROR(s)) s=flushed;
            EFI_STATUS closed=uefi_call_wrapper(file->Close,1,file);
            if(!EFI_ERROR(s)) s=closed;
        }
    }
    uefi_call_wrapper(root->Close,1,root);
    note(L"Save raw ROM %s bytes=%x status=%r\r\n",name,size,s);
    return s;
}

static EFI_STATUS save_prom_captures(void) {
    EFI_STATUS result=EFI_SUCCESS;
    for(UINTN i=0;i<3;i++) {
        if(!prom_captures[i].data || !prom_captures[i].size) continue;
        EFI_STATUS s=save_capture(prom_captures[i].name,prom_captures[i].data,prom_captures[i].size);
        if(EFI_ERROR(s) && !EFI_ERROR(result)) result=s;
    }
    return result;
}
