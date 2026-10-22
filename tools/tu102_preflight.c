/* Board-specific UEFI preflight v4, launched by GRUB; not an unlocker.
 * Temporarily sets only PCI COMMAND.MEMORY on the exact target if needed,
 * then restores/verifies the original 16-bit command register on all exits.
 * BAR0 access is READ ONLY. No bus-master enable, resets, BAR0 writes,
 * NVRAM writes, VBIOS flashing, or GPU firmware execution.
 * Finds the known ESP by GPT partition GUID, streams a timestamped log,
 * and returns to GRUB. Does NOT load or start another EFI application.
 */
#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include <stdarg.h>

#ifndef TU102_LOG_PREFIX
#define TU102_LOG_PREFIX L"preflight-v4"
#endif
#ifndef TU102_CONSOLE_ALLOWED
#define TU102_CONSOLE_ALLOWED 1
#endif

#ifndef TU102_LOG_BYTES
#define TU102_LOG_BYTES 16384
#endif
static CHAR8 logbuf[TU102_LOG_BYTES];
static UINTN loglen;
static EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
/* Public source intentionally has no usable log partition configured.
 * Configure this initializer only after auditing the exact target ESP and
 * the partition geometry in is_target_esp(). Host tests use simulated paths.
 * This privacy change means public builds differ from the tested host EFI.
 */
static const EFI_GUID esp_guid = {0,0,0,{0,0,0,0,0,0,0,0}};
static EFI_FILE_PROTOCOL *log_file;
static UINTN log_written;
static EFI_STATUS log_status;
static EFI_HANDLE log_device;
static CHAR16 capture_prefix[160];

static EFI_STATUS flush_log(void) {
    if(!log_file || EFI_ERROR(log_status)) return log_status;
    UINTN remaining=loglen-log_written, written=remaining;
    if(remaining) {
        log_status=uefi_call_wrapper(log_file->Write,3,log_file,&written,logbuf+log_written);
        if(!EFI_ERROR(log_status) && written!=remaining) log_status=EFI_DEVICE_ERROR;
        if(!EFI_ERROR(log_status)) log_written+=written;
    }
    if(!EFI_ERROR(log_status)) log_status=uefi_call_wrapper(log_file->Flush,1,log_file);
    return log_status;
}

static void note(const CHAR16 *fmt, ...) {
    CHAR16 line[768]; va_list args; UINTN i;
    va_start(args,fmt); VSPrint(line,sizeof(line),(CHAR16 *)fmt,args); va_end(args);
    if(TU102_CONSOLE_ALLOWED)
        uefi_call_wrapper(ST->ConOut->OutputString,2,ST->ConOut,line);
    for(i=0;line[i] && loglen+1<sizeof(logbuf);i++)
        logbuf[loglen++]=line[i]<128 ? (CHAR8)line[i] : '?';
    logbuf[loglen]=0;
    flush_log();
#ifdef TU102_LOG_STRICT
    /* A full in-memory log must not silently allow additional GPU writes. */
    if(line[i] && !EFI_ERROR(log_status)) log_status=EFI_VOLUME_FULL;
#endif
}

static EFI_STATUS read_config(EFI_PCI_IO_PROTOCOL *pci, UINT32 off, UINT32 *v) {
    return uefi_call_wrapper(pci->Pci.Read,5,pci,EfiPciIoWidthUint32,off,1,v);
}
static EFI_STATUS read_bar(EFI_PCI_IO_PROTOCOL *pci, UINT32 off, UINT32 *v) {
    if(EFI_ERROR(log_status)) return log_status;
    return uefi_call_wrapper(pci->Mem.Read,6,pci,EfiPciIoWidthUint32,0,(UINT64)off,1,v);
}

#ifndef TU102_LOG_HELPERS_ONLY
#include "tu102_prom_capture.h"

