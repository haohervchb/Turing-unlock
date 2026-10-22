/* Host-only entry-path regression tests, including the r=14 failure class.
 * Existing PCI/memory cases are retained and run first. No real hardware.
 */
#define main original_tests_main
#include "test_preflight.c"
#undef main

static int esp_token[2], image_token;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL filesystem;
static EFI_FILE_PROTOCOL root_file, output_file, existing_file, capture_file;
static EFI_LOADED_IMAGE loaded_info;
static UINT8 device_path[512];
static UINTN fs_count=1, loaded_mode, write_calls, flush_calls, create_calls, close_calls, stalls;
static UINTN write_failure, flush_failure, short_write, disk_size, opened_existing;
static int existing_logs, open_failure;
static CHAR8 disk_log[32768];
static UINTN blob_index,blob_creates,blob_bytes;
static int blob_exists,blob_short,blob_flush_failure;
static BOOLEAN contains16(const CHAR16 *s,const CHAR16 *part) {
    for(;*s;s++) {
        UINTN i=0;
        while(s[i] && part[i] && s[i]==part[i]) i++;
        if(!part[i]) return TRUE;
    }
    return FALSE;
}

static EFI_STATUS EFIAPI startup_locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,
                                        VOID *key,UINTN *n,EFI_HANDLE **h) {
    if(!memcmp(guid,&pci_guid,sizeof(*guid))) return locate(type,guid,key,n,h);
    assert(!memcmp(guid,&fs_guid,sizeof(*guid)) && fs_count<=2);
    *n=fs_count; *h=malloc(2*sizeof(**h));
    for(UINTN i=0;i<fs_count;i++) (*h)[i]=&esp_token[i];
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI startup_handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
    if(h==&mock_pci) return handle(h,guid,out);
    if(h==&image_token) {
        assert(!memcmp(guid,&LoadedImageProtocol,sizeof(*guid)));
        if(loaded_mode==1) return EFI_NOT_FOUND;
        loaded_info.DeviceHandle=loaded_mode==2 ? NULL:&image_token; // deliberately NOT the ESP
        *out=&loaded_info; return EFI_SUCCESS;
    }
    assert(h==&esp_token[0] || h==&esp_token[1]);
    if(!memcmp(guid,&path_guid,sizeof(*guid))) *out=device_path;
    else { assert(!memcmp(guid,&fs_guid,sizeof(*guid))); *out=&filesystem; }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,EFI_FILE_PROTOCOL **out) {
    assert(self==&filesystem); *out=&root_file; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,
                                   CHAR16 *name,UINT64 mode,UINT64 attrs) {
    assert(self==&root_file && name && !attrs);
    if(contains16(name,L".bin")) {
        assert(log_device && config_writes==2 && test_cmd==0x00100000);
        if(mode==EFI_FILE_MODE_READ) {
            if(blob_exists) { *out=&existing_file; return EFI_SUCCESS; }
            return EFI_NOT_FOUND;
        }
        assert(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE));
        blob_index=contains16(name,L"-bulk.bin") ? 0:contains16(name,L"-scalar.bin") ? 1:2;
        blob_creates++; *out=&capture_file; return EFI_SUCCESS;
    }
    assert(!bar_reads && !config_writes); // file creation MUST precede GPU access
    if(open_failure) return EFI_DEVICE_ERROR;
    if(mode==EFI_FILE_MODE_READ) {
        if(existing_logs) { if(existing_logs>0) existing_logs--; *out=&existing_file; opened_existing++; return EFI_SUCCESS; }
        return EFI_NOT_FOUND;
    }
    assert(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE));
    create_calls++; *out=&output_file; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_write(EFI_FILE_PROTOCOL *self,UINTN *size,VOID *data) {
    if(self==&capture_file) {
        assert(*size==prom_captures[blob_index].size);
        assert(!memcmp(data,prom_captures[blob_index].data,*size));
        if(blob_short) *size/=2;
        blob_bytes+=*size; return EFI_SUCCESS;
    }
    assert(self==&output_file); write_calls++;
    if(write_calls==write_failure) return EFI_DEVICE_ERROR;
    if(short_write && write_calls==1) *size/=2;
    assert(disk_size+*size<sizeof(disk_log));
    memcpy(disk_log+disk_size,data,*size); disk_size+=*size; disk_log[disk_size]=0;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_flush(EFI_FILE_PROTOCOL *self) {
    if(self==&capture_file) return blob_flush_failure ? EFI_DEVICE_ERROR:EFI_SUCCESS;
    assert(self==&output_file); flush_calls++;
    return flush_calls==flush_failure ? EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *self) {
    assert(self==&root_file || self==&output_file || self==&existing_file || self==&capture_file);
    close_calls++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI get_time(EFI_TIME *when,EFI_TIME_CAPABILITIES *caps) {
    (void)caps; memset(when,0,sizeof(*when)); when->Year=2026; when->Month=10; when->Day=21;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI stall(UINTN duration) {
    assert(duration==5000000); stalls++; return EFI_SUCCESS;
}
static void valid_path(void) {
    memset(device_path,0,sizeof(device_path));
    // Real firmware layout, independent of GNU-EFI's padded native struct.
    device_path[0]=4; device_path[1]=1; device_path[2]=42;
    device_path[4]=1; // partition 1, little endian
    device_path[9]=8; // start LBA 0x800
    device_path[17]=0x98; device_path[18]=0x21; // sectors 0x219800
    memcpy(device_path+24,&esp_guid,sizeof(esp_guid));
    device_path[40]=2; device_path[41]=2;
    EFI_DEVICE_PATH *end=(EFI_DEVICE_PATH *)(device_path+42);
    SetDevicePathEndNode(end);
}
static void startup_reset(void) {
    clear_counts(); log_written=0; log_file=NULL; log_status=EFI_SUCCESS;
    log_device=NULL; capture_prefix[0]=0;
    blob_index=blob_creates=blob_bytes=0; blob_exists=blob_short=blob_flush_failure=rom_mode=0;
    write_calls=flush_calls=create_calls=close_calls=stalls=0;
    write_failure=flush_failure=short_write=disk_size=opened_existing=0;
    loaded_mode=0; fs_count=1; existing_logs=open_failure=0; disk_log[0]=0;
    test_id=0x1e0710de; test_sub=0x866a1043; test_bus=2; test_boot=0x162000a1;
    test_cmd=0x00100000; test_barlo=0xf9000000; fail_read_off=0xffffffff;
    fail_enable=ignore_enable=fail_restore=memory_mode=0;
    page_allocs=page_frees=0;
    valid_path();
}
static void check_run(BOOLEAN gpu_expected) {
    assert(run_preflight(&image_token)==EFI_SUCCESS && stalls==1 && !log_file);
    if(gpu_expected) {
        assert(bar_reads==COMPLETE_GPU_READS && config_writes==2 && test_cmd==0x00100000);
        assert(blob_creates==3 && blob_bytes==3*PROM_CAPTURE_SIZE);
        assert(strstr((const char *)disk_log,"GPU preflight status=Success"));
        assert(strstr((const char *)disk_log,"Returning to GRUB"));
    } else assert(!bar_reads && !config_writes);
}
#ifndef PREFLIGHT_STARTUP_TEST_MAIN
#define PREFLIGHT_STARTUP_TEST_MAIN main
#endif
int PREFLIGHT_STARTUP_TEST_MAIN(void) {
    original_tests_main();
    static EFI_RUNTIME_SERVICES runtime;
    RT=&runtime; runtime.GetTime=get_time;
    BS->LocateHandleBuffer=startup_locate; BS->HandleProtocol=startup_handle; BS->Stall=stall;
    // LoadImage, StartImage, Exit, and ResetSystem remain NULL: any use crashes.
    filesystem.OpenVolume=volume; root_file.Open=file_open; root_file.Close=file_close;
    output_file.Write=file_write; output_file.Flush=file_flush; output_file.Close=file_close;
    capture_file.Write=file_write; capture_file.Flush=file_flush; capture_file.Close=file_close;
    existing_file.Close=file_close;
    startup_reset(); check_run(TRUE); // unrelated LoadedImage.DeviceHandle
    startup_reset(); loaded_mode=1; check_run(TRUE); // protocol NOT_FOUND (r=14)
    startup_reset(); loaded_mode=2; check_run(TRUE); // NULL DeviceHandle
    startup_reset(); fs_count=0; check_run(FALSE);
    startup_reset(); fs_count=2; check_run(FALSE); // duplicate matching partition GUID
    startup_reset(); device_path[24]^=1; check_run(FALSE); // wrong GPT signature
    startup_reset(); existing_logs=1; check_run(TRUE); assert(opened_existing==1 && create_calls==1);
    startup_reset(); existing_logs=-1; check_run(FALSE); assert(opened_existing==100 && !create_calls);
    startup_reset(); open_failure=1; check_run(FALSE);
    startup_reset(); write_failure=1; check_run(FALSE);
    startup_reset(); short_write=1; check_run(FALSE);
    startup_reset(); flush_failure=1; check_run(FALSE);
    startup_reset(); write_failure=8; // fails after memory decode is enabled
    assert(run_preflight(&image_token)==EFI_SUCCESS);
    assert(config_writes==2 && test_cmd==0x00100000 && bar_reads==1 && !log_file);
    puts("PASS: 13 startup/logging cases; missing image metadata tolerated; ambiguity/I/O failure fail closed; PCI restore on logging failure");
    startup_reset(); assert(is_target_esp((EFI_DEVICE_PATH *)device_path));
    device_path[2]=0; assert(!is_target_esp((EFI_DEVICE_PATH *)device_path));
    valid_path(); device_path[2]=255; device_path[3]=255;
    assert(!is_target_esp((EFI_DEVICE_PATH *)device_path));
    valid_path(); device_path[42+2]=3;
    assert(!is_target_esp((EFI_DEVICE_PATH *)device_path));
    assert(!is_target_esp(NULL));
    puts("PASS: 5 device-path parser cases");
    startup_reset(); rom_mode=1; check_run(TRUE);
    assert(strstr((const char *)disk_log,"bulk vs repeat differing_bytes=1 first_offset=400"));
    startup_reset(); rom_mode=2; check_run(TRUE);
    assert(strstr((const char *)disk_log,"bulk vs scalar differing_bytes=1 first_offset=400"));
    startup_reset(); rom_mode=3; check_run(TRUE);
    assert(!strstr((const char *)disk_log,"bulk vs repeat differing_bytes=0"));
    startup_reset(); fail_read_off=0x301000;
    assert(run_preflight(&image_token)==EFI_SUCCESS && test_cmd==0x00100000);
    assert(blob_creates==2 && blob_bytes==PROM_CAPTURE_SIZE+0x1000);
    startup_reset(); blob_exists=1;
    assert(run_preflight(&image_token)==EFI_SUCCESS && !blob_creates && !blob_bytes);
    startup_reset(); blob_short=1;
    assert(run_preflight(&image_token)==EFI_SUCCESS && blob_creates==3 && blob_bytes==3*PROM_CAPTURE_SIZE/2);
    assert(strstr((const char *)disk_log,"Raw capture files status=Device Error"));
    startup_reset(); blob_flush_failure=1;
    assert(run_preflight(&image_token)==EFI_SUCCESS);
    assert(strstr((const char *)disk_log,"Raw capture files status=Device Error"));
    puts("PASS: 7 ROM capture cases; changed/partial reads detected; raw files only after PCI restore; no overwrites; I/O failures reported");
    assert(crc32_ieee((const UINT8 *)"123456789",9)==0xcbf43926);
    assert(crc32_ieee((const UINT8 *)"",0)==0);
#ifdef TU102_TEST_PRIVATE_ROM
    FILE *reference=fopen("artifacts/2080ti-original-prom.bin","rb"); assert(reference);
    UINT8 *bytes=malloc(PROM_CAPTURE_SIZE); assert(bytes);
    assert(fread(bytes,1,PROM_CAPTURE_SIZE,reference)==PROM_CAPTURE_SIZE); fclose(reference);
    assert(crc32_ieee(bytes,PROM_CAPTURE_SIZE)==0x41fde4d3);
    assert(crc32_ieee(bytes+0x3f060,0x9a10)==0xf9328437);
    free(bytes);
    puts("PASS: 4 CRC cases, including known vector, saved Linux ROM, and native FWSEC bytes");
#else
    puts("PASS: 2 public CRC cases; private ROM fixture checks not enabled");
#endif
    free_prom_captures();
    return 0;
}
