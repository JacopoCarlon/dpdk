# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation

if is_windows
    build = false
    reason = 'not supported on Windows'
    subdir_done()
endif

sources = files('rte_bbdev.c',
        'bbdev_trace_points.c')
headers = files('rte_bbdev.h',
        'rte_bbdev_pmd.h',
        'rte_bbdev_op.h',
        'rte_bbdev_trace_fp.h')
deps += ['mbuf']