static EFI_STATUS inspect_enabled_bar(EFI_PCI_IO_PROTOCOL *target) {
    UINT32 boot0; UINTN i;
    EFI_STATUS s=read_bar(target,0,&boot0); if(EFI_ERROR(s)) return s;
    note(L"BOOT0=%08x\r\n",boot0);
    if(boot0!=0x162000a1) { note(L"Unexpected BOOT0: refusing further BAR access.\r\n"); return EFI_UNSUPPORTED; }
    struct { const char *name; UINT32 off; } regs[]={
        {"FECS_FEATURE_PLM",0x409650},{"FECS_FEATURE_READOUT",0x409660},
        {"FECS_SS0",0x409664},{"FECS_SPEED_READOUT",0x409668},{"FECS_SS1",0x40966c},
        {"WPR2_LO",0x1fa824},{"WPR2_HI",0x1fa828},
        {"SEC2_CPUCTL",0x840100},{"GSP_CPUCTL",0x110100},
        {"GFW_BOOT_OK",0x118234},{"VGA_WORKSPACE_BASE",0x625f04},
        {"PROM_HEADER",0x300000}
    };
    for(i=0;i<sizeof(regs)/sizeof(regs[0]);i++) {
        note(L"Reading %a @%08x\r\n",regs[i].name,regs[i].off);
        UINT32 value=0; s=read_bar(target,regs[i].off,&value);
        note(L"%a @%08x = %08x status=%r\r\n",regs[i].name,regs[i].off,value,s);
        if(EFI_ERROR(s)) return s;
    }
    UINT32 header=0;
    s=read_bar(target,0x300000,&header);
    if(!EFI_ERROR(s) && (header&0xffff)==0xaa55) {
        s=capture_prom(target);
    } else if(!EFI_ERROR(s)) {
        note(L"ROM signature missing; capture refused.\r\n");
        s=EFI_COMPROMISED_DATA;
    }
    return s;
}

static EFI_STATUS inspect_gpu(void) {
    EFI_HANDLE *handles=NULL; UINTN count=0,i;
    EFI_PCI_IO_PROTOCOL *target=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&pci_guid,NULL,&count,&handles);
    if(EFI_ERROR(s)) return s;
    for(i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *pci=NULL; UINTN seg,bus,dev,fn;
        s=uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&pci_guid,(VOID **)&pci);
        if(EFI_ERROR(s) || !pci) continue;
        s=uefi_call_wrapper(pci->GetLocation,5,pci,&seg,&bus,&dev,&fn);
        if(!EFI_ERROR(s) && seg==0 && bus==2 && dev==0 && fn==0) { target=pci; break; }
    }
    FreePool(handles);
    if(!target) { note(L"Target 0000:02:00.0 absent. No GPU accessed.\r\n"); return EFI_NOT_FOUND; }
    UINT32 id,sub,command,barlo,barhi=0;
    if(EFI_ERROR(read_config(target,0,&id)) || EFI_ERROR(read_config(target,0x2c,&sub)) ||
       EFI_ERROR(read_config(target,4,&command)) || EFI_ERROR(read_config(target,0x10,&barlo)))
        return EFI_DEVICE_ERROR;
    // Offset 0x14 is BAR1 on a 32-bit BAR0, not BAR0's high half.
    if((barlo&7)==4 && EFI_ERROR(read_config(target,0x14,&barhi))) return EFI_DEVICE_ERROR;
    note(L"BDF=0000:02:00.0 PCI_ID=%08x SUBSYSTEM=%08x COMMAND_STATUS=%08x\r\n",
         id,sub,command);
    note(L"BAR0 width=%d base=%08x:%08x\r\n",(barlo&7)==4 ? 64:32,barhi,barlo&~15u);
    if(id!=0x1e0710de || sub!=0x866a1043) {
        note(L"Identity mismatch: no writes or BAR access.\r\n"); return EFI_UNSUPPORTED;
    }
    if((barlo&1) || ((barlo&6)!=0 && (barlo&6)!=4) || (!(barlo&~15u) && !barhi) ||
       barlo==0xffffffff || barhi==0xffffffff) {
        note(L"Invalid/unassigned BAR0. No changes.\r\n"); return EFI_NOT_READY;
    }
    if(command&2) return inspect_enabled_bar(target);
    // 16-bit config write avoids touching write-one-to-clear PCI STATUS bits.
    // No BAR relocation and no bus-master/DMA enable. Restore even on failure.
    UINT16 original=(UINT16)command, enabled=original|2;
    UINT32 check=0;
    note(L"Temporarily enabling COMMAND.MEMORY only: %04x -> %04x\r\n",original,enabled);
    if(EFI_ERROR(log_status)) return log_status;
    s=uefi_call_wrapper(target->Pci.Write,5,target,EfiPciIoWidthUint16,4,1,&enabled);
    if(!EFI_ERROR(s)) {
        s=read_config(target,4,&check);
        if(!EFI_ERROR(s) && (UINT16)check!=enabled) s=EFI_DEVICE_ERROR;
        if(!EFI_ERROR(s)) s=inspect_enabled_bar(target);
    }
    EFI_STATUS restored=uefi_call_wrapper(target->Pci.Write,5,target,EfiPciIoWidthUint16,4,1,&original);
    if(!EFI_ERROR(restored)) {
        restored=read_config(target,4,&check);
        if(!EFI_ERROR(restored) && (UINT16)check!=original) restored=EFI_DEVICE_ERROR;
    }
    note(L"Original PCI COMMAND restore=%r readback=%04x expected=%04x\r\n",restored,(UINT16)check,original);
    return EFI_ERROR(restored) ? restored : s;
}

