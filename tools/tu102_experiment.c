/* Board-specific experimental entry. Reuses only proven ESP/logging helpers.
 * The read-only diagnostic entry is retained under a DIFFERENT function name.
 * Nothing installs/arms this binary merely by compiling it.
 */
static int tx_file_only;
#ifdef TX_EXPERIMENT_V4
#ifndef TX_DIRECT_DMA
#define TX_DIRECT_DMA
#endif
#define TX_HIGH_DMA
#define TU102_LOG_HELPERS_ONLY
#define TU102_LOG_BYTES 32768
#define TU102_LOG_STRICT
#define TU102_LOG_PREFIX L"experiment-v4"
#define TX_BANNER L"RTX2080TI EXPERIMENT v4 -- OBSERVED BASELINES / DIRECT DMA; NO FLASH\r\n"
#elif defined(TX_DIRECT_DMA)
#define TX_HIGH_DMA
#define TU102_LOG_PREFIX L"experiment-v3"
#define TX_BANNER L"RTX2080TI EXPERIMENT v3 -- DIRECT-PHYSICAL DMA; NO FLASH\r\n"
#elif defined(TX_HIGH_DMA)
#define TU102_LOG_PREFIX L"experiment-v2"
#define TX_BANNER L"RTX2080TI EXPERIMENT v2 -- HIGH-RAM VOLATILE UNLOCK ATTEMPT\r\n"
#else
#define TU102_LOG_PREFIX L"experiment-v1"
#define TX_BANNER L"RTX2080TI EXPERIMENT v1 -- VOLATILE COMPUTE UNLOCK ATTEMPT\r\n"
#endif
#define TU102_CONSOLE_ALLOWED (!tx_file_only)
#define efi_main retained_readonly_preflight_entry
#include "tu102_preflight.c"
#undef efi_main
#include "tu102_experiment_core.h"
#ifdef TX_HIGH_DMA
#include "tu102_high_dma.h"
#endif
#ifdef TX_DIRECT_DMA
#include "tu102_direct_dma.h"
#endif

extern const UINT8 tx_native[], tx_native_end[];
extern const UINT8 tx_generic[], tx_generic_end[];
extern const UINT8 tx_booter[], tx_booter_end[];
extern const UINT8 tx_signature[], tx_signature_end[];
extern const UINT8 tx_boot[], tx_boot_end[];

typedef struct {
    EFI_PCI_IO_PROTOCOL *pci;
    UINT32 last_offset,last_value;
    EFI_STATUS last_status;
    UINT16 command;
    BOOLEAN dac_changed, mapped, command_changed;
    VOID *mapping;
    Tx core;
#ifdef TX_HIGH_DMA
    HighDma high;
#endif
} Platform;

#ifdef TX_EXPERIMENT_V4
static int platform_guard_note(void *ctx,uint32_t off,uint32_t want,uint32_t actual,uint32_t mask) {
    (void)ctx;
    note(L"GUARD @%08x actual=%08x expected=%08x mask=%08x\r\n",off,actual,want,mask);
    return EFI_ERROR(log_status);
}
#endif

