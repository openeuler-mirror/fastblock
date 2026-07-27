savedcmd_/root/fastblock-round2/kfastblock/src/pipeline.o := gcc -Wp,-MMD,/root/fastblock-round2/kfastblock/src/.pipeline.o.d -nostdinc -I../arch/x86/include -I./arch/x86/include/generated -I../include -I./include -I../arch/x86/include/uapi -I./arch/x86/include/generated/uapi -I../include/uapi -I./include/generated/uapi -include ../include/linux/compiler-version.h -include ../include/linux/kconfig.h -include ../include/linux/compiler_types.h -D__KERNEL__ -fmacro-prefix-map=../= -Werror -std=gnu11 -fshort-wchar -funsigned-char -fno-common -fno-PIE -fno-strict-aliasing -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx -fcf-protection=branch -fno-jump-tables -m64 -falign-jumps=1 -falign-loops=1 -mno-80387 -mno-fp-ret-in-387 -mpreferred-stack-boundary=3 -mskip-rax-setup -mtune=generic -mno-red-zone -mcmodel=kernel -Wno-sign-compare -fno-asynchronous-unwind-tables -mindirect-branch=thunk-extern -mindirect-branch-register -mindirect-branch-cs-prefix -mfunction-return=thunk-extern -fno-jump-tables -fpatchable-function-entry=16,16 -fno-delete-null-pointer-checks -O2 -fno-allow-store-data-races -fstack-protector-strong -fomit-frame-pointer -ftrivial-auto-var-init=zero -fno-stack-clash-protection -falign-functions=16 -fno-strict-overflow -fno-stack-check -fconserve-stack -fno-builtin-wcslen -Wall -Wundef -Werror=implicit-function-declaration -Werror=implicit-int -Werror=return-type -Werror=strict-prototypes -Wno-format-security -Wno-trigraphs -Wno-frame-address -Wno-address-of-packed-member -Wframe-larger-than=2048 -Wno-main -Wno-unused-but-set-variable -Wno-unused-const-variable -Wno-dangling-pointer -Wvla -Wno-pointer-sign -Wcast-function-type -Wno-array-bounds -Wno-alloc-size-larger-than -Wimplicit-fallthrough=5 -Werror=date-time -Werror=incompatible-pointer-types -Werror=designated-init -Wenum-conversion -Wno-unused-but-set-variable -Wno-unused-const-variable -Wno-restrict -Wno-packed-not-aligned -Wno-format-overflow -Wno-format-truncation -Wno-stringop-overflow -Wno-stringop-truncation -Wno-missing-field-initializers -Wno-type-limits -Wno-shift-negative-value -Wno-maybe-uninitialized -Wno-sign-compare -I/root/fastblock-round2/kfastblock/include  -DMODULE  -DKBUILD_BASENAME='"pipeline"' -DKBUILD_MODNAME='"kfastblock"' -D__KBUILD_MODNAME=kmod_kfastblock -c -o /root/fastblock-round2/kfastblock/src/pipeline.o /root/fastblock-round2/kfastblock/src/pipeline.c  

source_/root/fastblock-round2/kfastblock/src/pipeline.o := /root/fastblock-round2/kfastblock/src/pipeline.c

