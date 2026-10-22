// Host-side mocked UEFI protocol tests. Never touches real hardware.
#define efi_main preflight_efi_main
#include "tu102_preflight.c"
#undef efi_main
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static EFI_PCI_IO_PROTOCOL mock_pci;
static UINT32 test_id=0x1e0710de,test_sub=0x866a1043,test_cmd=2,test_boot=0x162000a1;
static UINT32 test_barlo=0xf9000000;
static UINTN test_bus=2,bar_reads,rom_reads,config_writes;
static UINT32 fail_read_off=0xffffffff;
static int fail_enable,ignore_enable,fail_restore;
static int memory_mode;
static UINTN page_allocs,page_frees;
#define COMPLETE_GPU_READS (14u+PROM_CAPTURE_SIZE/4u+2u)
static int rom_mode;
static UINT32 mock_rom_word(UINTN offset) {
    return offset==0 ? 0xeb76aa55u:(UINT32)offset^0x81927364u;
}
static EFI_STATUS EFIAPI output(EFI_SIMPLE_TEXT_OUT_PROTOCOL *self,CHAR16 *str) {
    (void)self; (void)str; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI alloc_pool(EFI_MEMORY_TYPE type,UINTN size,VOID **ptr) {
    (void)type; *ptr=malloc(size); return *ptr ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI free_pool(VOID *ptr) { free(ptr); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **h) {
    (void)type; (void)key; assert(!memcmp(guid,&pci_guid,sizeof(*guid)));
    *n=1; *h=malloc(sizeof(**h)); (*h)[0]=&mock_pci; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
    assert(h==&mock_pci); assert(!memcmp(guid,&pci_guid,sizeof(*guid)));
    *out=&mock_pci; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *self,UINTN *seg,UINTN *bus,UINTN *dev,UINTN *fn) {
    assert(self==&mock_pci); *seg=0; *bus=test_bus; *dev=*fn=0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI cfg(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *out) {
    assert(self==&mock_pci && width==EfiPciIoWidthUint32 && count==1);
    UINT32 v;
    switch(off) { case 0:v=test_id;break; case 0x2c:v=test_sub;break;
      case 4:v=test_cmd;break; case 0x10:v=test_barlo;break; case 0x14:v=0;break;
      default:abort(); }
    *(UINT32 *)out=v; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI cfg_write(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
                                  UINT32 off,UINTN count,VOID *in) {
    assert(self==&mock_pci && width==EfiPciIoWidthUint16 && off==4 && count==1);
    assert(test_id==0x1e0710de && test_sub==0x866a1043 && test_bus==2);
    UINT16 wanted=*(UINT16 *)in;
    assert(((wanted^(UINT16)test_cmd)&~2u)==0); // Only memory decode may change.
    config_writes++;
    if(fail_restore && config_writes==2) return EFI_DEVICE_ERROR;
    if(!(ignore_enable && config_writes==1)) test_cmd=(test_cmd&0xffff0000)|wanted;
    if(fail_enable && config_writes==1) return EFI_DEVICE_ERROR; // Failure may follow a write.
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI memread(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
                                UINT8 bar,UINT64 off,UINTN count,VOID *out) {
    assert(self==&mock_pci && width==EfiPciIoWidthUint32 && bar==0);
    assert(test_id==0x1e0710de && test_sub==0x866a1043 && (test_cmd&2));
    bar_reads++;
    if(off==fail_read_off) return EFI_DEVICE_ERROR;
    if(count==0x20000) {
        assert(off==0x300000); rom_reads++;
        UINTN words=rom_mode==3 ? 64:count;
        for(UINTN i=0;i<words;i++) ((UINT32 *)out)[i]=mock_rom_word(i*4);
        if(rom_mode==1 && rom_reads==2) ((UINT8 *)out)[0x400]^=1;
        if(rom_mode==2) ((UINT8 *)out)[0x400]^=1;
        return EFI_SUCCESS;
    }
    assert(count==1);
    if(off>=0x300000 && off<0x380000) {
        *(UINT32 *)out=mock_rom_word(off-0x300000); return EFI_SUCCESS;
    }
    switch(off) {
      case 0: *(UINT32 *)out=test_boot;break;
      case 0x300000: *(UINT32 *)out=0xaa55;break;
      case 0x409650: case 0x409660: case 0x409664: case 0x409668: case 0x40966c:
      case 0x1fa824: case 0x1fa828: case 0x840100: case 0x110100: case 0x118234: case 0x625f04:
        assert(test_boot==0x162000a1); *(UINT32 *)out=0;break;
      default:abort();
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI crc(VOID *data,UINTN size,UINT32 *out) {
    assert(data && size); *out=crc32_ieee(data,size); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI get_map(UINTN *size,EFI_MEMORY_DESCRIPTOR *map,UINTN *key,
                                 UINTN *stride,UINT32 *version) {
    *stride=sizeof(*map)+8; *version=1; *key=123;
    if(memory_mode==3) *stride=8;
    UINTN required=3*(*stride);
    if(*size<required || !map) { *size=required; return EFI_BUFFER_TOO_SMALL; }
    memset(map,0,required); *size=required;
    EFI_MEMORY_DESCRIPTOR *low=map;
    EFI_MEMORY_DESCRIPTOR *high=(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map+*stride);
    EFI_MEMORY_DESCRIPTOR *reserved=(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map+2*(*stride));
    low->Type=EfiConventionalMemory; low->PhysicalStart=0x100000; low->NumberOfPages=0x70000;
    high->Type=memory_mode==1 ? EfiReservedMemoryType:EfiConventionalMemory;
    high->PhysicalStart=0x100000000ULL; high->NumberOfPages=0x100000;
    reserved->Type=EfiReservedMemoryType; reserved->PhysicalStart=0x200000000ULL; reserved->NumberOfPages=256;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI alloc_pages(EFI_ALLOCATE_TYPE type,EFI_MEMORY_TYPE memtype,UINTN pages,
                                     EFI_PHYSICAL_ADDRESS *address) {
    assert(type==AllocateAddress && memtype==EfiLoaderData && pages==2);
    assert(*address==0x1ffffe000ULL); page_allocs++;
    return memory_mode==2 ? EFI_NOT_FOUND:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_pages(EFI_PHYSICAL_ADDRESS address,UINTN pages) {
    assert(address==0x1ffffe000ULL && pages==2); page_frees++; return EFI_SUCCESS;
}
static void clear_counts(void) { free_prom_captures(); bar_reads=rom_reads=config_writes=0; loglen=0; }
int main(void) {
    static EFI_BOOT_SERVICES boot;
    static EFI_SYSTEM_TABLE system;
    static EFI_SIMPLE_TEXT_OUT_PROTOCOL console;
    ST=&system; BS=&boot; system.ConOut=&console; console.OutputString=output;
    boot.AllocatePool=alloc_pool; boot.FreePool=free_pool;
    boot.LocateHandleBuffer=locate; boot.HandleProtocol=handle; boot.CalculateCrc32=crc;
    boot.GetMemoryMap=get_map; boot.AllocatePages=alloc_pages; boot.FreePages=free_pages;
    mock_pci.GetLocation=location; mock_pci.Pci.Read=cfg; mock_pci.Pci.Write=cfg_write;
    mock_pci.Mem.Read=memread;
    // BAR0 write and reset entry points deliberately remain NULL.
    assert(inspect_gpu()==EFI_SUCCESS && rom_reads==2 && bar_reads==COMPLETE_GPU_READS && !config_writes);
    clear_counts(); test_id=0x2d0410de;
    assert(inspect_gpu()==EFI_UNSUPPORTED && bar_reads==0 && !config_writes);
    test_id=0x1e0710de; test_sub=0;
    assert(inspect_gpu()==EFI_UNSUPPORTED && bar_reads==0 && !config_writes);
    test_sub=0x866a1043; test_barlo=0;
    assert(inspect_gpu()==EFI_NOT_READY && bar_reads==0 && !config_writes);
    test_barlo=0xf9000000; test_bus=1;
    assert(inspect_gpu()==EFI_NOT_FOUND && bar_reads==0 && !config_writes);
    test_bus=2; test_boot=0xffffffff;
    assert(inspect_gpu()==EFI_UNSUPPORTED && bar_reads==1 && !config_writes);
    clear_counts(); test_boot=0x162000a1; test_cmd=0x00100000;
    assert(inspect_gpu()==EFI_SUCCESS && bar_reads==COMPLETE_GPU_READS && config_writes==2 && test_cmd==0x00100000);
    clear_counts(); test_boot=0xffffffff;
    assert(inspect_gpu()==EFI_UNSUPPORTED && bar_reads==1 && config_writes==2 && test_cmd==0x00100000);
    clear_counts(); test_boot=0x162000a1; fail_read_off=0x1fa824;
    assert(inspect_gpu()==EFI_DEVICE_ERROR && config_writes==2 && test_cmd==0x00100000);
    clear_counts(); fail_read_off=0xffffffff; fail_enable=1;
    assert(inspect_gpu()==EFI_DEVICE_ERROR && !bar_reads && config_writes==2 && test_cmd==0x00100000);
    clear_counts(); fail_enable=0; ignore_enable=1;
    assert(inspect_gpu()==EFI_DEVICE_ERROR && !bar_reads && config_writes==2 && test_cmd==0x00100000);
    clear_counts(); ignore_enable=0; fail_restore=1;
    assert(inspect_gpu()==EFI_DEVICE_ERROR && config_writes==2 && test_cmd==0x00100002);
    puts("PASS: 12 PCI/BAR cases; temporary memory decode and restore on success/failures; no DMA or BAR0 writes");
    clear_counts(); assert(inspect_host_memory()==EFI_SUCCESS && page_allocs==1 && page_frees==1);
    assert(strstr((const char *)logbuf,"100000000"));
    assert(strstr((const char *)logbuf,"1FFFFE000"));
    memory_mode=1; page_allocs=page_frees=0;
    assert(inspect_host_memory()==EFI_NOT_FOUND && !page_allocs && !page_frees);
    memory_mode=2;
    assert(inspect_host_memory()==EFI_NOT_FOUND && page_allocs==1 && !page_frees);
    memory_mode=3;
    assert(inspect_host_memory()==EFI_COMPROMISED_DATA);
    puts("PASS: 4 memory map/allocation cases, including descriptor stride and exact high-memory address");
    return 0;
}
