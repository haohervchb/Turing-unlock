/* DMA service diagnostic ONLY. No GPU MMIO/config writes, engine resets,
 * firmware blobs, firmware execution, DMA transfers, or IOMMU disabling.
 * Firmware Map/Unmap may manage temporary mapping state; this application
 * never grants device DMA permissions or writes IOMMU control registers.
 * The inherited preflight entry is discarded by --gc-sections at link time.
 */
#define TU102_LOG_PREFIX L"dma-probe-v1"
#define efi_main unused_readonly_preflight_entry
#include "tu102_preflight.c"
#undef efi_main

#include "tu102_dma_services.h"

EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
    InitializeLib(image,system);
    DmaProbe p; SetMem(&p,sizeof(p),0);
    EFI_HANDLE esp=NULL; EFI_TIME when; SetMem(&when,sizeof(when),0);
    note(L"RTX2080TI DMA PROBE v1 -- NO GPU WRITES OR FIRMWARE EXECUTION\r\n");
    EFI_STATUS s=find_esp(&esp); if(EFI_ERROR(s)) goto done;
    s=uefi_call_wrapper(RT->GetTime,2,&when,NULL); if(EFI_ERROR(s)) goto done;
    s=open_log(esp,&when); if(EFI_ERROR(s) || EFI_ERROR(log_status)) goto done;
    note(L"Firmware=%s revision=%08x UEFI=%08x\r\n",ST->FirmwareVendor,ST->FirmwareRevision,ST->Hdr.Revision);
    s=dp_target(&p); if(EFI_ERROR(s) || EFI_ERROR(log_status)) goto done;
    s=dp_root(&p); if(EFI_ERROR(s) || EFI_ERROR(log_status)) goto done;
    VOID *iommu=NULL;
    EFI_STATUS observed=uefi_call_wrapper(BS->LocateProtocol,3,&iommu_guid,NULL,&iommu);
    note(L"EDKII IOMMU protocol status=%r present=%d; no permission changes.\r\n",observed,iommu!=NULL);
    if(EFI_ERROR(log_status)) goto done;
    observed=dp_dmar(&p);
    note(L"Read-only ACPI/VT-d observation status=%r; absence is NOT proof DMA is unrestricted.\r\n",observed);
    s=dp_map(&p);
    EFI_STATUS released=dp_release(&p);
    if(EFI_ERROR(released)) s=released;
    observed=dp_unchanged(&p);
    if(EFI_ERROR(observed)) s=observed;
done:
    if(EFI_ERROR(log_status)) s=log_status;
    note(L"DMA probe result=%r. NO GPU WRITES, NO DMA TRANSFER, NO UNLOCK.\r\n",s);
    note(L"Returning to GRUB; no automatic repeat.\r\n");
    if(log_file) { uefi_call_wrapper(log_file->Close,1,log_file); log_file=NULL; }
    uefi_call_wrapper(BS->Stall,1,5000000);
    return EFI_SUCCESS;
}
