/* Host-only mocks. No physical MMIO, actual high-RAM dereference or code execution. */
#define TX_DIRECT_DMA
#define HIGH_DMA_TEST_MAIN legacy_high_cases
#include "test_high_dma.c"

static EFI_RUNTIME_SERVICES runtime_services;
static UINT8 direct_code[91] __attribute__((aligned(4096)));
static UINT32 pmen[2],caplo[2];
static UINT8 above4g,csm;
static UINTN setup_size;
static UINT32 setup_attrs,port_id,port_command,port_buses,port_class;
static int setup_fail,cap_fail,pmen_fail,port_fail,owned_readonly,owned_wrong_type;
static unsigned pmen_reads,forbidden_calls;

static EFI_STATUS EFIAPI direct_mem(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 addr,UINTN n,VOID *v) {
    UINTN off=addr&0xfff,unit=(addr&~0xfffULL)==0xfbffc000?0:1;
    if(off==0 || off==0x1c)return mem(r,w,addr,n,v);
    assert(r==&root && w==EfiPciIoWidthUint32 && n==1);
    assert((addr&~0xfffULL)==0xfbffc000 || (addr&~0xfffULL)==0xfbffd000);
    if(off==8) { if(cap_fail)return EFI_DEVICE_ERROR; *(UINT32 *)v=caplo[unit]; }
    else { assert(off==0x64);if(pmen_fail)return EFI_DEVICE_ERROR;*(UINT32 *)v=pmen[unit];pmen_reads++; }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI direct_cfg(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 addr,UINTN n,VOID *v) {
    if((addr&~0xffULL)==0x02000000)return rcfg(r,w,addr,n,v);
    assert(r==&root && w==EfiPciIoWidthUint32 && n==1 && (addr&~0xffULL)==0x00020000);
    if(port_fail)return EFI_DEVICE_ERROR;
    switch(addr&0xff) {
    case 0: *(UINT32 *)v=port_id;break;
    case 4: *(UINT32 *)v=port_command;break;
    case 8: *(UINT32 *)v=port_class;break;
    case 0x18: *(UINT32 *)v=port_buses;break;
    default: abort();
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI setup_get(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *n,VOID *buf) {
    assert(!CompareMem(guid,&dd_setup_guid,sizeof(*guid)) && !StrCmp(name,L"Setup"));
    assert(*n==0x2e1 && buf && attrs);
    if(setup_fail)return EFI_NOT_FOUND;
    memset(buf,0,*n);((UINT8 *)buf)[0x24d]=above4g;((UINT8 *)buf)[0x277]=csm;
    *n=setup_size;*attrs=setup_attrs;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI direct_memory_map(UINTN *n,EFI_MEMORY_DESCRIPTOR *p,UINTN *key,UINTN *stride,UINT32 *ver) {
    row_count=owned_host?4:3;
    if(owned_host) {
        rows[3].Type=owned_wrong_type?EfiReservedMemoryType:EfiBootServicesData;
        rows[3].PhysicalStart=owned_host;rows[3].NumberOfPages=8192;
        rows[3].Attribute=EFI_MEMORY_WB|(owned_readonly?0x20000ULL:0);
        rows[1].NumberOfPages=(owned_host-rows[1].PhysicalStart)>>12;
    }
    return memory_map(n,p,key,stride,ver);
}
static EFI_STATUS EFIAPI forbid_unmap(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,VOID *v) {
    (void)r;(void)v;forbidden_calls++;abort();
}
static Platform direct_setup(void) {
    Platform p=high_setup();
    memcpy(direct_code,dd_a14_map,sizeof(direct_code));
    root.Map=(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_MAP)(UINTN)direct_code;
    root.Unmap=forbid_unmap;root.Mem.Read=direct_mem;root.Pci.Read=direct_cfg;
    rows[2].Type=EfiBootServicesCode;rows[2].PhysicalStart=(UINTN)direct_code;
    rows[2].NumberOfPages=1;rows[2].Attribute=EFI_MEMORY_WB;
    services.GetMemoryMap=direct_memory_map;
    runtime_services.GetVariable=setup_get;RT=&runtime_services;
    above4g=1;csm=0;setup_size=0x2e1;setup_attrs=3;
    port_id=0x2f048086;port_command=7;port_buses=0x020200;port_class=0x06040002;
    caplo[0]=caplo[1]=0x60;pmen[0]=pmen[1]=0;pmen_reads=forbidden_calls=0;
    setup_fail=cap_fail=pmen_fail=port_fail=owned_readonly=owned_wrong_type=0;
    return p;
}
int main(void) {
    assert(old_dma_cases()==0);
    unsigned cases=0;Platform p=direct_setup();
    FILE *file=fopen("artifacts/dell-a14-audit/PciRootBridge.pe","rb");assert(file);
    assert(fseek(file,0x2a98,SEEK_SET)==0);UINT8 audited[91];
    assert(fread(audited,1,sizeof(audited),file)==sizeof(audited));fclose(file);
    assert(!memcmp(audited,dd_a14_map,sizeof(audited)));cases++;
    assert(dd_cpu_profile(0x756e6547,0x6c65746e,0x49656e69,0x306f2,0,46));cases++;
    assert(!dd_cpu_profile(0,0x6c65746e,0x49656e69,0x306f2,0,46));cases++;
    assert(!dd_cpu_profile(0x756e6547,0x6c65746e,0x49656e69,0x306f1,0,46));cases++;
    assert(!dd_cpu_profile(0x756e6547,0x6c65746e,0x49656e69,0x306f2,0x80000000,46));cases++;
    assert(!dd_cpu_profile(0x756e6547,0x6c65746e,0x49656e69,0x306f2,0,36));cases++;
    assert(dd_guard(&p.high)==EFI_SUCCESS && p.high.guarded && pmen_reads==2);cases++;
    p=direct_setup();above4g=0;
#ifdef TX_EXPERIMENT_V4
    assert(dd_guard(&p.high)==EFI_SUCCESS && p.high.guarded && !page_allocs);
#else
    assert(EFI_ERROR(dd_guard(&p.high)) && !p.high.guarded && !page_allocs);
#endif
    cases++;
    p=direct_setup();csm=1;assert(dd_guard(&p.high)==EFI_SUCCESS);cases++;
    p=direct_setup();csm=2;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();setup_size--;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();setup_attrs=0;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();setup_attrs=7;assert(dd_guard(&p.high)==EFI_SUCCESS);cases++;
    p=direct_setup();setup_attrs=0x43;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();setup_fail=1;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();direct_code[50]^=1;assert(dd_guard(&p.high)==EFI_SECURITY_VIOLATION && !p.high.guarded);cases++;
    p=direct_setup();rows[2].Type=EfiBootServicesData;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();rows[2].Attribute|=EFI_MEMORY_RP;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();rows[2].NumberOfPages=0;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();root.Map=(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_MAP)~(UINTN)0;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();row_stride=8;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();bad_map_size=1;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();map_retry_forever=1;assert(EFI_ERROR(dd_guard(&p.high)) && memory_map_calls==4);cases++;
    p=direct_setup();pool_fail=1;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();iommu_present=1;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();gst_status=0x80000000;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<2;j++) {
        p=direct_setup();pmen[i]=j?0x80000000:1;
        assert(dd_guard(&p.high)==EFI_ACCESS_DENIED && !p.high.guarded && !page_allocs);cases++;
    }
    p=direct_setup();caplo[1]=0xffffffff;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();caplo[1]=0;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();caplo[0]=caplo[1]=1;assert(dd_guard(&p.high)==EFI_SUCCESS && !pmen_reads);cases++;
    p=direct_setup();cap_fail=1;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();pmen_fail=1;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();port_id=0xffffffff;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();port_command=3;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();port_buses=0x030200;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();port_class=0;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();port_fail=1;assert(EFI_ERROR(dd_guard(&p.high)));cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && p.core.device_address==0x87e000000ULL);
    assert(dd_begin(&p.high,1)==EFI_SUCCESS && !p.high.mapped && !map_calls);
    assert(dd_begin(&p.high,1)==EFI_INVALID_PARAMETER);
    p.core.mutated=1;p.core.stage=TX_STAGE_BOOTER_RESET;
    assert(platform_next_input(&p)==0 && p.high.phase==2 && !map_calls && !unmaps);
    assert(experiment_release(&p)==EFI_ACCESS_DENIED && !page_frees && !command_writes);
    p.core.quiescent=1;assert(experiment_release(&p)==EFI_SUCCESS && page_frees==1 && command_writes==2 && command==7);cases++;
    p=direct_setup();owned_readonly=1;assert(EFI_ERROR(experiment_allocate(&p)));
    assert(experiment_release(&p)==EFI_SUCCESS && page_frees==1 && !command_writes);cases++;
    p=direct_setup();owned_wrong_type=1;assert(EFI_ERROR(experiment_allocate(&p)));
    assert(experiment_release(&p)==EFI_SUCCESS && page_frees==1);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS);
    assert(dd_begin(&p.high,2)==EFI_INVALID_PARAMETER && !p.high.phase);
    pmen[1]=1;assert(dd_begin(&p.high,1)==EFI_ACCESS_DENIED && !p.high.phase);
    assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && dd_begin(&p.high,1)==EFI_SUCCESS);
    p.core.mutated=1;p.core.stage=TX_STAGE_BOOTER_RESET;gst_status=0x80000000;
    assert(platform_next_input(&p)!=0 && p.high.phase==1);
    assert(experiment_release(&p)==EFI_ACCESS_DENIED && !page_frees);
    gst_status=0;p.core.quiescent=1;assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && dd_begin(&p.high,1)==EFI_SUCCESS);
    pmen[0]=1;assert(platform_event(&p,TX_STAGE_FWSEC_START,0,0)!=0);
    assert(platform_event(&p,TX_STAGE_BOOTER_START,0,0)!=0);
    assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS);
    log_status=EFI_DEVICE_ERROR;assert(EFI_ERROR(dd_begin(&p.high,1)) && !p.high.phase);
    log_status=EFI_SUCCESS;assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=direct_setup();assert(dd_begin(&p.high,1)==EFI_INVALID_PARAMETER);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS);p.high.guarded=FALSE;
    assert(dd_begin(&p.high,1)==EFI_INVALID_PARAMETER);assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=direct_setup();assert(platform_write(&p,TX_GSP+0x100,2)!=0 && !tx_file_only);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS);
    assert(platform_write(&p,TX_GSP+0x100,2)!=0 && !tx_file_only);
    assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && dd_begin(&p.high,1)==EFI_SUCCESS);
    assert(platform_next_input(&p)!=0 && p.high.phase==1);
    assert(experiment_release(&p)==EFI_SUCCESS && !command_writes);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS);free_pages_fail=1;
    assert(EFI_ERROR(experiment_release(&p)) && p.high.allocated && !page_frees);
    free_pages_fail=0;assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=direct_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && dd_begin(&p.high,1)==EFI_SUCCESS);
    p.core.mutated=1;p.core.quiescent=1;free_pages_fail=1;
    assert(EFI_ERROR(experiment_release(&p)) && p.high.allocated && command==3 && p.command_changed);
    free_pages_fail=0;assert(experiment_release(&p)==EFI_SUCCESS && command==7);cases++;
