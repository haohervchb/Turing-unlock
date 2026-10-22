# Persistence and dual-GPU status

**Not implemented or enabled.** The demonstrated unlock is volatile and was
applied by a one-shot pre-OS experiment. The normal Ubuntu boot remains the
default. This archive must not be presented as an automatic boot installer.

## Intended persistence design

Persistence would mean reapplying the volatile unlock before Linux on each
approved boot, not flashing a VBIOS or resetting a live Linux GPU. A production
wrapper still needs:

1. An explicit enable setting and an accessible normal-Ubuntu bypass.
2. A persistent in-progress/failure marker written successfully **before** any
   GPU mutation. If a run hangs or power is lost, the next boot must skip the
   experiment rather than retry automatically.
3. A same-boot guard so returning through `grub.cfg` cannot repeat the loader.
4. A Linux post-boot verifier that resolves the intended GPU by UUID/identity,
   checks the loader result and issue rates, and only then permits rearming.
5. Offline boot-flow tests, a controlled physical test, and a documented
   disable/recovery procedure before selecting it automatically.

Simply setting `GRUB_DEFAULT=tu102-experiment-v4` is not this design. The
archived entry re-enters `grub.cfg`, whose generated header sets the default
again; this can loop. Its current stop message also assumes a consumed one-shot
and a normal next boot. Preserve that behavior until the replacement is tested.

## The RTX 5060 Ti

The v4 loader uses an EFI PCI handle for `0000:02:00.0` and then checks exact
TU102/ASUS identity (`10de:1e07`, `1043:866a`), BAR0 `fa000000`, PCI command
`0007`, and the expected parent bridge. It does not use NVIDIA's Linux index.
A 5060 Ti in that slot fails the identity guard before the payload is written.

However, adding a GPU can change preboot resource placement, display selection,
command bits, ROM workspace and firmware state. Unchanged Linux BDF is not
sufficient to validate all preboot assumptions. The current strict guards may
correctly refuse the new configuration. They must not be weakened blindly.

The 5060 Ti has been reinstalled in its original slot and ordinary two-card
mining works. Next work is a read-only preboot inventory of that configuration,
followed by a separately authorized bounded 2080 Ti attempt. No 5060 Ti unlock
has been attempted, and TU102 firmware is not interchangeable with SM120.

Linux health checks and tuning must resolve each model/UUID afresh: with both
cards installed, index 0 is the 5060 Ti and index 1 is the 2080 Ti on this host.
