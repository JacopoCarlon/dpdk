# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation

if arch_subdir == 'loongarch'
    build = false
    reason = 'not supported on LoongArch'
    subdir_done()
endif

if arch_subdir == 'riscv'
    build = false
    reason = 'not supported on RISC-V'
    subdir_done()
endif

if dpdk_conf.get('RTE_IOVA_IN_MBUF') == 0
    subdir_done()
endif

subdir('base')
objs = [base_objs]

sources = files(
        'i40e_ethdev.c',
        'i40e_rxtx.c',
        'i40e_pf.c',
        'i40e_fdir.c',
        'i40e_flow.c',
        'i40e_tm.c',
        'i40e_hash.c',
        'i40e_vf_representor.c',
        'i40e_recycle_mbufs_vec_common.c',
        'rte_pmd_i40e.c',
)

testpmd_sources = files('i40e_testpmd.c')

deps += ['hash']
includes += include_directories('base')

if arch_subdir == 'x86'
    sources += files('i40e_rxtx_vec_sse.c')

    if is_windows and cc.get_id() != 'clang'
        cflags += ['-fno-asynchronous-unwind-tables']
    endif

    i40e_avx2_lib = static_library('i40e_avx2_lib',
            'i40e_rxtx_vec_avx2.c',
            dependencies: [static_rte_ethdev, static_rte_kvargs, static_rte_hash],
            include_directories: includes,
            c_args: [cflags, cc_avx2_flags])
    objs += i40e_avx2_lib.extract_objects('i40e_rxtx_vec_avx2.c')

    if cc_has_avx512
        cflags += ['-DCC_AVX512_SUPPORT']
        avx512_args = cflags + cc_avx512_flags
        if cc.has_argument('-march=skylake-avx512')
            avx512_args += '-march=skylake-avx512'
            if cc.has_argument('-Wno-overriding-option')
                avx512_args += '-Wno-overriding-option'
            endif
        endif
        i40e_avx512_lib = static_library('i40e_avx512_lib',
                'i40e_rxtx_vec_avx512.c',
                dependencies: [static_rte_ethdev,
                    static_rte_kvargs, static_rte_hash],
                include_directories: includes,
                c_args: avx512_args)
        objs += i40e_avx512_lib.extract_objects('i40e_rxtx_vec_avx512.c')
    endif
elif arch_subdir == 'ppc'
       sources += files('i40e_rxtx_vec_altivec.c')
elif arch_subdir == 'arm'
       sources += files('i40e_rxtx_vec_neon.c')
endif

headers = files('rte_pmd_i40e.h')