static EFI_STATUS inspect_host_memory(void) {
    UINTN needed=0,key=0,stride=0,capacity=0; UINT32 version=0;
    EFI_MEMORY_DESCRIPTOR *map=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->GetMemoryMap,5,&needed,NULL,&key,&stride,&version);
    if(s!=EFI_BUFFER_TOO_SMALL) return EFI_ERROR(s) ? s : EFI_COMPROMISED_DATA;
    if(stride<sizeof(*map) || stride>4096 || needed>0x100000) return EFI_COMPROMISED_DATA;
    for(UINTN retry=0;retry<3;retry++) {
        capacity=needed+16*stride;
        map=AllocatePool(capacity); if(!map) return EFI_OUT_OF_RESOURCES;
        needed=capacity;
        s=uefi_call_wrapper(BS->GetMemoryMap,5,&needed,map,&key,&stride,&version);
        if(s!=EFI_BUFFER_TOO_SMALL) break;
        FreePool(map); map=NULL;
        if(needed>0x100000 || stride<sizeof(*map) || stride>4096) return EFI_COMPROMISED_DATA;
    }
    if(EFI_ERROR(s)) { if(map) FreePool(map); return s; }
    if(!map || needed>capacity || stride<sizeof(*map) || stride>4096 || needed%stride) {
        if(map) FreePool(map);
        return EFI_COMPROMISED_DATA;
    }
    UINT64 high_pages=0; UINTN attempts=0; BOOLEAN allocated=FALSE;
    note(L"Memory map entries=%d stride=%d version=%d\r\n",needed/stride,stride,version);
    for(UINTN off=0;off<needed;off+=stride) {
        EFI_MEMORY_DESCRIPTOR *d=(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map+off);
        if(!d->NumberOfPages || d->NumberOfPages>((~(UINT64)0-d->PhysicalStart)>>12)) continue;
        UINT64 end=d->PhysicalStart+(d->NumberOfPages<<12);
        if(d->Type!=EfiConventionalMemory || end<=0x100000000ULL) continue;
        UINT64 start=d->PhysicalStart<0x100000000ULL ? 0x100000000ULL:d->PhysicalStart;
        UINT64 pages=(end-start)>>12; high_pages+=pages;
        note(L"Free high RAM: %lx..%lx pages=%lx attr=%lx\r\n",start,end,pages,d->Attribute);
        if(!allocated && pages>=2 && attempts++<16) {
            EFI_PHYSICAL_ADDRESS scratch=end-8192,requested=scratch;
            s=uefi_call_wrapper(BS->AllocatePages,4,AllocateAddress,EfiLoaderData,2,&scratch);
            note(L"AllocateAddress requested=%lx returned=%lx status=%r\r\n",requested,scratch,s);
            if(!EFI_ERROR(s)) {
                EFI_STATUS released=uefi_call_wrapper(BS->FreePages,2,scratch,2);
                note(L"FreePages=%r\r\n",released);
                if(EFI_ERROR(released)) { FreePool(map); return released; }
                if(scratch!=requested) { FreePool(map); return EFI_COMPROMISED_DATA; }
                allocated=TRUE;
            }
        }
    }
    FreePool(map);
    note(L"Conventional RAM above4G=%ld MiB; exact-address allocation succeeded=%d\r\n",high_pages>>8,allocated);
    return allocated ? EFI_SUCCESS:EFI_NOT_FOUND;
}