deps_/root/fastblock-round2/kfastblock/src/pipeline.o := \
  ../include/linux/compiler-version.h \
    $(wildcard include/config/CC_VERSION_TEXT) \
  ../include/linux/kconfig.h \
    $(wildcard include/config/CPU_BIG_ENDIAN) \
    $(wildcard include/config/BOOGER) \
    $(wildcard include/config/FOO) \
  ../include/linux/compiler_types.h \
    $(wildcard include/config/DEBUG_INFO_BTF) \
    $(wildcard include/config/PAHOLE_HAS_BTF_TAG) \
    $(wildcard include/config/FUNCTION_ALIGNMENT) \
    $(wildcard include/config/CC_IS_GCC) \
    $(wildcard include/config/X86_64) \
    $(wildcard include/config/ARM64) \
    $(wildcard include/config/HAVE_ARCH_COMPILER_H) \
    $(wildcard include/config/OPTIMIZE_INLINING) \
    $(wildcard include/config/CC_HAS_COUNTED_BY) \
    $(wildcard include/config/CC_HAS_ASM_INLINE) \
  ../include/linux/compiler_attributes.h \
  ../include/linux/compiler-gcc.h \
    $(wildcard include/config/RETPOLINE) \
    $(wildcard include/config/ARCH_USE_BUILTIN_BSWAP) \
    $(wildcard include/config/SHADOW_CALL_STACK) \
    $(wildcard include/config/KCOV) \
  ../include/linux/jiffies.h \
  ../include/linux/cache.h \
    $(wildcard include/config/SMP) \
    $(wildcard include/config/ARCH_HAS_CACHE_LINE_SIZE) \
  ../include/uapi/linux/kernel.h \
  ../include/uapi/linux/sysinfo.h \
  ../include/linux/types.h \
    $(wildcard include/config/HAVE_UID16) \
    $(wildcard include/config/UID16) \
    $(wildcard include/config/ARCH_DMA_ADDR_T_64BIT) \
    $(wildcard include/config/PHYS_ADDR_T_64BIT) \
    $(wildcard include/config/64BIT) \
    $(wildcard include/config/ARCH_32BIT_USTAT_F_TINODE) \
  ../include/uapi/linux/types.h \
  arch/x86/include/generated/uapi/asm/types.h \
  ../include/uapi/asm-generic/types.h \
  ../include/asm-generic/int-ll64.h \
  ../include/uapi/asm-generic/int-ll64.h \
  ../arch/x86/include/uapi/asm/bitsperlong.h \
  ../include/asm-generic/bitsperlong.h \
  ../include/uapi/asm-generic/bitsperlong.h \
  ../include/uapi/linux/posix_types.h \
  ../include/linux/stddef.h \
  ../include/uapi/linux/stddef.h \
  ../arch/x86/include/asm/posix_types.h \
    $(wildcard include/config/X86_32) \
  ../arch/x86/include/uapi/asm/posix_types_64.h \
  ../include/uapi/asm-generic/posix_types.h \
  ../include/linux/const.h \
  ../include/vdso/const.h \
  ../include/uapi/linux/const.h \
  ../arch/x86/include/asm/cache.h \
    $(wildcard include/config/X86_L1_CACHE_SHIFT) \
    $(wildcard include/config/X86_INTERNODE_CACHE_SHIFT) \
    $(wildcard include/config/X86_VSMP) \
  ../include/linux/linkage.h \
    $(wildcard include/config/ARCH_USE_SYM_ANNOTATIONS) \
  ../include/linux/stringify.h \
  ../include/linux/export.h \
    $(wildcard include/config/MODVERSIONS) \
  ../include/linux/compiler.h \
    $(wildcard include/config/TRACE_BRANCH_PROFILING) \
    $(wildcard include/config/PROFILE_ALL_BRANCHES) \
    $(wildcard include/config/OBJTOOL) \
  arch/x86/include/generated/asm/rwonce.h \
  ../include/asm-generic/rwonce.h \
  ../include/linux/kasan-checks.h \
    $(wildcard include/config/KASAN_GENERIC) \
    $(wildcard include/config/KASAN_SW_TAGS) \
  ../include/linux/kcsan-checks.h \
    $(wildcard include/config/KCSAN) \
    $(wildcard include/config/KCSAN_WEAK_MEMORY) \
    $(wildcard include/config/KCSAN_IGNORE_ATOMICS) \
  ../arch/x86/include/asm/linkage.h \
    $(wildcard include/config/CALL_PADDING) \
    $(wildcard include/config/RETHUNK) \
    $(wildcard include/config/SLS) \
    $(wildcard include/config/FUNCTION_PADDING_BYTES) \
    $(wildcard include/config/UML) \
  ../arch/x86/include/asm/ibt.h \
    $(wildcard include/config/X86_KERNEL_IBT) \
  ../include/linux/limits.h \
  ../include/uapi/linux/limits.h \
  ../include/vdso/limits.h \
  ../include/linux/math64.h \
    $(wildcard include/config/ARCH_SUPPORTS_INT128) \
  ../include/linux/math.h \
  ../arch/x86/include/asm/div64.h \
  ../include/asm-generic/div64.h \
  ../include/vdso/math64.h \
  ../include/linux/minmax.h \
  ../include/linux/build_bug.h \
  ../include/linux/time.h \
    $(wildcard include/config/POSIX_TIMERS) \
  ../include/linux/time64.h \
  ../include/vdso/time64.h \
  ../include/uapi/linux/time.h \
  ../include/uapi/linux/time_types.h \
  ../include/linux/time32.h \
  ../include/linux/timex.h \
  ../include/uapi/linux/timex.h \
  ../include/uapi/linux/param.h \
  arch/x86/include/generated/uapi/asm/param.h \
  ../include/asm-generic/param.h \
    $(wildcard include/config/HZ) \
  ../include/uapi/asm-generic/param.h \
  ../arch/x86/include/asm/timex.h \
    $(wildcard include/config/X86_TSC) \
  ../arch/x86/include/asm/processor.h \
    $(wildcard include/config/X86_VMX_FEATURE_NAMES) \
    $(wildcard include/config/X86_IOPL_IOPERM) \
    $(wildcard include/config/STACKPROTECTOR) \
    $(wildcard include/config/VM86) \
    $(wildcard include/config/X86_USER_SHADOW_STACK) \
    $(wildcard include/config/PARAVIRT_XXL) \
    $(wildcard include/config/X86_DEBUGCTLMSR) \
    $(wildcard include/config/CPU_SUP_AMD) \
    $(wildcard include/config/XEN) \
    $(wildcard include/config/X86_SGX) \
  ../arch/x86/include/asm/processor-flags.h \
    $(wildcard include/config/PAGE_TABLE_ISOLATION) \
  ../arch/x86/include/uapi/asm/processor-flags.h \
  ../include/linux/mem_encrypt.h \
    $(wildcard include/config/ARCH_HAS_MEM_ENCRYPT) \
    $(wildcard include/config/AMD_MEM_ENCRYPT) \
  ../arch/x86/include/asm/mem_encrypt.h \
    $(wildcard include/config/X86_MEM_ENCRYPT) \
    $(wildcard include/config/HYGON_CSV) \
  ../include/linux/init.h \
    $(wildcard include/config/MEMORY_HOTPLUG) \
    $(wildcard include/config/HAVE_ARCH_PREL32_RELOCATIONS) \
    $(wildcard include/config/STRICT_KERNEL_RWX) \
    $(wildcard include/config/STRICT_MODULE_RWX) \
    $(wildcard include/config/LTO_CLANG) \
  ../include/linux/cc_platform.h \
    $(wildcard include/config/ARCH_HAS_CC_PLATFORM) \
  ../arch/x86/include/asm/asm.h \
    $(wildcard include/config/KPROBES) \
  ../arch/x86/include/asm/extable_fixup_types.h \
  ../arch/x86/include/asm/math_emu.h \
  ../arch/x86/include/asm/ptrace.h \
    $(wildcard include/config/PARAVIRT) \
    $(wildcard include/config/IA32_EMULATION) \
  ../arch/x86/include/asm/segment.h \
    $(wildcard include/config/XEN_PV) \
  ../arch/x86/include/asm/alternative.h \
    $(wildcard include/config/CALL_THUNKS) \
  ../arch/x86/include/asm/page_types.h \
    $(wildcard include/config/PHYSICAL_START) \
    $(wildcard include/config/PHYSICAL_ALIGN) \
    $(wildcard include/config/DYNAMIC_PHYSICAL_MASK) \
  ../arch/x86/include/asm/page_64_types.h \
    $(wildcard include/config/KASAN) \
    $(wildcard include/config/DYNAMIC_MEMORY_LAYOUT) \
    $(wildcard include/config/X86_5LEVEL) \
    $(wildcard include/config/RANDOMIZE_BASE) \
  ../arch/x86/include/asm/kaslr.h \
    $(wildcard include/config/RANDOMIZE_MEMORY) \
  ../arch/x86/include/uapi/asm/ptrace.h \
  ../arch/x86/include/uapi/asm/ptrace-abi.h \
  ../include/linux/kabi.h \
    $(wildcard include/config/KABI_COMPAT) \
    $(wildcard include/config/KABI_SIZE_ALIGN_CHECKS) \
    $(wildcard include/config/KABI_RESERVE) \
  ../arch/x86/include/asm/paravirt_types.h \
    $(wildcard include/config/PGTABLE_LEVELS) \
    $(wildcard include/config/ZERO_CALL_USED_REGS) \
    $(wildcard include/config/PARAVIRT_DEBUG) \
  ../arch/x86/include/asm/desc_defs.h \
  ../arch/x86/include/asm/pgtable_types.h \
    $(wildcard include/config/X86_INTEL_MEMORY_PROTECTION_KEYS) \
    $(wildcard include/config/X86_PAE) \
    $(wildcard include/config/MEM_SOFT_DIRTY) \
    $(wildcard include/config/HAVE_ARCH_USERFAULTFD_WP) \
    $(wildcard include/config/PROC_FS) \
  ../arch/x86/include/asm/pgtable_64_types.h \
    $(wildcard include/config/KMSAN) \
    $(wildcard include/config/DEBUG_KMAP_LOCAL_FORCE_MAP) \
  ../arch/x86/include/asm/sparsemem.h \
    $(wildcard include/config/SPARSEMEM) \
    $(wildcard include/config/NUMA_KEEP_MEMINFO) \
  ../arch/x86/include/asm/nospec-branch.h \
    $(wildcard include/config/CALL_THUNKS_DEBUG) \
    $(wildcard include/config/CALL_DEPTH_TRACKING) \
    $(wildcard include/config/NOINSTR_VALIDATION) \
    $(wildcard include/config/CPU_UNRET_ENTRY) \
    $(wildcard include/config/CPU_SRSO) \
    $(wildcard include/config/CPU_IBPB_ENTRY) \
  ../include/linux/static_key.h \
  ../include/linux/jump_label.h \
    $(wildcard include/config/JUMP_LABEL) \
    $(wildcard include/config/HAVE_ARCH_JUMP_LABEL_RELATIVE) \
    $(wildcard include/config/LIVEPATCH_WO_FTRACE) \
  ../arch/x86/include/asm/jump_label.h \
    $(wildcard include/config/HAVE_JUMP_LABEL_HACK) \
  ../arch/x86/include/asm/nops.h \
  ../include/linux/objtool.h \
    $(wildcard include/config/FRAME_POINTER) \
  ../include/linux/objtool_types.h \
  ../arch/x86/include/asm/cpufeatures.h \
  ../arch/x86/include/asm/required-features.h \
    $(wildcard include/config/X86_MINIMUM_CPU_FAMILY) \
    $(wildcard include/config/MATH_EMULATION) \
    $(wildcard include/config/X86_CMPXCHG64) \
    $(wildcard include/config/X86_CMOV) \
    $(wildcard include/config/X86_P6_NOP) \
    $(wildcard include/config/MATOM) \
  ../arch/x86/include/asm/disabled-features.h \
    $(wildcard include/config/X86_UMIP) \
    $(wildcard include/config/ADDRESS_MASKING) \
    $(wildcard include/config/INTEL_IOMMU_SVM) \
    $(wildcard include/config/INTEL_TDX_GUEST) \
  ../arch/x86/include/asm/msr-index.h \
  ../include/linux/bits.h \
  ../include/vdso/bits.h \
  ../arch/x86/include/asm/unwind_hints.h \
  ../arch/x86/include/asm/orc_types.h \
  ../arch/x86/include/uapi/asm/byteorder.h \
  ../include/linux/byteorder/little_endian.h \
  ../include/uapi/linux/byteorder/little_endian.h \
  ../include/linux/swab.h \
  ../include/uapi/linux/swab.h \
  ../arch/x86/include/uapi/asm/swab.h \
  ../include/linux/byteorder/generic.h \
  ../arch/x86/include/asm/percpu.h \
    $(wildcard include/config/X86_64_SMP) \
  ../include/linux/kernel.h \
    $(wildcard include/config/PREEMPT_VOLUNTARY_BUILD) \
    $(wildcard include/config/PREEMPT_DYNAMIC) \
    $(wildcard include/config/HAVE_PREEMPT_DYNAMIC_CALL) \
    $(wildcard include/config/HAVE_PREEMPT_DYNAMIC_KEY) \
    $(wildcard include/config/PREEMPT_) \
    $(wildcard include/config/DEBUG_ATOMIC_SLEEP) \
    $(wildcard include/config/MMU) \
    $(wildcard include/config/PROVE_LOCKING) \
    $(wildcard include/config/TRACING) \
    $(wildcard include/config/FTRACE_MCOUNT_RECORD) \
  ../include/linux/stdarg.h \
  ../include/linux/align.h \
  ../include/linux/array_size.h \
  ../include/linux/container_of.h \
  ../include/linux/bitops.h \
  ../include/linux/typecheck.h \
  ../include/asm-generic/bitops/generic-non-atomic.h \
  ../arch/x86/include/asm/barrier.h \
  ../include/asm-generic/barrier.h \
  ../arch/x86/include/asm/bitops.h \
  ../arch/x86/include/asm/rmwcc.h \
  ../include/linux/args.h \
  ../include/asm-generic/bitops/sched.h \
  ../arch/x86/include/asm/arch_hweight.h \
  ../include/asm-generic/bitops/const_hweight.h \
  ../include/asm-generic/bitops/instrumented-atomic.h \
  ../include/linux/instrumented.h \
  ../include/linux/kmsan-checks.h \
  ../include/asm-generic/bitops/instrumented-non-atomic.h \
    $(wildcard include/config/KCSAN_ASSUME_PLAIN_WRITES_ATOMIC) \
  ../include/asm-generic/bitops/instrumented-lock.h \
  ../include/asm-generic/bitops/le.h \
  ../include/asm-generic/bitops/ext2-atomic-setbit.h \
  ../include/linux/hex.h \
  ../include/linux/kstrtox.h \
  ../include/linux/log2.h \
    $(wildcard include/config/ARCH_HAS_ILOG2_U32) \
    $(wildcard include/config/ARCH_HAS_ILOG2_U64) \
  ../include/linux/panic.h \
    $(wildcard include/config/PANIC_TIMEOUT) \
  ../include/linux/printk.h \
    $(wildcard include/config/MESSAGE_LOGLEVEL_DEFAULT) \
    $(wildcard include/config/CONSOLE_LOGLEVEL_DEFAULT) \
    $(wildcard include/config/CONSOLE_LOGLEVEL_QUIET) \
    $(wildcard include/config/EARLY_PRINTK) \
    $(wildcard include/config/PRINTK) \
    $(wildcard include/config/PRINTK_INDEX) \
    $(wildcard include/config/DYNAMIC_DEBUG) \
    $(wildcard include/config/DYNAMIC_DEBUG_CORE) \
  ../include/linux/kern_levels.h \
  ../include/linux/ratelimit_types.h \
  ../include/linux/spinlock_types_raw.h \
    $(wildcard include/config/DEBUG_SPINLOCK) \
    $(wildcard include/config/DEBUG_LOCK_ALLOC) \
  ../arch/x86/include/asm/spinlock_types.h \
  ../include/asm-generic/qspinlock_types.h \
    $(wildcard include/config/NR_CPUS) \
  ../include/asm-generic/qrwlock_types.h \
  ../include/linux/lockdep_types.h \
    $(wildcard include/config/PROVE_RAW_LOCK_NESTING) \
    $(wildcard include/config/LOCKDEP) \
    $(wildcard include/config/LOCK_STAT) \
  ../include/linux/once_lite.h \
  ../include/linux/sprintf.h \
  ../include/linux/static_call_types.h \
    $(wildcard include/config/HAVE_STATIC_CALL) \
    $(wildcard include/config/HAVE_STATIC_CALL_INLINE) \
  ../include/linux/instruction_pointer.h \
  ../include/asm-generic/percpu.h \
    $(wildcard include/config/DEBUG_PREEMPT) \
    $(wildcard include/config/HAVE_SETUP_PER_CPU_AREA) \
  ../include/linux/threads.h \
    $(wildcard include/config/BASE_SMALL) \
  ../include/linux/percpu-defs.h \
    $(wildcard include/config/DEBUG_FORCE_WEAK_PER_CPU) \
  ../arch/x86/include/asm/current.h \
  ../arch/x86/include/asm/asm-offsets.h \
  include/generated/asm-offsets.h \
  ../arch/x86/include/asm/GEN-for-each-reg.h \
  ../arch/x86/include/asm/proto.h \
  ../arch/x86/include/uapi/asm/ldt.h \
  ../arch/x86/include/uapi/asm/sigcontext.h \
  ../arch/x86/include/asm/cpuid.h \
  ../arch/x86/include/asm/string.h \
  ../arch/x86/include/asm/string_64.h \
    $(wildcard include/config/ARCH_HAS_UACCESS_FLUSHCACHE) \
  ../arch/x86/include/asm/page.h \
  ../arch/x86/include/asm/page_64.h \
    $(wildcard include/config/DEBUG_VIRTUAL) \
    $(wildcard include/config/X86_VSYSCALL_EMULATION) \
  ../include/linux/range.h \
  ../include/asm-generic/memory_model.h \
    $(wildcard include/config/FLATMEM) \
    $(wildcard include/config/SPARSEMEM_VMEMMAP) \
  ../include/linux/pfn.h \
  ../include/asm-generic/getorder.h \
  ../arch/x86/include/asm/msr.h \
    $(wildcard include/config/TRACEPOINTS) \
  arch/x86/include/generated/uapi/asm/errno.h \
  ../include/uapi/asm-generic/errno.h \
  ../include/uapi/asm-generic/errno-base.h \
  ../arch/x86/include/asm/cpumask.h \
  ../include/linux/cpumask.h \
    $(wildcard include/config/FORCE_NR_CPUS) \
    $(wildcard include/config/HOTPLUG_CPU) \
    $(wildcard include/config/DEBUG_PER_CPU_MAPS) \
    $(wildcard include/config/CPUMASK_OFFSTACK) \
  ../include/linux/bitmap.h \
  ../include/linux/cleanup.h \
  ../include/linux/find.h \
  ../include/linux/string.h \
    $(wildcard include/config/BINARY_PRINTF) \
    $(wildcard include/config/FORTIFY_SOURCE) \
  ../include/linux/err.h \
  ../include/linux/errno.h \
  ../include/uapi/linux/errno.h \
  ../include/linux/overflow.h \
  ../include/uapi/linux/string.h \
  ../include/linux/atomic.h \
  ../arch/x86/include/asm/atomic.h \
  ../arch/x86/include/asm/cmpxchg.h \
  ../arch/x86/include/asm/cmpxchg_64.h \
  ../arch/x86/include/asm/atomic64_64.h \
  ../include/linux/atomic/atomic-arch-fallback.h \
    $(wildcard include/config/GENERIC_ATOMIC64) \
  ../include/linux/atomic/atomic-long.h \
  ../include/linux/atomic/atomic-instrumented.h \
  ../include/linux/bug.h \
    $(wildcard include/config/GENERIC_BUG) \
    $(wildcard include/config/BUG_ON_DATA_CORRUPTION) \
  ../arch/x86/include/asm/bug.h \
    $(wildcard include/config/DEBUG_BUGVERBOSE) \
  ../include/linux/instrumentation.h \
  ../include/asm-generic/bug.h \
    $(wildcard include/config/BUG) \
    $(wildcard include/config/GENERIC_BUG_RELATIVE_POINTERS) \
  ../include/linux/gfp_types.h \
    $(wildcard include/config/MEMORY_RELIABLE) \
    $(wildcard include/config/KASAN_HW_TAGS) \
  ../include/linux/numa.h \
    $(wildcard include/config/NODES_SHIFT) \
    $(wildcard include/config/NUMA) \
    $(wildcard include/config/HAVE_ARCH_NODE_DEV_GROUP) \
  ../arch/x86/include/uapi/asm/msr.h \
  ../include/uapi/linux/ioctl.h \
  arch/x86/include/generated/uapi/asm/ioctl.h \
  ../include/asm-generic/ioctl.h \
  ../include/uapi/asm-generic/ioctl.h \
  ../arch/x86/include/asm/shared/msr.h \
  ../include/linux/tracepoint-defs.h \
  ../arch/x86/include/asm/special_insns.h \
  ../include/linux/irqflags.h \
    $(wildcard include/config/TRACE_IRQFLAGS) \
    $(wildcard include/config/PREEMPT_RT) \
    $(wildcard include/config/IRQSOFF_TRACER) \
    $(wildcard include/config/PREEMPT_TRACER) \
    $(wildcard include/config/DEBUG_IRQFLAGS) \
    $(wildcard include/config/TRACE_IRQFLAGS_SUPPORT) \
  ../arch/x86/include/asm/irqflags.h \
    $(wildcard include/config/DEBUG_ENTRY) \
  ../arch/x86/include/asm/fpu/types.h \
  ../arch/x86/include/asm/vmxfeatures.h \
  ../arch/x86/include/asm/vdso/processor.h \
  ../arch/x86/include/asm/shstk.h \
  ../include/linux/personality.h \
  ../include/uapi/linux/personality.h \
  ../arch/x86/include/asm/tsc.h \
  ../arch/x86/include/asm/cpufeature.h \
  ../include/vdso/time32.h \
  ../include/vdso/time.h \
  ../include/vdso/jiffies.h \
  include/generated/timeconst.h \
  ../include/linux/slab.h \
    $(wildcard include/config/DEBUG_SLAB) \
    $(wildcard include/config/DEBUG_OBJECTS) \
    $(wildcard include/config/SLUB_TINY) \
    $(wildcard include/config/FAILSLAB) \
    $(wildcard include/config/MEMCG_KMEM) \
    $(wildcard include/config/KFENCE) \
    $(wildcard include/config/SLAB) \
    $(wildcard include/config/SLUB) \
    $(wildcard include/config/RANDOM_KMALLOC_CACHES) \
    $(wildcard include/config/ZONE_DMA) \
  ../include/linux/gfp.h \
    $(wildcard include/config/HIGHMEM) \
    $(wildcard include/config/ZONE_DMA32) \
    $(wildcard include/config/ZONE_DEVICE) \
    $(wildcard include/config/COMPACTION) \
    $(wildcard include/config/CONTIG_ALLOC) \
  ../include/linux/mmzone.h \
    $(wildcard include/config/ARCH_FORCE_MAX_ORDER) \
    $(wildcard include/config/CMA) \
    $(wildcard include/config/MEMORY_ISOLATION) \
    $(wildcard include/config/ZSMALLOC) \
    $(wildcard include/config/UNACCEPTED_MEMORY) \
    $(wildcard include/config/SWAP) \
    $(wildcard include/config/NUMA_BALANCING) \
    $(wildcard include/config/TRANSPARENT_HUGEPAGE) \
    $(wildcard include/config/LRU_GEN) \
    $(wildcard include/config/LRU_GEN_STATS) \
    $(wildcard include/config/MEMCG) \
    $(wildcard include/config/ZONE_EXTMEM) \
    $(wildcard include/config/MEMORY_FAILURE) \
    $(wildcard include/config/PAGE_EXTENSION) \
    $(wildcard include/config/DEFERRED_STRUCT_PAGE_INIT) \
    $(wildcard include/config/HAVE_MEMORYLESS_NODES) \
    $(wildcard include/config/SPARSEMEM_EXTREME) \
    $(wildcard include/config/HAVE_ARCH_PFN_VALID) \
  ../include/linux/spinlock.h \
    $(wildcard include/config/PREEMPTION) \
  ../include/linux/preempt.h \
    $(wildcard include/config/PREEMPT_COUNT) \
    $(wildcard include/config/TRACE_PREEMPT_TOGGLE) \
    $(wildcard include/config/PREEMPT_NOTIFIERS) \
  ../include/linux/list.h \
    $(wildcard include/config/LIST_HARDENED) \
    $(wildcard include/config/DEBUG_LIST) \
  ../include/linux/poison.h \
    $(wildcard include/config/ILLEGAL_POINTER_VALUE) \
  ../arch/x86/include/asm/preempt.h \
  ../include/linux/thread_info.h \
    $(wildcard include/config/GENERIC_ENTRY) \
    $(wildcard include/config/HAVE_ARCH_WITHIN_STACK_FRAMES) \
    $(wildcard include/config/HARDENED_USERCOPY) \
    $(wildcard include/config/SH) \
  ../include/linux/restart_block.h \
  ../include/linux/thread_bits.h \
    $(wildcard include/config/THREAD_INFO_IN_TASK) \
  ../arch/x86/include/asm/thread_info.h \
    $(wildcard include/config/COMPAT) \
  ../include/linux/bottom_half.h \
  ../include/linux/lockdep.h \
    $(wildcard include/config/DEBUG_LOCKING_API_SELFTESTS) \
  ../include/linux/smp.h \
    $(wildcard include/config/UP_LATE_INIT) \
  ../include/linux/smp_types.h \
  ../include/linux/llist.h \
    $(wildcard include/config/ARCH_HAVE_NMI_SAFE_CMPXCHG) \
  ../arch/x86/include/asm/smp.h \
    $(wildcard include/config/DEBUG_NMI_SELFTEST) \
  arch/x86/include/generated/asm/mmiowb.h \
  ../include/asm-generic/mmiowb.h \
    $(wildcard include/config/MMIOWB) \
  ../include/linux/spinlock_types.h \
  ../include/linux/rwlock_types.h \
  ../arch/x86/include/asm/spinlock.h \
  ../arch/x86/include/asm/paravirt.h \
    $(wildcard include/config/PARAVIRT_SPINLOCKS) \
  ../arch/x86/include/asm/frame.h \
  ../arch/x86/include/asm/qspinlock.h \
    $(wildcard include/config/NUMA_AWARE_SPINLOCKS) \
  ../include/asm-generic/qspinlock.h \
  ../arch/x86/include/asm/qrwlock.h \
  ../include/asm-generic/qrwlock.h \
  ../include/linux/rwlock.h \
    $(wildcard include/config/PREEMPT) \
  ../include/linux/spinlock_api_smp.h \
    $(wildcard include/config/INLINE_SPIN_LOCK) \
    $(wildcard include/config/INLINE_SPIN_LOCK_BH) \
    $(wildcard include/config/INLINE_SPIN_LOCK_IRQ) \
    $(wildcard include/config/INLINE_SPIN_LOCK_IRQSAVE) \
    $(wildcard include/config/INLINE_SPIN_TRYLOCK) \
    $(wildcard include/config/INLINE_SPIN_TRYLOCK_BH) \
    $(wildcard include/config/UNINLINE_SPIN_UNLOCK) \
    $(wildcard include/config/INLINE_SPIN_UNLOCK_BH) \
    $(wildcard include/config/INLINE_SPIN_UNLOCK_IRQ) \
    $(wildcard include/config/INLINE_SPIN_UNLOCK_IRQRESTORE) \
    $(wildcard include/config/GENERIC_LOCKBREAK) \
  ../include/linux/rwlock_api_smp.h \
    $(wildcard include/config/INLINE_READ_LOCK) \
    $(wildcard include/config/INLINE_WRITE_LOCK) \
    $(wildcard include/config/INLINE_READ_LOCK_BH) \
    $(wildcard include/config/INLINE_WRITE_LOCK_BH) \
    $(wildcard include/config/INLINE_READ_LOCK_IRQ) \
    $(wildcard include/config/INLINE_WRITE_LOCK_IRQ) \
    $(wildcard include/config/INLINE_READ_LOCK_IRQSAVE) \
    $(wildcard include/config/INLINE_WRITE_LOCK_IRQSAVE) \
    $(wildcard include/config/INLINE_READ_TRYLOCK) \
    $(wildcard include/config/INLINE_WRITE_TRYLOCK) \
    $(wildcard include/config/INLINE_READ_UNLOCK) \
    $(wildcard include/config/INLINE_WRITE_UNLOCK) \
    $(wildcard include/config/INLINE_READ_UNLOCK_BH) \
    $(wildcard include/config/INLINE_WRITE_UNLOCK_BH) \
    $(wildcard include/config/INLINE_READ_UNLOCK_IRQ) \
    $(wildcard include/config/INLINE_WRITE_UNLOCK_IRQ) \
    $(wildcard include/config/INLINE_READ_UNLOCK_IRQRESTORE) \
    $(wildcard include/config/INLINE_WRITE_UNLOCK_IRQRESTORE) \
  ../include/linux/list_nulls.h \
  ../include/linux/wait.h \
  ../include/uapi/linux/wait.h \
  ../include/linux/seqlock.h \
  ../include/linux/mutex.h \
    $(wildcard include/config/MUTEX_SPIN_ON_OWNER) \
    $(wildcard include/config/DEBUG_MUTEXES) \
  ../include/linux/osq_lock.h \
  ../include/linux/debug_locks.h \
  ../include/linux/nodemask.h \
  ../include/linux/random.h \
    $(wildcard include/config/VMGENID) \
  ../include/uapi/linux/random.h \
  ../include/linux/irqnr.h \
  ../include/uapi/linux/irqnr.h \
  ../include/linux/prandom.h \
  ../include/linux/once.h \
  ../include/linux/percpu.h \
    $(wildcard include/config/MODULES) \
    $(wildcard include/config/NEED_PER_CPU_PAGE_FIRST_CHUNK) \
  ../include/linux/mmdebug.h \
    $(wildcard include/config/DEBUG_VM) \
    $(wildcard include/config/DEBUG_VM_IRQSOFF) \
    $(wildcard include/config/DEBUG_VM_PGFLAGS) \
  ../include/linux/pageblock-flags.h \
    $(wildcard include/config/HUGETLB_PAGE) \
    $(wildcard include/config/HUGETLB_PAGE_SIZE_VARIABLE) \
  ../include/linux/page-flags-layout.h \
  include/generated/bounds.h \
  ../include/linux/mm_types.h \
    $(wildcard include/config/HAVE_ALIGNED_STRUCT_PAGE) \
    $(wildcard include/config/HUGETLB_PMD_PAGE_TABLE_SHARING) \
    $(wildcard include/config/USERFAULTFD) \
    $(wildcard include/config/PER_VMA_LOCK) \
    $(wildcard include/config/ANON_VMA_NAME) \
    $(wildcard include/config/SHARE_POOL) \
    $(wildcard include/config/GMEM) \
    $(wildcard include/config/HAVE_ARCH_COMPAT_MMAP_BASES) \
    $(wildcard include/config/MEMBARRIER) \
    $(wildcard include/config/SCHED_MM_CID) \
    $(wildcard include/config/AIO) \
    $(wildcard include/config/MMU_NOTIFIER) \
    $(wildcard include/config/ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH) \
    $(wildcard include/config/IOMMU_MM_DATA) \
    $(wildcard include/config/KSM) \
    $(wildcard include/config/KVM) \
    $(wildcard include/config/DAMON_MEM_SAMPLING) \
    $(wildcard include/config/DYNAMIC_XCALL) \
  ../include/linux/mm_types_task.h \
    $(wildcard include/config/SPLIT_PTLOCK_CPUS) \
    $(wildcard include/config/ARCH_ENABLE_SPLIT_PMD_PTLOCK) \
  ../arch/x86/include/asm/tlbbatch.h \
  ../include/linux/auxvec.h \
  ../include/uapi/linux/auxvec.h \
  ../arch/x86/include/uapi/asm/auxvec.h \
  ../include/linux/kref.h \
  ../include/linux/refcount.h \
  ../include/linux/rbtree.h \
  ../include/linux/rbtree_types.h \
  ../include/linux/rcupdate.h \
    $(wildcard include/config/PREEMPT_RCU) \
    $(wildcard include/config/TINY_RCU) \
    $(wildcard include/config/RCU_STRICT_GRACE_PERIOD) \
    $(wildcard include/config/RCU_LAZY) \
    $(wildcard include/config/TASKS_RCU_GENERIC) \
    $(wildcard include/config/RCU_STALL_COMMON) \
    $(wildcard include/config/NO_HZ_FULL) \
    $(wildcard include/config/KVM_XFER_TO_GUEST_WORK) \
    $(wildcard include/config/RCU_NOCB_CPU) \
    $(wildcard include/config/TASKS_RCU) \
    $(wildcard include/config/TASKS_TRACE_RCU) \
    $(wildcard include/config/TASKS_RUDE_RCU) \
    $(wildcard include/config/TREE_RCU) \
    $(wildcard include/config/DEBUG_OBJECTS_RCU_HEAD) \
    $(wildcard include/config/PROVE_RCU) \
    $(wildcard include/config/ARCH_WEAK_RELEASE_ACQUIRE) \
  ../include/linux/context_tracking_irq.h \
    $(wildcard include/config/CONTEXT_TRACKING_IDLE) \
  ../include/linux/rcutree.h \
  ../include/linux/maple_tree.h \
    $(wildcard include/config/MAPLE_RCU_DISABLED) \
    $(wildcard include/config/DEBUG_MAPLE_TREE) \
  ../include/linux/rwsem.h \
    $(wildcard include/config/RWSEM_SPIN_ON_OWNER) \
    $(wildcard include/config/DEBUG_RWSEMS) \
  ../include/linux/completion.h \
  ../include/linux/swait.h \
  ../include/linux/uprobes.h \
    $(wildcard include/config/UPROBES) \
  ../arch/x86/include/asm/uprobes.h \
  ../include/linux/notifier.h \
    $(wildcard include/config/TREE_SRCU) \
  ../include/linux/srcu.h \
    $(wildcard include/config/TINY_SRCU) \
    $(wildcard include/config/NEED_SRCU_NMI_SAFE) \
  ../include/linux/workqueue.h \
    $(wildcard include/config/DEBUG_OBJECTS_WORK) \
    $(wildcard include/config/FREEZER) \
    $(wildcard include/config/SYSFS) \
    $(wildcard include/config/WQ_WATCHDOG) \
  ../include/linux/timer.h \
    $(wildcard include/config/DEBUG_OBJECTS_TIMERS) \
  ../include/linux/ktime.h \
  ../include/vdso/ktime.h \
  ../include/linux/timekeeping.h \
    $(wildcard include/config/GENERIC_CMOS_UPDATE) \
  ../include/linux/clocksource_ids.h \
  ../include/linux/debugobjects.h \
    $(wildcard include/config/DEBUG_OBJECTS_FREE) \
  ../include/linux/rcu_segcblist.h \
  ../include/linux/srcutree.h \
  ../include/linux/rcu_node_tree.h \
    $(wildcard include/config/RCU_FANOUT) \
    $(wildcard include/config/RCU_FANOUT_LEAF) \
  ../include/linux/percpu_counter.h \
  ../arch/x86/include/asm/mmu.h \
    $(wildcard include/config/MODIFY_LDT_SYSCALL) \
  ../include/linux/page-flags.h \
    $(wildcard include/config/ARCH_USES_PG_UNCACHED) \
    $(wildcard include/config/PAGE_IDLE_FLAG) \
    $(wildcard include/config/ARCH_USES_PG_ARCH_X) \
    $(wildcard include/config/DYNAMIC_POOL) \
    $(wildcard include/config/PSWIOTLB) \
    $(wildcard include/config/HUGETLB_PAGE_OPTIMIZE_VMEMMAP) \
  ../include/linux/local_lock.h \
  ../include/linux/local_lock_internal.h \
  ../include/linux/zswap.h \
    $(wildcard include/config/ZSWAP) \
  ../include/linux/memory_hotplug.h \
    $(wildcard include/config/HAVE_ARCH_NODEDATA_EXTENSION) \
    $(wildcard include/config/HISI_HBMDEV) \
    $(wildcard include/config/ARCH_HAS_ADD_PAGES) \
    $(wildcard include/config/MEMORY_HOTREMOVE) \
  ../arch/x86/include/asm/mmzone.h \
  ../arch/x86/include/asm/mmzone_64.h \
  ../include/linux/topology.h \
    $(wildcard include/config/USE_PERCPU_NUMA_NODE_ID) \
    $(wildcard include/config/SCHED_SMT) \
    $(wildcard include/config/GENERIC_ARCH_TOPOLOGY) \
  ../include/linux/arch_topology.h \
    $(wildcard include/config/ACPI_CPPC_LIB) \
  ../arch/x86/include/asm/topology.h \
    $(wildcard include/config/SCHED_MC_PRIO) \
  ../arch/x86/include/asm/mpspec.h \
    $(wildcard include/config/EISA) \
    $(wildcard include/config/X86_LOCAL_APIC) \
    $(wildcard include/config/X86_MPPARSE) \
  ../arch/x86/include/asm/mpspec_def.h \
  ../arch/x86/include/asm/x86_init.h \
  ../arch/x86/include/uapi/asm/bootparam.h \
  ../include/linux/screen_info.h \
    $(wildcard include/config/PCI) \
  ../include/uapi/linux/screen_info.h \
  ../include/linux/apm_bios.h \
  ../include/uapi/linux/apm_bios.h \
  ../include/linux/edd.h \
  ../include/uapi/linux/edd.h \
  ../arch/x86/include/asm/ist.h \
  ../arch/x86/include/uapi/asm/ist.h \
  ../include/video/edid.h \
    $(wildcard include/config/X86) \
  ../include/uapi/video/edid.h \
  ../arch/x86/include/asm/apicdef.h \
  ../include/asm-generic/topology.h \
  ../include/linux/cpu_smt.h \
    $(wildcard include/config/HOTPLUG_SMT) \
  ../include/linux/percpu-refcount.h \
  ../include/linux/hash.h \
    $(wildcard include/config/HAVE_ARCH_HASH) \
  ../include/linux/kasan.h \
    $(wildcard include/config/KASAN_STACK) \
    $(wildcard include/config/KASAN_VMALLOC) \
  ../include/linux/kasan-enabled.h \
  /root/fastblock-round2/kfastblock/include/kfastblock/pipeline.h \

/root/fastblock-round2/kfastblock/src/pipeline.o: $(deps_/root/fastblock-round2/kfastblock/src/pipeline.o)

$(deps_/root/fastblock-round2/kfastblock/src/pipeline.o):