static int platform_read(void *ctx,uint32_t off,uint32_t *value) {
    Platform *p=ctx; p->last_offset=off;
    *value=0;
    if(EFI_ERROR(log_status)) return 1;
    p->last_status=read_bar(p->pci,off,value); p->last_value=*value;
    return EFI_ERROR(p->last_status);
}
static int platform_write(void *ctx,uint32_t off,uint32_t value) {
    Platform *p=ctx; p->last_offset=off; p->last_value=value;
    if(EFI_ERROR(log_status) || !tx_write_allowed(off)) return 1;
#ifdef TX_DIRECT_DMA
    if(!p->high.guarded || !p->high.allocated || p->high.mapped ||
       (p->high.phase!=1 && p->high.phase!=2)) return 1;
#endif
    /* Do not re-enter the GPU's GOP renderer while resetting its engines.
     * Every stage still flushes the ESP log, including failures. */
    tx_file_only=1;
    p->last_status=uefi_call_wrapper(p->pci->Mem.Write,6,p->pci,EfiPciIoWidthUint32,
                                    0,(UINT64)off,1,&value);
    return EFI_ERROR(p->last_status);
}
static int platform_delay(void *ctx,uint32_t usec) {
    (void)ctx;
    return EFI_ERROR(uefi_call_wrapper(BS->Stall,1,(UINTN)usec));
}
static int platform_barrier(void *ctx) {
    Platform *p=ctx;
    __asm__ volatile("wbinvd" ::: "memory");
    p->last_status=uefi_call_wrapper(p->pci->Flush,1,p->pci);
    return EFI_ERROR(p->last_status);
}
static int platform_event(void *ctx,unsigned stage,uint32_t a,uint32_t b) {
    (void)ctx;
    static const CHAR16 *names[]={L"invalid",L"identity and firmware guards",
        L"native FWSEC reset",L"native FWSEC load",L"native FWSEC start",
        L"native FWSEC result",L"Booter reset",L"Booter load",L"Booter start",
        L"Booter result",L"engine cleanup",L"completed"};
    if(stage>=sizeof(names)/sizeof(names[0])) return 1;
    note(L"STAGE %d: %s a=%08x b=%08x\r\n",stage,names[stage],a,b);
#ifdef TX_DIRECT_DMA
    /* Recheck protections immediately before either firmware CPU starts.
     * These are read-only checks; a failure after reset holds the machine. */
    if(stage==TX_STAGE_FWSEC_START || stage==TX_STAGE_BOOTER_START) {
        Platform *p=ctx;
        p->last_status=dd_protection(&p->high);
        if(EFI_ERROR(p->last_status)) return 1;
    }
#endif
    return EFI_ERROR(log_status);
}

static EFI_STATUS experiment_target(Platform *p) {
    EFI_HANDLE *handles=NULL; UINTN count=0,matches=0;
    EFI_STATUS s=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&pci_guid,NULL,&count,&handles);
    if(EFI_ERROR(s)) return s;
    for(UINTN i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *pci=NULL; UINTN seg,bus,dev,fn;
        s=uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&pci_guid,(VOID **)&pci);
        if(EFI_ERROR(s) || !pci) continue;
        s=uefi_call_wrapper(pci->GetLocation,5,pci,&seg,&bus,&dev,&fn);
        if(!EFI_ERROR(s) && seg==0 && bus==2 && dev==0 && fn==0) { p->pci=pci; matches++; }
    }
    FreePool(handles);
    if(matches!=1) { note(L"Exact target absent/ambiguous; no GPU writes.\r\n"); return EFI_NOT_FOUND; }
    UINT32 id=0,sub=0,bar=0,command=0;
    if(EFI_ERROR(read_config(p->pci,0,&id)) || EFI_ERROR(read_config(p->pci,0x2c,&sub)) ||
       EFI_ERROR(read_config(p->pci,0x10,&bar)) || EFI_ERROR(read_config(p->pci,4,&command))) return EFI_DEVICE_ERROR;
    note(L"Exact target BDF 02:00.0 id=%08x sub=%08x BAR0=%08x command=%04x\r\n",id,sub,bar,command&0xffff);
    /* Current sole-display boot is deliberately required. Do not enable BME
     * or move BARs on a different boot topology. */
    if(id!=0x1e0710de || sub!=0x866a1043 || bar!=0xfa000000 || (command&0xffff)!=7)
        return EFI_UNSUPPORTED;
    p->command=(UINT16)command;
    return EFI_SUCCESS;
}