#endif /* !TU102_LOG_HELPERS_ONLY: exclude ALL GPU/config writes and probes. */

/* The GRUB PE loader may reuse its own image handle. Never assume that
 * LoadedImage.DeviceHandle identifies the partition containing this file.
 * Only the unique, known GPT partition is permitted as our log destination.
 */
static BOOLEAN is_target_esp(const EFI_DEVICE_PATH *path) {
    BOOLEAN matched=FALSE;
    if(!path) return FALSE;
    for(UINTN off=0,nodes=0;nodes<64;nodes++) {
        UINTN length=DevicePathNodeLength(path);
        if(length<sizeof(*path) || length>4096-off) return FALSE;
        if(IsDevicePathEnd(path)) return length==sizeof(*path) && matched;
        if(DevicePathType(path)==MEDIA_DEVICE_PATH && DevicePathSubType(path)==MEDIA_HARDDRIVE_DP) {
            // The on-wire HD() node is 42 bytes. GNU-EFI's native C struct
            // has trailing padding on x86_64, so sizeof(struct) is NOT valid.
            if(length!=42) return FALSE;
            const UINT8 *bytes=(const UINT8 *)path;
            UINT32 number; UINT64 start,size;
            CopyMem(&number,(VOID *)(bytes+4),sizeof(number));
            CopyMem(&start,(VOID *)(bytes+8),sizeof(start));
            CopyMem(&size,(VOID *)(bytes+16),sizeof(size));
            if(bytes[40]==MBR_TYPE_EFI_PARTITION_TABLE_HEADER && bytes[41]==SIGNATURE_TYPE_GUID &&
               number==1 && start==0x800 && size==0x219800 &&
               CompareMem((VOID *)(bytes+24),(VOID *)&esp_guid,sizeof(esp_guid))==0) matched=TRUE;
        }
        off+=length;
        if(off>4096-sizeof(*path)) return FALSE;
        path=(const EFI_DEVICE_PATH *)((const UINT8 *)path+length);
    }
    return FALSE;
}

static EFI_STATUS find_esp(EFI_HANDLE *device) {
    EFI_HANDLE *handles=NULL; UINTN count=0,matches=0;
    *device=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&fs_guid,NULL,&count,&handles);
    if(EFI_ERROR(s)) return s;
    for(UINTN i=0;i<count;i++) {
        EFI_DEVICE_PATH *path=NULL;
        s=uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&path_guid,(VOID **)&path);
        if(!EFI_ERROR(s) && is_target_esp(path)) { *device=handles[i]; matches++; }
    }
    FreePool(handles);
    if(matches==1) return EFI_SUCCESS;
    *device=NULL;
    return matches ? EFI_ACCESS_DENIED:EFI_NOT_FOUND;
}

static EFI_STATUS open_log(EFI_HANDLE device, const EFI_TIME *when) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
    EFI_FILE_PROTOCOL *root=NULL,*file=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->HandleProtocol,3,device,&fs_guid,(VOID **)&fs);
    if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(fs->OpenVolume,2,fs,&root); if(EFI_ERROR(s)) return s;
    // Preserve existing logs even if the firmware clock repeats or is invalid.
    for(UINTN attempt=0;attempt<100;attempt++) {
        CHAR16 name[160];
        SPrint(capture_prefix,sizeof(capture_prefix),L"\\EFI\\2080TI-PREFLIGHT\\" TU102_LOG_PREFIX L"-%04d%02d%02d-%02d%02d%02d-%02d",
            when->Year,when->Month,when->Day,when->Hour,when->Minute,when->Second,attempt);
        SPrint(name,sizeof(name),L"%s.log",capture_prefix);
        s=uefi_call_wrapper(root->Open,5,root,&file,name,EFI_FILE_MODE_READ,0);
        if(!EFI_ERROR(s)) { uefi_call_wrapper(file->Close,1,file); s=EFI_ACCESS_DENIED; continue; }
        if(s!=EFI_NOT_FOUND) break;
        s=uefi_call_wrapper(root->Open,5,root,&file,name,
                           EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
        if(!EFI_ERROR(s)) {
            log_file=file;
            log_device=device;
            note(L"Log file: %s\r\n",name);
            s=log_status;
        }
        break;
    }
    uefi_call_wrapper(root->Close,1,root); return s;
}

