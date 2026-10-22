/* Host-only protocol simulations. Fake high addresses are NEVER dereferenced. */
#define TX_HIGH_DMA
#define DMA_TEST_USE_EXPERIMENT
#define main old_dma_cases
#include "test_dma_probe.c"
#undef main

static EFI_MEMORY_DESCRIPTOR rows[4];
static UINTN row_count,row_stride;
static unsigned page_allocs,page_frees,map_calls,command_writes,memory_map_calls;
static int iommu_present,iommu_error,pool_fail,allocate_failures,allocate_wrong,free_pages_fail;
static int map_retry_forever,bad_map_size,stream_short,stream_shift,stream_fail;
static EFI_PHYSICAL_ADDRESS owned_host;
static UINTN stream_offset,stream_bytes;

static EFI_STATUS EFIAPI pool_alloc(EFI_MEMORY_TYPE type,UINTN n,VOID **p) {
    (void)type;
    if(pool_fail)return EFI_OUT_OF_RESOURCES;
    *p=malloc(n);assert(*p);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI memory_map(UINTN *n,EFI_MEMORY_DESCRIPTOR *p,UINTN *key,UINTN *stride,UINT32 *version) {
    memory_map_calls++;*key=1;*stride=row_stride;*version=1;
    UINTN needed=row_count*row_stride;
    if(!p || *n<needed || map_retry_forever) { *n=needed;return EFI_BUFFER_TOO_SMALL; }
    if(bad_map_size) { *n+=1;return EFI_SUCCESS; }
    memset(p,0,needed);
    for(UINTN i=0;i<row_count;i++)memcpy((UINT8 *)p+i*row_stride,&rows[i],sizeof(rows[i]));
    *n=needed;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI pages_alloc(EFI_ALLOCATE_TYPE type,EFI_MEMORY_TYPE mem,UINTN pages,EFI_PHYSICAL_ADDRESS *addr) {
    assert(type==AllocateAddress && mem==EfiBootServicesData && pages==8192);
    assert(*addr>=0x100000000ULL && !(*addr&4095) && *addr<=0x10000000000ULL-TX_POOL_SIZE);
    page_allocs++;
    if(allocate_failures) { allocate_failures--;return EFI_OUT_OF_RESOURCES; }
    if(allocate_wrong)*addr-=4096;
    owned_host=*addr;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI pages_free(EFI_PHYSICAL_ADDRESS addr,UINTN pages) {
    assert(addr==owned_host && pages==8192);
    if(free_pages_fail)return EFI_DEVICE_ERROR;
    page_frees++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI iommu_locate(EFI_GUID *g,VOID *registration,VOID **outp) {
    assert(!CompareMem(g,&iommu_guid,sizeof(*g)) && !registration);
    *outp=iommu_present?&root:NULL;
    return iommu_error?EFI_DEVICE_ERROR:iommu_present?EFI_SUCCESS:EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI stream_map(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION op,VOID *host,UINTN *n,EFI_PHYSICAL_ADDRESS *dev,VOID **token) {
    assert(r==&root && op==EfiPciOperationBusMasterRead64 && host);
    map_calls++;
    stream_offset=(UINTN)host-owned_host;stream_bytes=*n;
    assert((stream_offset==0 && *n==TX_FWSEC_SIZE) ||
           (stream_offset==TX_SIGNATURE_OFF && *n==TX_POOL_SIZE-TX_SIGNATURE_OFF));
    if(stream_fail)return EFI_DEVICE_ERROR;
    *dev=(UINT64)(UINTN)host+(stream_shift?4096:0);*token=null_mapping?NULL:&root;
    if(stream_short)*n-=16;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI command_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *v) {
    assert(p==&pci && w==EfiPciIoWidthUint16 && off==4 && n==1);
    UINT16 value=*(UINT16 *)v;assert(value==3 || value==7);
    command=value;command_writes++;return EFI_SUCCESS;
}
static Platform high_setup(void) {
    DmaProbe d=setup();gst_status=0;
    Platform p;memset(&p,0,sizeof(p));p.pci=&pci;p.command=7;p.high.probe=d;
    services.GetMemoryMap=memory_map;services.AllocatePool=pool_alloc;
    services.AllocatePages=pages_alloc;services.FreePages=pages_free;services.LocateProtocol=iommu_locate;
    pci.Pci.Write=command_write;root.Map=stream_map;
    row_count=2;row_stride=48;memset(rows,0,sizeof(rows));
    rows[0].Type=EfiConventionalMemory;rows[0].PhysicalStart=0x70000000;
    rows[0].NumberOfPages=0x10000;rows[0].Attribute=EFI_MEMORY_WB;
    rows[1].Type=EfiConventionalMemory;rows[1].PhysicalStart=0x100000000ULL;
    rows[1].NumberOfPages=0x780000;rows[1].Attribute=EFI_MEMORY_WB;
    page_allocs=page_frees=map_calls=command_writes=memory_map_calls=0;
    iommu_present=iommu_error=pool_fail=allocate_failures=allocate_wrong=free_pages_fail=0;
    map_retry_forever=bad_map_size=stream_short=stream_shift=stream_fail=0;
    owned_host=stream_offset=stream_bytes=0;return p;
}
#ifndef HIGH_DMA_TEST_MAIN
#define HIGH_DMA_TEST_MAIN main
#endif
int HIGH_DMA_TEST_MAIN(void) {
    assert(old_dma_cases()==0);
    unsigned cases=0;Platform p=high_setup();
    assert(hd_guard(&p.high)==EFI_SUCCESS && p.high.guarded && p.high.probe.dmar_units==2);cases++;
    p=high_setup();iommu_present=1;assert(EFI_ERROR(hd_guard(&p.high)) && !p.high.guarded && !page_allocs);cases++;
    p=high_setup();iommu_error=1;assert(EFI_ERROR(hd_guard(&p.high)) && !page_allocs);cases++;
    p=high_setup();gst_status=0x80000000;assert(EFI_ERROR(hd_guard(&p.high)) && !page_allocs);cases++;
    p=high_setup();dmar[9]++;assert(EFI_ERROR(hd_guard(&p.high)) && !page_allocs);cases++;
    p=high_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && p.high.host==0x87e000000ULL);
    assert(p.core.device_address==p.high.host && (UINTN)p.core.pool==p.high.host);
    assert(experiment_release(&p)==EFI_SUCCESS && page_frees==1 && !command_writes);cases++;
    p=high_setup();row_count=1;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES && !page_allocs);cases++;
    p=high_setup();rows[1].NumberOfPages=8191;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES && !page_allocs);cases++;
    p=high_setup();rows[1].Attribute=EFI_MEMORY_UC;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES);cases++;
    p=high_setup();rows[1].Attribute|=EFI_MEMORY_RP;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES);cases++;
    p=high_setup();rows[1].Type=EfiReservedMemoryType;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES);cases++;
    p=high_setup();rows[1].NumberOfPages=~(UINT64)0;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES);cases++;
    p=high_setup();rows[1].PhysicalStart++;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES);cases++;
    p=high_setup();rows[1].PhysicalStart=0xff00000000ULL;rows[1].NumberOfPages=0x200000;
    assert(hd_allocate(&p.high)==EFI_SUCCESS && p.high.host==0xfffe000000ULL);assert(hd_release(&p.high)==EFI_SUCCESS);cases++;
    p=high_setup();allocate_failures=1;assert(hd_allocate(&p.high)==EFI_SUCCESS && page_allocs==2 && p.high.host==0x87c000000ULL);
    assert(hd_release(&p.high)==EFI_SUCCESS);cases++;
    p=high_setup();allocate_failures=100;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES && page_allocs==16);cases++;
    p=high_setup();allocate_wrong=1;assert(hd_allocate(&p.high)==EFI_COMPROMISED_DATA && p.high.allocated);
    assert(hd_release(&p.high)==EFI_SUCCESS && page_frees==1);cases++;
    p=high_setup();row_stride=8;assert(hd_allocate(&p.high)==EFI_COMPROMISED_DATA && !page_allocs);cases++;
    p=high_setup();row_stride=8192;assert(hd_allocate(&p.high)==EFI_COMPROMISED_DATA && !page_allocs);cases++;
    p=high_setup();bad_map_size=1;assert(hd_allocate(&p.high)==EFI_COMPROMISED_DATA && !page_allocs);cases++;
    p=high_setup();map_retry_forever=1;assert(hd_allocate(&p.high)==EFI_BUFFER_TOO_SMALL && memory_map_calls==4);cases++;
    p=high_setup();pool_fail=1;assert(hd_allocate(&p.high)==EFI_OUT_OF_RESOURCES && !page_allocs);cases++;
    p=high_setup();log_status=EFI_DEVICE_ERROR;assert(hd_allocate(&p.high)==EFI_DEVICE_ERROR && !page_allocs);cases++;
    p=high_setup();assert(hd_allocate(&p.high)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_SUCCESS);
    assert(hd_map(&p.high,1)==EFI_INVALID_PARAMETER && map_calls==1);
    assert(hd_release(&p.high)==EFI_SUCCESS && unmaps==1 && page_frees==1);cases++;
    p=high_setup();null_mapping=1;assert(hd_allocate(&p.high)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_SUCCESS);
    assert(hd_release(&p.high)==EFI_SUCCESS && unmaps==1);cases++;
    p=high_setup();stream_short=1;assert(hd_allocate(&p.high)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_UNSUPPORTED);
    assert(hd_release(&p.high)==EFI_SUCCESS && unmaps==1);cases++;
    p=high_setup();stream_shift=1;assert(hd_allocate(&p.high)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_UNSUPPORTED);
    assert(hd_release(&p.high)==EFI_SUCCESS && unmaps==1);cases++;
    p=high_setup();stream_fail=1;assert(hd_allocate(&p.high)==EFI_SUCCESS && EFI_ERROR(hd_map(&p.high,1)));
    assert(hd_release(&p.high)==EFI_SUCCESS && !unmaps && page_frees==1);cases++;
    p=high_setup();assert(hd_allocate(&p.high)==EFI_SUCCESS && hd_map(&p.high,2)==EFI_INVALID_PARAMETER && !map_calls);
    assert(hd_release(&p.high)==EFI_SUCCESS);cases++;
    p=high_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_SUCCESS);
    p.core.mutated=1;p.core.stage=TX_STAGE_BOOTER_RESET;
    assert(platform_next_input(&p)==0 && p.high.phase==2 && unmaps==1 && map_calls==2);
    assert(stream_offset==TX_SIGNATURE_OFF && stream_bytes==TX_POOL_SIZE-TX_SIGNATURE_OFF);
    assert(experiment_release(&p)==EFI_ACCESS_DENIED && !page_frees && !command_writes);
    p.core.quiescent=1;assert(experiment_release(&p)==EFI_SUCCESS && page_frees==1 && command_writes==2 && command==7);cases++;
    p=high_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_SUCCESS);
    p.core.mutated=1;p.core.stage=TX_STAGE_BOOTER_RESET;fail_unmap=1;
    assert(platform_next_input(&p)!=0 && map_calls==1 && p.high.mapped && !page_frees);
    fail_unmap=0;p.core.quiescent=1;assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=high_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_SUCCESS);
    p.core.mutated=1;p.core.stage=TX_STAGE_BOOTER_RESET;stream_fail=1;
    assert(platform_next_input(&p)!=0 && !p.high.mapped && !page_frees);
    assert(experiment_release(&p)==EFI_ACCESS_DENIED && !page_frees);
    p.core.quiescent=1;assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=high_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_SUCCESS);
    assert(platform_next_input(&p)!=0 && unmaps==0 && map_calls==1);
    assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=high_setup();assert(experiment_allocate(&p)==EFI_SUCCESS && hd_map(&p.high,1)==EFI_SUCCESS);fail_unmap=1;
    assert(EFI_ERROR(experiment_release(&p)) && p.high.allocated && !page_frees);
    fail_unmap=0;assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    p=high_setup();assert(experiment_allocate(&p)==EFI_SUCCESS);free_pages_fail=1;
    assert(EFI_ERROR(experiment_release(&p)) && p.high.allocated && !page_frees);
    free_pages_fail=0;assert(experiment_release(&p)==EFI_SUCCESS);cases++;
    printf("PASS: %u high-DMA/experiment platform cases; no physical GPU or high-RAM access.\n",cases);
    return 0;
}