static EFI_STATUS experiment_allocate(Platform *p) {
#ifdef TX_HIGH_DMA
#ifdef TX_DIRECT_DMA
    EFI_STATUS s=dd_guard(&p->high); if(EFI_ERROR(s)) return s;
#else
    EFI_STATUS s=hd_guard(&p->high); if(EFI_ERROR(s)) return s;
#endif
    if(p->high.probe.pci!=p->pci) return EFI_SECURITY_VIOLATION;
    s=hd_allocate(&p->high); if(EFI_ERROR(s)) return s;
#ifdef TX_DIRECT_DMA
    s=dd_memory(&p->high); if(EFI_ERROR(s)) return s;
#endif
    p->core.pool=(VOID *)(UINTN)p->high.host;
    /* Candidate base for preparation. v2 requires Map verification; v3 uses
     * the explicitly guarded direct-physical path, NOT a firmware mapping. */
    p->core.device_address=p->high.host;
    return EFI_SUCCESS;
#else
    UINT64 supported=0,attributes=0;
    EFI_STATUS s=uefi_call_wrapper(p->pci->Attributes,4,p->pci,EfiPciIoAttributeOperationSupported,0,&supported);
    if(EFI_ERROR(s)) return s;
    s=uefi_call_wrapper(p->pci->Attributes,4,p->pci,EfiPciIoAttributeOperationGet,0,&attributes);
    if(EFI_ERROR(s)) return s;
    note(L"PCI attributes current=%lx supported=%lx\r\n",attributes,supported);
    if(!(supported&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)) return EFI_UNSUPPORTED;
    if(!(attributes&EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE)) {
        p->dac_changed=TRUE; /* restore even when Enable reports an error */
        s=uefi_call_wrapper(p->pci->Attributes,4,p->pci,EfiPciIoAttributeOperationEnable,
                            EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,NULL);
        if(EFI_ERROR(s)) return s;
    }
    VOID *pool=NULL;
    s=uefi_call_wrapper(p->pci->AllocateBuffer,6,p->pci,AllocateAnyPages,EfiBootServicesData,
                        (UINTN)(TX_POOL_SIZE>>12),&pool,EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE);
    if(EFI_ERROR(s)) return s;
    p->core.pool=pool;
    UINTN bytes=TX_POOL_SIZE; EFI_PHYSICAL_ADDRESS device=0;
    /* PCI_IO (unlike ROOT_BRIDGE_IO) has no CommonBuffer64 operation.
     * Enabled DUAL_ADDRESS_CYCLE makes the bus driver select 64-bit mapping. */
    s=uefi_call_wrapper(p->pci->Map,6,p->pci,EfiPciIoOperationBusMasterCommonBuffer,
                        pool,&bytes,&device,&p->mapping);
    if(EFI_ERROR(s)) return s;
    p->mapped=TRUE;
    note(L"Common-buffer host=%lx GPU=%lx bytes=%lx\r\n",(UINT64)(UINTN)pool,device,(UINT64)bytes);
    if(bytes!=TX_POOL_SIZE || device<0x100000000ULL || (device&4095) ||
       device>0x10000000000ULL-TX_POOL_SIZE) return EFI_UNSUPPORTED;
    p->core.device_address=device;
    return EFI_SUCCESS;
#endif
}

#ifdef TX_HIGH_DMA
static int platform_next_input(void *ctx) {
    Platform *p=ctx;
#ifdef TX_DIRECT_DMA
    if(!p->core.mutated || p->high.phase!=1 ||
       p->core.stage!=TX_STAGE_BOOTER_RESET) return 1;
    p->last_status=dd_begin(&p->high,2);
#else
    if(!p->core.mutated || !p->high.mapped || p->high.phase!=1 ||
       p->core.stage!=TX_STAGE_BOOTER_RESET) return 1;
    p->last_status=hd_unmap(&p->high);
    if(EFI_ERROR(p->last_status)) return 1;
    p->last_status=hd_map(&p->high,2);
#endif
    return EFI_ERROR(p->last_status);
}
#endif