#ifndef TU102_LOG_HELPERS_ONLY
static EFI_STATUS run_preflight(EFI_HANDLE image) {
    note(L"RTX2080TI PREFLIGHT v4 -- RAW ROM CAPTURE ONLY -- RETURN TO GRUB\r\n");
    EFI_LOADED_IMAGE *loaded=NULL; EFI_HANDLE device=NULL;
    EFI_STATUS s=uefi_call_wrapper(BS->HandleProtocol,3,image,&LoadedImageProtocol,(VOID **)&loaded);
    note(L"LoadedImage status=%r; device handle=%lx (diagnostic only, not trusted for ESP)\r\n",
         s,(!EFI_ERROR(s) && loaded) ? (UINT64)(UINTN)loaded->DeviceHandle:0ULL);
    s=find_esp(&device);
    note(L"Find exact configured ESP: %r\r\n",s);
    if(EFI_ERROR(s)) goto finished;
    EFI_TIME when; SetMem(&when,sizeof(when),0);
    s=uefi_call_wrapper(RT->GetTime,2,&when,NULL);
    note(L"Firmware time %04d-%02d-%02d %02d:%02d:%02d status=%r\r\n",
        when.Year,when.Month,when.Day,when.Hour,when.Minute,when.Second,s);
    s=open_log(device,&when);
    note(L"Open and flush startup log: %r\r\n",s);
    if(EFI_ERROR(log_status)) s=log_status;
    if(EFI_ERROR(s)) goto finished;
    // Known-vector check distinguishes software/firmware CRC behavior without
    // any hardware access. A firmware CRC discrepancy is recorded, not trusted.
    if(crc32_ieee((const UINT8 *)"123456789",9)!=0xcbf43926) {
        s=EFI_COMPROMISED_DATA;
        note(L"Software CRC self-test FAILED. No GPU access.\r\n");
        goto finished;
    }
    UINT32 firmware_test_crc=0;
    s=uefi_call_wrapper(BS->CalculateCrc32,3,(VOID *)"123456789",(UINTN)9,&firmware_test_crc);
    note(L"CRC known vector: software=CBF43926 firmware=%08x status=%r\r\n",firmware_test_crc,s);
    note(L"BEGIN GPU inspection; no GPU access occurred before startup log.\r\n");
    if(EFI_ERROR(log_status)) { s=log_status; goto finished; }
    EFI_STATUS gpu_status=inspect_gpu(); note(L"GPU preflight status=%r\r\n",gpu_status);
    // inspect_gpu has restored PCI COMMAND before any binary file is saved.
    EFI_STATUS capture_status=save_prom_captures();
    note(L"Raw capture files status=%r\r\n",capture_status);
    free_prom_captures();
    // AllocateMaxAddress may choose ANY lower address, not necessarily the
    // highest one. Inspect the memory map and test an exact free high address.
    s=inspect_host_memory(); note(L"Host high-memory preflight status=%r\r\n",s);
    if(EFI_ERROR(gpu_status)) s=gpu_status;
    if(EFI_ERROR(capture_status)) s=capture_status;
    note(L"No GPU BAR0 writes, bus-master enable, resets, or firmware execution.\r\n");
    note(L"PCI command restoration result is above.\r\n");
finished:
    free_prom_captures();
    note(L"Returning to GRUB. Select normal Ubuntu; do not rerun diagnostics on error.\r\n");
    if(log_file) {
        EFI_STATUS closed=uefi_call_wrapper(log_file->Close,1,log_file);
        log_file=NULL;
        if(!EFI_ERROR(log_status)) log_status=closed;
    }
    note(L"Final log status=%r; diagnostic status=%r\r\n",log_status,s);
    uefi_call_wrapper(BS->Stall,1,5000000);
    // Return success to the boot menu even on diagnostic failure; details are
    // in the flushed log / console. Never try to load a nested Ubuntu shim.
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    InitializeLib(image,system);
    return run_preflight(image);
}
#endif
