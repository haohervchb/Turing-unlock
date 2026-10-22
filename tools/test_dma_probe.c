/* Mocked firmware services. Any config/MMIO write callback aborts. */
#ifdef DMA_TEST_USE_EXPERIMENT
#include "tu102_experiment.c"
#else
#include "tu102_dma_probe.c"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EFI_BOOT_SERVICES services;
static EFI_SYSTEM_TABLE system_table;
static EFI_SIMPLE_TEXT_OUT_PROTOCOL console;
static EFI_PCI_IO_PROTOCOL pci;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL root;
static unsigned allocations,frees,maps,unmaps,mem_reads,cfg_reads;
static int fail_alloc,fail_map,fail_unmap,fail_free,fail_cfg,fail_mem,bad_version;
static int bad_root,missing_root,bad_id,bad_bar,missing_target,duplicate_target,short_map,null_mapping;
static UINT32 command;
static UINT32 gst_status;
static UINT64 pci_attrs,root_attrs,address;
static UINT8 path_data[12]={2,1,4,0,1,1,4,0,0x7f,0xff,4,0};
static UINT8 dmar[80],xsdt[44],rsdp[36];
static EFI_CONFIGURATION_TABLE config;
static EFI_STATUS EFIAPI out(EFI_SIMPLE_TEXT_OUT_PROTOCOL *s,CHAR16 *t) { (void)s;(void)t;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI attrs(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 a,UINT64 *r) {
    assert(p==&pci && !a && r);
    assert(op==EfiPciIoAttributeOperationSupported || op==EfiPciIoAttributeOperationGet);
    *r=op==EfiPciIoAttributeOperationSupported?0x800000000006377bULL:pci_attrs;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI root_get(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,UINT64 *s,UINT64 *a) {
    assert(r==&root);*s=0x6377b;*a=root_attrs;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI alloc(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_ALLOCATE_TYPE type,EFI_MEMORY_TYPE mem,UINTN pages,VOID **host,UINT64 a) {
    assert(r==&root && type==AllocateAnyPages && mem==EfiBootServicesData && pages==8192);
    assert(a==EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE);
    if(fail_alloc)return EFI_UNSUPPORTED;
    *host=aligned_alloc(4096,PROBE_BYTES);assert(*host);allocations++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI release(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,UINTN pages,VOID *host) {
    assert(r==&root && pages==8192 && host);
    if(fail_free)return EFI_DEVICE_ERROR;
    free(host);frees++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI map(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_OPERATION op,VOID *host,UINTN *n,EFI_PHYSICAL_ADDRESS *dev,VOID **token) {
    assert(r==&root && op==EfiPciOperationBusMasterCommonBuffer64 && host && *n==PROBE_BYTES);
    if(fail_map)return EFI_UNSUPPORTED;
    *dev=address;*token=null_mapping?NULL:&root;if(short_map)*n-=4096;maps++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unmap(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,VOID *token) {
    assert(r==&root && token==(null_mapping?NULL:&root));
    if(fail_unmap)return EFI_DEVICE_ERROR;
    unmaps++;return EFI_SUCCESS;
}
static UINT32 cfg_value(UINT32 off) {
    if(off==0)return bad_id?0xffffffff:0x1e0710de;
    if(off==0x2c)return 0x866a1043;
    if(off==0x10)return bad_bar?0xf9000000:0xfa000000;
    assert(off==4);return command;
}
static EFI_STATUS EFIAPI cfg(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *v) {
    assert(p==&pci && w==EfiPciIoWidthUint32 && n==1);
    if(fail_cfg)return EFI_DEVICE_ERROR;
    *(UINT32 *)v=cfg_value(off);cfg_reads++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI rcfg(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 a,UINTN n,VOID *v) {
    assert(r==&root && w==EfiPciIoWidthUint32 && n==1 && (a&~0xffULL)==0x02000000);
    if(fail_cfg)return EFI_DEVICE_ERROR;
    *(UINT32 *)v=cfg_value((UINT32)(a&0xff));cfg_reads++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mem(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 a,UINTN n,VOID *v) {
    assert(r==&root && w==EfiPciIoWidthUint32 && n==1);
    assert((a&~0xfffULL)==0xfbffc000 || (a&~0xfffULL)==0xfbffd000);
    assert((a&0xfff)==0 || (a&0xfff)==0x1c);
    if(fail_mem)return EFI_DEVICE_ERROR;
    *(UINT32 *)v=(a&0xfff)?gst_status:(bad_version?0xffffffff:0x10);
    mem_reads++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI no_cfg_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 a,UINTN n,VOID *v) {
    (void)p;(void)w;(void)a;(void)n;(void)v;abort();
}
static EFI_STATUS EFIAPI no_root_write(EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *r,EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH w,UINT64 a,UINTN n,VOID *v) {
    (void)r;(void)w;(void)a;(void)n;(void)v;abort();
}
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **h) {
    assert(type==ByProtocol && !CompareMem(g,&pci_guid,sizeof(*g)) && !key);
    *n=missing_target?0:duplicate_target?2:1;*h=malloc(2*sizeof(**h));assert(*h);
    (*h)[0]=&pci;(*h)[1]=&pci;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI freepool(VOID *p) { free(p);return EFI_SUCCESS; }
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID *g,VOID **v) {
    if(!CompareMem(g,&pci_guid,sizeof(*g))) { assert(h==&pci);*v=&pci; }
    else if(!CompareMem(g,&path_guid,sizeof(*g))) { assert(h==&pci);*v=path_data; }
    else { assert(!CompareMem(g,&root_guid,sizeof(*g)) && h==&root);*v=&root; }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
    assert(p==&pci);*s=0;*b=2;*d=0;*f=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI path(EFI_GUID *g,EFI_DEVICE_PATH **p,EFI_HANDLE *h) {
    assert(!CompareMem(g,&root_guid,sizeof(*g)) && (VOID *)*p==path_data);
    if(missing_root)return EFI_NOT_FOUND;
    if(!bad_root)*p=(VOID *)(path_data+4);
    *h=&root;return EFI_SUCCESS;
}
static void put16(UINT8 *p,UINT16 n) { memcpy(p,&n,2); }
static void put32(UINT8 *p,UINT32 n) { memcpy(p,&n,4); }
static void put64(UINT8 *p,UINT64 n) { memcpy(p,&n,8); }
static void checksum(UINT8 *p,UINTN n,UINTN pos) {
    p[pos]=0;UINT8 sum=0;for(UINTN i=0;i<n;i++)sum+=p[i];p[pos]=(UINT8)-sum;
}
static void acpi(void) {
    memset(dmar,0,sizeof(dmar));memcpy(dmar,"DMAR",4);put32(dmar+4,sizeof(dmar));
    memcpy(dmar+10,"ALWAREALIENWRE",14);
    put16(dmar+50,16);put64(dmar+56,0xfbffc000);
    put16(dmar+66,16);put64(dmar+72,0xfbffd000);checksum(dmar,sizeof(dmar),9);
    memset(xsdt,0,sizeof(xsdt));memcpy(xsdt,"XSDT",4);put32(xsdt+4,sizeof(xsdt));
    put64(xsdt+36,(UINT64)(UINTN)dmar);checksum(xsdt,sizeof(xsdt),9);
    memset(rsdp,0,sizeof(rsdp));memcpy(rsdp,"RSD PTR ",8);rsdp[15]=2;
    put32(rsdp+20,sizeof(rsdp));put64(rsdp+24,(UINT64)(UINTN)xsdt);
    checksum(rsdp,20,8);checksum(rsdp,sizeof(rsdp),32);
    config.VendorGuid=acpi_guid;config.VendorTable=rsdp;
    system_table.ConfigurationTable=&config;system_table.NumberOfTableEntries=1;
}
static DmaProbe setup(void) {
    allocations=frees=maps=unmaps=mem_reads=cfg_reads=0;
    fail_alloc=fail_map=fail_unmap=fail_free=fail_cfg=fail_mem=bad_version=0;
    bad_root=missing_root=bad_id=bad_bar=missing_target=duplicate_target=short_map=null_mapping=0;
    root.SegmentNumber=0;command=7;pci_attrs=0x718;root_attrs=0x700;address=0x800000000ULL;gst_status=0x80000000;
    loglen=log_written=0;log_status=EFI_SUCCESS;log_file=NULL;
    acpi();DmaProbe p;memset(&p,0,sizeof(p));p.pci=&pci;p.root=&root;p.device=&pci;
    p.command=7;p.pci_attributes=pci_attrs;p.root_attributes=root_attrs;return p;
}
int main(void) {
    memset(&services,0,sizeof(services));memset(&system_table,0,sizeof(system_table));
    console.OutputString=out;system_table.ConOut=&console;system_table.BootServices=&services;
    ST=&system_table;BS=&services;
    services.FreePool=freepool;services.LocateHandleBuffer=locate;services.HandleProtocol=handle;services.LocateDevicePath=path;
    pci.Attributes=attrs;pci.Pci.Read=cfg;pci.Pci.Write=no_cfg_write;pci.GetLocation=location;
    root.AllocateBuffer=alloc;root.FreeBuffer=release;root.Map=map;root.Unmap=unmap;
    root.GetAttributes=root_get;root.Pci.Read=rcfg;root.Pci.Write=no_root_write;
    root.Mem.Read=mem;root.Mem.Write=no_root_write;
    unsigned cases=0;DmaProbe p=setup();
    assert(dp_target(&p)==EFI_SUCCESS && dp_root(&p)==EFI_SUCCESS);cases++;
    p=setup();missing_target=1;assert(dp_target(&p)==EFI_NOT_FOUND && !cfg_reads);cases++;
    p=setup();duplicate_target=1;assert(dp_target(&p)==EFI_NOT_FOUND && !cfg_reads);cases++;
    p=setup();bad_id=1;assert(dp_target(&p)==EFI_UNSUPPORTED);cases++;
    p=setup();bad_bar=1;assert(dp_target(&p)==EFI_UNSUPPORTED);cases++;
    p=setup();command=3;assert(dp_target(&p)==EFI_UNSUPPORTED);cases++;
    p=setup();fail_cfg=1;assert(EFI_ERROR(dp_target(&p)));cases++;
    p=setup();missing_root=1;assert(dp_root(&p)==EFI_NOT_FOUND);cases++;
    p=setup();bad_root=1;assert(dp_root(&p)==EFI_NOT_FOUND);cases++;
    p=setup();root.SegmentNumber=1;assert(dp_root(&p)==EFI_UNSUPPORTED && !cfg_reads);cases++;
    p=setup();bad_id=1;assert(dp_root(&p)==EFI_SECURITY_VIOLATION);cases++;
    p=setup();fail_cfg=1;assert(EFI_ERROR(dp_root(&p)));cases++;
    p=setup();assert(dp_map(&p)==EFI_SUCCESS && dp_release(&p)==EFI_SUCCESS && frees==1 && unmaps==1);cases++;
    p=setup();null_mapping=1;assert(dp_map(&p)==EFI_SUCCESS && dp_release(&p)==EFI_SUCCESS && unmaps==1);cases++;
    p=setup();fail_alloc=1;assert(dp_map(&p)==EFI_UNSUPPORTED && dp_release(&p)==EFI_SUCCESS && !maps && !frees);cases++;
    p=setup();fail_map=1;assert(dp_map(&p)==EFI_UNSUPPORTED && dp_release(&p)==EFI_SUCCESS && frees==1 && !unmaps);cases++;
    p=setup();short_map=1;assert(dp_map(&p)==EFI_UNSUPPORTED && dp_release(&p)==EFI_SUCCESS && unmaps==1);cases++;
    const UINT64 rejected[]={0,0xfe000000,0x800000001ULL,0x10000000000ULL-PROBE_BYTES+4096};
    for(UINTN i=0;i<sizeof(rejected)/sizeof(rejected[0]);i++) {
        p=setup();address=rejected[i];assert(dp_map(&p)==EFI_UNSUPPORTED && dp_release(&p)==EFI_SUCCESS && frees==1);cases++;
    }
    p=setup();fail_unmap=1;assert(dp_map(&p)==EFI_SUCCESS && EFI_ERROR(dp_release(&p)) && !frees && p.pool);
    fail_unmap=0;assert(dp_release(&p)==EFI_SUCCESS);cases++;
    p=setup();fail_free=1;assert(dp_map(&p)==EFI_SUCCESS && EFI_ERROR(dp_release(&p)) && unmaps==1 && p.pool);
    fail_free=0;assert(dp_release(&p)==EFI_SUCCESS && unmaps==1);cases++;
    p=setup();log_status=EFI_DEVICE_ERROR;assert(EFI_ERROR(dp_map(&p)) && !allocations);cases++;
    p=setup();assert(dp_unchanged(&p)==EFI_SUCCESS);cases++;
    p=setup();command=3;assert(EFI_ERROR(dp_unchanged(&p)));cases++;
    p=setup();pci_attrs|=0x8000;assert(EFI_ERROR(dp_unchanged(&p)));cases++;
    p=setup();root_attrs|=0x8000;assert(EFI_ERROR(dp_unchanged(&p)));cases++;
    p=setup();assert(dp_dmar(&p)==EFI_SUCCESS && mem_reads==4);cases++;
    p=setup();system_table.NumberOfTableEntries=0;assert(dp_dmar(&p)==EFI_NOT_FOUND && !mem_reads);cases++;
    p=setup();rsdp[8]++;assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();xsdt[9]++;assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();dmar[9]++;assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();put64(dmar+72,0xfbffc000);checksum(dmar,sizeof(dmar),9);assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();put64(dmar+72,0xfa000000);checksum(dmar,sizeof(dmar),9);assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();put16(dmar+66,0);checksum(dmar,sizeof(dmar),9);assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();put16(dmar+66,17);checksum(dmar,sizeof(dmar),9);assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();dmar[10]='X';checksum(dmar,sizeof(dmar),9);assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();fail_mem=1;assert(EFI_ERROR(dp_dmar(&p)));cases++;
    p=setup();bad_version=1;assert(EFI_ERROR(dp_dmar(&p)) && mem_reads==1);cases++;
    p=setup();log_status=EFI_DEVICE_ERROR;assert(EFI_ERROR(dp_dmar(&p)) && !mem_reads);cases++;
    p=setup();
    FILE *file=fopen("artifacts/2080ti-dmar-linux.bin","rb");assert(file);
    UINT8 captured[220];assert(fread(captured,1,sizeof(captured),file)==sizeof(captured));
    assert(fgetc(file)==EOF);fclose(file);
    assert(dp_dmar_table(&p,captured,sizeof(captured))==EFI_SUCCESS && mem_reads==4);cases++;
    printf("PASS: %u DMA diagnostic cases; mock PCI/MMIO writes abort; no hardware access.\n",cases);
    return 0;
}