static EFI_STATUS set_command(Platform *p,UINT16 command) {
    UINT32 value=0;
    EFI_STATUS s=uefi_call_wrapper(p->pci->Pci.Write,5,p->pci,EfiPciIoWidthUint16,4,1,&command);
    if(!EFI_ERROR(s)) s=read_config(p->pci,4,&value);
    if(!EFI_ERROR(s) && (UINT16)value!=command) s=EFI_DEVICE_ERROR;
    return s;
}
static EFI_STATUS experiment_release(Platform *p) {
    EFI_STATUS s;
    /* A partially executed firmware sequence MUST NOT return to GRUB or free
     * DMA backing. It stays in boot services until the user power-cycles. */
    if(p->core.mutated && !p->core.quiescent) return EFI_ACCESS_DENIED;
    if(p->core.mutated) {
        p->command_changed=TRUE;
        s=set_command(p,p->command&~4u); if(EFI_ERROR(s)) return s;
    }
#ifdef TX_HIGH_DMA
    s=hd_release(&p->high); if(EFI_ERROR(s)) return s;
    p->core.pool=NULL;
#else
    if(p->mapped) {
        s=uefi_call_wrapper(p->pci->Unmap,2,p->pci,p->mapping); if(EFI_ERROR(s)) return s;
        p->mapped=FALSE;
    }
    if(p->core.pool) {
        s=uefi_call_wrapper(p->pci->FreeBuffer,3,p->pci,(UINTN)(TX_POOL_SIZE>>12),p->core.pool);
        if(EFI_ERROR(s)) return s;
        p->core.pool=NULL;
    }
    if(p->dac_changed) {
        s=uefi_call_wrapper(p->pci->Attributes,4,p->pci,EfiPciIoAttributeOperationDisable,
                            EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,NULL);
        if(EFI_ERROR(s)) return s;
        p->dac_changed=FALSE;
    }
#endif
    if(p->command_changed) {
        s=set_command(p,p->command); if(EFI_ERROR(s)) return s;
        p->command_changed=FALSE;
    }
#ifdef TX_HIGH_DMA
    if(p->high.guarded) {
        s=dp_unchanged(&p->high.probe); if(EFI_ERROR(s)) return s;
    }
#endif
    return EFI_SUCCESS;
}

