### Machine
- Host: Micro-Star International Co., Ltd. B450M-S01 (MS-7B90), bare metal: none
- CPU: AMD Ryzen 5 3600 6-Core Processor, microcode 0x8701034
- CPUs online: 0-5 (SMT off), caches: L1d cache 192 KiB (6 instances); L2 cache 3 MiB (6 instances); L3 cache 32 MiB (2 instances); 
- RAM: 62 GB, 2400 MT/s
- NUMA nodes: 1, NIC enp34s0 on node -1
- NIC: driver r8169; firmware-version rtl8168h-2_0.0.2 02/26/15; 

### OS
- Kernel: 7.0.0-42-generic, Ubuntu 26.04 LTS
- Boot line: BOOT_IMAGE=/vmlinuz-7.0.0-42-generic root=UUID=4f358fce-caf1-43b9-a951-774aba204d13 ro consoleblank=0 systemd.show_status=true isolcpus=managed_irq,domain,1-5,7-11 nohz_full=1-5,7-11 rcu_nocbs=1-5,7-11 irqaffinity=0 nmi_watchdog=0 consoleblank=0
- Isolated CPUs: 1-5,7-11, nohz_full: 1-5,7-11
- Frequency: driver amd-pstate-epp, governor performance, measured 3599 MHz under load on cpu 3
- Idle states enabled: POLL 
- NIC IRQs: irq 87 -> cpu 0; 
- Huge pages: 512 x 2048kB, THP always [madvise] never
- Clock source: tsc
- Mitigations: retbleed: Mitigation: untrained return thunk; SMT disabled | spec_rstack_overflow: Mitigation: Safe RET | spec_store_bypass: Mitigation: Speculative Store Bypass disabled via prctl | spectre_v1: Mitigation: usercopy/swapgs barriers and __user pointer sanitization | spectre_v2: Mitigation: Retpolines; IBPB: conditional; STIBP: always-on; RSB filling; PBRSB-eIBRS: Not affected; BHI: Not affected | vmscape: Mitigation: IBPB before exit to userspace | 

### Build
- Commit: 7908491
- Compiler: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0
- Build type: Release, flags: -DFASTBOOK_AF_XDP -DFASTBOOK_ARCH_AVX2 -DFASTBOOK_N_SHARDS=4 -DFASTBOOK_PLATFORM_LINUX -O3 -DNDEBUG -std=gnu++23 -fno-rtti -mavx2