#ifdef TX_EXPERIMENT_V4
    p=direct_setup();above4g=2;assert(EFI_ERROR(dd_guard(&p.high)) && !page_allocs);cases++;
    p=direct_setup();above4g=0;csm=1;
    assert(dd_guard(&p.high)==EFI_SUCCESS && !map_calls && !command_writes);cases++;
    for(unsigned mode=0;mode<4;mode++) {
        p=direct_setup();above4g=0;csm=1;
        if(mode==0)gst_status=0x80000000;
        if(mode==1)pmen[0]=1;
        if(mode==2)iommu_present=1;
        if(mode==3)direct_code[0]^=1;
        assert(EFI_ERROR(dd_guard(&p.high)) && !p.high.guarded && !page_allocs);cases++;
    }
    p=direct_setup();above4g=0;csm=1;
    assert(experiment_allocate(&p)==EFI_SUCCESS && dd_begin(&p.high,1)==EFI_SUCCESS);
    assert(experiment_release(&p)==EFI_SUCCESS && page_frees==1 && !command_writes && !map_calls);cases++;
    p=direct_setup();rows[1].Attribute|=0x20000;
    assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES && !page_allocs);cases++;
    p=direct_setup();loglen=sizeof(logbuf)-2;log_written=loglen;
    note(L"overflow test");assert(log_status==EFI_VOLUME_FULL);
    assert(platform_write(&p,TX_GSP+0x100,2)!=0 && !command_writes);cases++;
#endif
    assert(!forbidden_calls && !map_calls && !unmaps);
    printf("PASS: %u direct-DMA cases; no hardware access, Map/Unmap calls, NVRAM writes or firmware patches.\n",cases);
    return 0;
}