static void experiment_hold(void) {
    note(L"STOP: uncertain GPU state. DMA buffers retained. DO NOT continue boot.\r\n"
         L"Power the machine fully off, then on. The one-shot was consumed; normal Ubuntu is next.\r\n");
    flush_log();
    /* Log already persisted before trying GOP again, in case GOP is unusable. */
    tx_file_only=0;
    note(L"EXPERIMENT STOPPED. Fully power off/on now. Do not rerun this entry.\r\n");
    for(;;) uefi_call_wrapper(BS->Stall,1,1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
    InitializeLib(image,system);
    Platform p; SetMem(&p,sizeof(p),0);
    p.core.opaque=&p; p.core.read=platform_read; p.core.write=platform_write;
    p.core.delay=platform_delay; p.core.barrier=platform_barrier; p.core.event=platform_event;
    p.core.native_fwsec=tx_native; p.core.generic_bl=tx_generic;
#ifdef TX_EXPERIMENT_V4
    p.core.guard_note=platform_guard_note;
#endif
#ifdef TX_HIGH_DMA
    p.core.next_input=platform_next_input;
#endif
    EFI_HANDLE device=NULL; EFI_TIME when; SetMem(&when,sizeof(when),0);
    note(TX_BANNER);
    EFI_STATUS s=find_esp(&device); if(EFI_ERROR(s)) goto finish;
    s=uefi_call_wrapper(RT->GetTime,2,&when,NULL); if(EFI_ERROR(s)) goto finish;
    s=open_log(device,&when); if(EFI_ERROR(s) || EFI_ERROR(log_status)) goto finish;
    note(L"Not a read-only diagnostic. Resets GSP/SEC2 and runs signed firmware plus V67.\r\n"
         L"NO flash, ReBAR, fan, power, PCI link, or display-workspace changes.\r\n"
         L"If no Ubuntu or STOP message within 90 seconds: fully power off/on.\r\n");
#ifdef TX_EXPERIMENT_V4
    note(L"v4: finite observed SS tuples, exact locked readout and halted engines required.\r\n"
         L"Selector mask=0 means tuple policy, NOT arbitrary acceptance. Tuple pinned across preparation.\r\n"
         L"Above4G is observed only: no high BAR window is used. High-RAM identity DMA remains experimental.\r\n");
#endif
    if((UINTN)(tx_native_end-tx_native)!=TX_FWSEC_SIZE ||
       (UINTN)(tx_generic_end-tx_generic)!=TX_GENERIC_SIZE ||
       (UINTN)(tx_booter_end-tx_booter)!=TX_BOOTER_SIZE ||
       (UINTN)(tx_signature_end-tx_signature)!=TX_SIGNATURE_SIZE ||
       (UINTN)(tx_boot_end-tx_boot)!=TX_BOOT_CONTAINER_SIZE) { s=EFI_COMPROMISED_DATA; goto finish; }
    s=experiment_target(&p); if(EFI_ERROR(s)) goto finish;
    /* Read-only guards run BEFORE changing DMA attributes or allocating. */
    int result=tx_guards(&p.core);
    if(result) {
        note(L"Initial GPU guard refusal: core=%d last=%08x/%08x IO=%r; NO GPU writes.\r\n",
             result,p.last_offset,p.last_value,p.last_status);
        s=EFI_SECURITY_VIOLATION; goto finish;
    }
#ifdef TX_EXPERIMENT_V4
    note(L"All 39440 native FWSEC bytes and descriptor MATCH; observed SS pair=%08x/%08x pinned.\r\n",
         p.core.initial_ss0,p.core.initial_ss1);
#endif
    s=experiment_allocate(&p); if(EFI_ERROR(s)) goto finish;
    result=tx_prepare(&p.core,tx_booter,tx_signature,tx_boot);
    if(result) { s=EFI_COMPROMISED_DATA; goto finish; }
#ifdef TX_HIGH_DMA
    /* No host writes to the prepared pool after this point. FWSEC and Booter
     * consume separate immutable input regions; results are GPU registers. */
#ifdef TX_DIRECT_DMA
    s=dd_begin(&p.high,1); if(EFI_ERROR(s)) goto finish;
#else
    s=hd_map(&p.high,1); if(EFI_ERROR(s)) goto finish;
#endif
#endif
    note(L"Prepared native FWSEC; FB=11GiB heap=106MiB FRTS=2BFEE0000.\r\n"
         L"One attempt only. Firmware prerequisites must pass before Booter starts.\r\n"
         L"GPU writes begin next; console updates paused, ESP logging continues.\r\n");
    if(EFI_ERROR(log_status)) { s=log_status; goto finish; }
    s=uefi_call_wrapper(BS->SetWatchdogTimer,4,0,0,0,NULL);
    if(EFI_ERROR(s)) goto finish;
    result=tx_run(&p.core);
    note(L"CORE result=%d stage=%d mutated=%d unlocked=%d quiescent=%d last=%08x/%08x IO=%r\r\n",
         result,p.core.stage,p.core.mutated,p.core.unlocked,p.core.quiescent,
         p.last_offset,p.last_value,p.last_status);
    if(result && p.core.mutated) experiment_hold();
    s=result?EFI_DEVICE_ERROR:EFI_SUCCESS;
finish:
    if(p.pci) {
        EFI_STATUS released=experiment_release(&p);
        note(L"DMA/PCI release status=%r\r\n",released);
        if(EFI_ERROR(released) && (p.core.mutated || p.command_changed)) experiment_hold();
        if(EFI_ERROR(released)) s=released;
    }
    tx_file_only=0;
    note(L"Experiment result=%r; selector observation=%d (Linux benchmark still required).\r\n",s,p.core.unlocked);
    note(L"Returning to GRUB; no automatic repeat.\r\n");
    if(log_file) { uefi_call_wrapper(log_file->Close,1,log_file); log_file=NULL; }
    uefi_call_wrapper(BS->Stall,1,5000000);
    return EFI_SUCCESS;
}
