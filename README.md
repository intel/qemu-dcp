Sapphire Rapids Best Known Configuration (BKC) QEMU
=====================================================
https://github.com/intel-innersource/virtualization.hypervisors.server.vmm.qemu-bkc

Purpose
=======
Prepare for releasing public SPR BKC QEMU to external and internal customers.

SPR-BKC-QEMU-v1.9
----------------

14. Backport some upstream bug fix
7e6055c99f2f(tests: bios-tables-test update expected blobs)
211afe5c69b5(hw/i386/acpi-build: Deny control on PCIe Native Hot-plug in _OSC)
be12e3a016f1(bios-tables-test: Allow changes in DSDT ACPI tables)
c318bef76206(hw/acpi/ich9: Add compat prop to keep HPC bit set for 6.1 machine type)
2aa1842d6d79(pcie: rename 'native-hotplug' to 'x-native-hotplug')

SPR-BKC-QEMU-v1.8
----------------

13. TDX: AMX: enable AMX xstate(Yang Zhong)

SPR-BKC-QEMU-v1.7
----------------

12. 0001-i386-cpu-Remove-CPUID_7_0_EDX_ARCH_LBR-if-vPMU-is-di.patch(Yuan Yao)

SPR-BKC-QEMU-v1.6
----------------

10. Revert the AMX QEMU patch and backport a new version(Yang Zhong)
11. A hack to avoid annoying QEMU command work around(Chenyi Qiang)

SPR-BKC-QEMU-v1.5
----------------

9. vIOMMU 5lvl paging support and one bug fix(Yi Liu)

SPR-BKC-QEMU-v1.4
----------------

8. SVM-SIOV support (Yi Liu)

SPR-BKC-QEMU-v1.3
----------------

7. PKS TDX support (Chenyi Qiang)

SPR-BKC-QEMU-v1.2
----------------

5. AMX (Jing Liu)
6. Compile error and TDX fix (Chenyi Qiang)

SPR-BKC-QEMU-v1.1
----------------

4. TDX (Chenyi Qiang)

SPR-BKC-QEMU-v1.0
---------------

1. CET (Weijiang Yang)
2. ARCH-LBR (Weijiang Yang)
2. SGX numa (Yang Zhong)
