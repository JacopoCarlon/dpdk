/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2021 Intel Corporation
 * Copyright(c) 2021 Arm Limited
 * Copyright(c) 2024 Advanced Micro Devices, Inc.
 */

#include <stdlib.h>

#include <rte_memcpy.h>
#include <rte_stdatomic.h>

#include "amd_pstate_cpufreq.h"
#include "power_common.h"

/* macros used for rounding frequency to nearest 1000 */
#define FREQ_ROUNDING_DELTA 500
#define ROUND_FREQ_TO_N_1000 1000

#define POWER_CONVERT_TO_DECIMAL 10

#define POWER_GOVERNOR_USERSPACE "userspace"
#define POWER_SYSFILE_SETSPEED   \
		"/sys/devices/system/cpu/cpu%u/cpufreq/scaling_setspeed"
#define POWER_SYSFILE_SCALING_MAX_FREQ \
		"/sys/devices/system/cpu/cpu%u/cpufreq/scaling_max_freq"
#define POWER_SYSFILE_SCALING_MIN_FREQ  \
		"/sys/devices/system/cpu/cpu%u/cpufreq/scaling_min_freq"
#define POWER_SYSFILE_HIGHEST_PERF \
		"/sys/devices/system/cpu/cpu%u/acpi_cppc/highest_perf"
#define POWER_SYSFILE_NOMINAL_PERF \
		"/sys/devices/system/cpu/cpu%u/acpi_cppc/nominal_perf"
#define POWER_SYSFILE_NOMINAL_FREQ \
		"/sys/devices/system/cpu/cpu%u/acpi_cppc/nominal_freq"

#define POWER_AMD_PSTATE_DRIVER "amd-pstate"
#define BUS_FREQ     1000	/* khz */

enum power_state {
	POWER_IDLE = 0,
	POWER_ONGOING,
	POWER_USED,
	POWER_UNKNOWN
};

/**
 * Power info per lcore.
 */
struct __rte_cache_aligned amd_pstate_power_info {
	uint32_t lcore_id;                   /**< Logical core id */
	RTE_ATOMIC(uint32_t) state;          /**< Power in use state */
	FILE *f;                             /**< FD of scaling_setspeed */
	char governor_ori[28];               /**< Original governor name */
	uint32_t curr_idx;                   /**< Freq index in freqs array */
	uint32_t nom_idx;                    /**< Nominal index in freqs array */
	uint32_t highest_perf;		     /**< system wide max freq */
	uint32_t nominal_perf;		     /**< system wide nominal freq */
	uint16_t turbo_available;            /**< Turbo Boost available */
	uint16_t turbo_enable;               /**< Turbo Boost enable/disable */
	uint32_t nb_freqs;                   /**< number of available freqs */
	uint32_t freqs[RTE_MAX_LCORE_FREQS]; /**< Frequency array */
};

static struct amd_pstate_power_info lcore_power_info[RTE_MAX_LCORE];

/**
 * It is to set specific freq for specific logical core, according to the index
 * of supported frequencies.
 */
static int
set_freq_internal(struct amd_pstate_power_info *pi, uint32_t idx)
{
	if (idx >= RTE_MAX_LCORE_FREQS || idx >= pi->nb_freqs) {
		POWER_LOG(ERR, "Invalid frequency index %u, which "
				"should be less than %u", idx, pi->nb_freqs);
		return -1;
	}

	/* Check if it is the same as current */
	if (idx == pi->curr_idx)
		return 0;

	POWER_DEBUG_LOG("Frequency[%u] %u to be set for lcore %u",
			idx, pi->freqs[idx], pi->lcore_id);
	if (fseek(pi->f, 0, SEEK_SET) < 0) {
		POWER_LOG(ERR, "Fail to set file position indicator to 0 "
			"for setting frequency for lcore %u", pi->lcore_id);
		return -1;
	}
	if (fprintf(pi->f, "%u", pi->freqs[idx]) < 0) {
		POWER_LOG(ERR, "Fail to write new frequency for "
				"lcore %u", pi->lcore_id);
		return -1;
	}
	fflush(pi->f);
	pi->curr_idx = idx;

	return 1;
}

/**
 * It is to check the current scaling governor by reading sys file, and then
 * set it into 'userspace' if it is not by writing the sys file. The original
 * governor will be saved for rolling back.
 */
static int
power_set_governor_userspace(struct amd_pstate_power_info *pi)
{
	return power_set_governor(pi->lcore_id, POWER_GOVERNOR_USERSPACE,
			pi->governor_ori, sizeof(pi->governor_ori));
}

static int
power_check_turbo(struct amd_pstate_power_info *pi)
{
	FILE *f_nom = NULL, *f_max = NULL;
	int ret = -1;
	uint32_t nominal_perf = 0, highest_perf = 0;

	open_core_sysfs_file(&f_max, "r", POWER_SYSFILE_HIGHEST_PERF,
			pi->lcore_id);
	if (f_max == NULL) {
		POWER_LOG(ERR, "failed to open %s",
				POWER_SYSFILE_HIGHEST_PERF);
		goto err;
	}

	open_core_sysfs_file(&f_nom, "r", POWER_SYSFILE_NOMINAL_PERF,
			pi->lcore_id);
	if (f_nom == NULL) {
		POWER_LOG(ERR, "failed to open %s",
				POWER_SYSFILE_NOMINAL_PERF);
		goto err;
	}

	ret = read_core_sysfs_u32(f_max, &highest_perf);
	if (ret < 0) {
		POWER_LOG(ERR, "Failed to read %s",
				POWER_SYSFILE_HIGHEST_PERF);
		goto err;
	}

	ret = read_core_sysfs_u32(f_nom, &nominal_perf);
	if (ret < 0) {
		POWER_LOG(ERR, "Failed to read %s",
				POWER_SYSFILE_NOMINAL_PERF);
		goto err;
	}

	pi->highest_perf = highest_perf;
	pi->nominal_perf = nominal_perf;

	if (highest_perf > nominal_perf) {
		pi->turbo_available = 1;
		pi->turbo_enable = 1;
		ret = 0;
		POWER_DEBUG_LOG("Lcore %u can do Turbo Boost! highest perf %u, "
				"nominal perf %u",
				pi->lcore_id, highest_perf, nominal_perf);
	} else {
		pi->turbo_available = 0;
		pi->turbo_enable = 0;
		POWER_DEBUG_LOG("Lcore %u Turbo not available! highest perf %u, "
				"nominal perf %u",
				pi->lcore_id, highest_perf, nominal_perf);
	}

err:
	if (f_max != NULL)
		fclose(f_max);
	if (f_nom != NULL)
		fclose(f_nom);

	return ret;
}

/**
 * It is to get the available frequencies of the specific lcore by reading the
 * sys file.
 */
static int
power_get_available_freqs(struct amd_pstate_power_info *pi)
{
	FILE *f_min = NULL, *f_max = NULL, *f_nom = NULL;
	int ret = -1, nominal_idx = -1;
	uint32_t scaling_min_freq = 0, scaling_max_freq = 0;
	uint32_t i, num_freqs = RTE_MAX_LCORE_FREQS;
	uint32_t nominal_freq = 0, scaling_freq = 0;
	uint32_t freq_calc = 0;

	open_core_sysfs_file(&f_max, "r", POWER_SYSFILE_SCALING_MAX_FREQ,
			pi->lcore_id);
	if (f_max == NULL) {
		POWER_LOG(ERR, "failed to open %s",
				POWER_SYSFILE_SCALING_MAX_FREQ);
		goto out;
	}

	open_core_sysfs_file(&f_min, "r", POWER_SYSFILE_SCALING_MIN_FREQ,
			pi->lcore_id);
	if (f_min == NULL) {
		POWER_LOG(ERR, "failed to open %s",
				POWER_SYSFILE_SCALING_MIN_FREQ);
		goto out;
	}

	open_core_sysfs_file(&f_nom, "r", POWER_SYSFILE_NOMINAL_FREQ,
			pi->lcore_id);
	if (f_nom == NULL) {
		POWER_LOG(ERR, "failed to open %s",
				POWER_SYSFILE_NOMINAL_FREQ);
		goto out;
	}

	ret = read_core_sysfs_u32(f_max, &scaling_max_freq);
	if (ret < 0) {
		POWER_LOG(ERR, "Failed to read %s",
				POWER_SYSFILE_SCALING_MAX_FREQ);
		goto out;
	}

	ret = read_core_sysfs_u32(f_min, &scaling_min_freq);
	if (ret < 0) {
		POWER_LOG(ERR, "Failed to read %s",
				POWER_SYSFILE_SCALING_MIN_FREQ);
		goto out;
	}

	ret = read_core_sysfs_u32(f_nom, &nominal_freq);
	if (ret < 0) {
		POWER_LOG(ERR, "Failed to read %s",
				POWER_SYSFILE_NOMINAL_FREQ);
		goto out;
	}

	power_check_turbo(pi);

	if (scaling_max_freq < scaling_min_freq) {
		POWER_LOG(ERR, "scaling min freq exceeds max freq, "
			"not expected! Check system power policy");
		goto out;
	} else if (scaling_max_freq == scaling_min_freq) {
		num_freqs = 1;
	}

	if (num_freqs > 1) {
		scaling_freq = (scaling_max_freq - scaling_min_freq);
		scaling_freq <<= 10;
		scaling_freq /= (num_freqs - 1);
		scaling_freq >>= 10;
	} else {
		scaling_freq = 0;
	}

	/* Generate the freq bucket array. */
	for (i = 0, pi->nb_freqs = 0; i < num_freqs; i++) {
		freq_calc = scaling_max_freq - (i * scaling_freq);
		/* convert the frequency to nearest 1000 value
		 * Ex: if freq=1396789 then freq_conv=1397000
		 * Ex: if freq=800030 then freq_conv=800000
		 */
		freq_calc = (freq_calc + FREQ_ROUNDING_DELTA)
					/ ROUND_FREQ_TO_N_1000;
		freq_calc = freq_calc * ROUND_FREQ_TO_N_1000;

		/* update the frequency table only if required */
		if ((pi->nb_freqs == 0) ||
				pi->freqs[pi->nb_freqs-1] != freq_calc) {
			pi->freqs[pi->nb_freqs++] = freq_calc;
		}
		if (nominal_idx == -1) {
			if ((nominal_freq * BUS_FREQ) >= freq_calc) {
				pi->nom_idx = pi->nb_freqs - 1;
				nominal_idx = pi->nom_idx;
			}
		}
	}

	ret = 0;

	POWER_DEBUG_LOG("%d frequency(s) of lcore %u are available",
			num_freqs, pi->lcore_id);

out:
	if (f_min != NULL)
		fclose(f_min);
	if (f_max != NULL)
		fclose(f_max);
	if (f_nom != NULL)
		fclose(f_nom);

	return ret;
}

/**
 * It is to fopen the sys file for the future setting the lcore frequency.
 */
static int
power_init_for_setting_freq(struct amd_pstate_power_info *pi)
{
	FILE *f = NULL;
	char buf[BUFSIZ];
	uint32_t i, freq;
	int ret;

	open_core_sysfs_file(&f, "rw+", POWER_SYSFILE_SETSPEED, pi->lcore_id);
	if (f == NULL) {
		POWER_LOG(ERR, "failed to open %s",
				POWER_SYSFILE_SETSPEED);
		goto err;
	}

	ret = read_core_sysfs_s(f, buf, sizeof(buf));
	if (ret < 0) {
		POWER_LOG(ERR, "Failed to read %s",
				POWER_SYSFILE_SETSPEED);
		goto err;
	}

	freq = strtoul(buf, NULL, POWER_CONVERT_TO_DECIMAL);

	/* convert the frequency to nearest 1000 value
	 * Ex: if freq=1396789 then freq_conv=1397000
	 * Ex: if freq=800030 then freq_conv=800000
	 */
	unsigned int freq_conv = 0;
	freq_conv = (freq + FREQ_ROUNDING_DELTA)
				/ ROUND_FREQ_TO_N_1000;
	freq_conv = freq_conv * ROUND_FREQ_TO_N_1000;

	for (i = 0; i < pi->nb_freqs; i++) {
		if (freq_conv == pi->freqs[i]) {
			pi->curr_idx = i;
			pi->f = f;
			return 0;
		}
	}

err:
	if (f != NULL)
		fclose(f);

	return -1;
}

int
power_amd_pstate_cpufreq_check_supported(void)
{
	printf("calling cpufreq_check_scaling_driver on POWER_AMD_PSTATE_DRIVER\n");
	return cpufreq_check_scaling_driver(POWER_AMD_PSTATE_DRIVER);
}

int
power_amd_pstate_cpufreq_init(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;
	uint32_t exp_state;

	if (!power_amd_pstate_cpufreq_check_supported()) {
		POWER_LOG(ERR, "%s driver is not supported",
				POWER_AMD_PSTATE_DRIVER);
		return -1;
	}

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Lcore id %u can not exceeds %u",
				lcore_id, RTE_MAX_LCORE - 1U);
		return -1;
	}

	pi = &lcore_power_info[lcore_id];
	exp_state = POWER_IDLE;
	/* The power in use state works as a guard variable between
	 * the CPU frequency control initialization and exit process.
	 * The ACQUIRE memory ordering here pairs with the RELEASE
	 * ordering below as lock to make sure the frequency operations
	 * in the critical section are done under the correct state.
	 */
	if (!rte_atomic_compare_exchange_strong_explicit(&(pi->state),
					&exp_state, POWER_ONGOING,
					rte_memory_order_acquire, rte_memory_order_relaxed)) {
		POWER_LOG(INFO, "Power management of lcore %u is "
				"in use", lcore_id);
		return -1;
	}

	if (power_get_lcore_mapped_cpu_id(lcore_id, &pi->lcore_id) < 0) {
		POWER_LOG(ERR, "Cannot get CPU ID mapped for lcore %u", lcore_id);
		return -1;
	}

	/* Check and set the governor */
	if (power_set_governor_userspace(pi) < 0) {
		POWER_LOG(ERR, "Cannot set governor of lcore %u to "
				"userspace", lcore_id);
		goto fail;
	}

	/* Get the available frequencies */
	if (power_get_available_freqs(pi) < 0) {
		POWER_LOG(ERR, "Cannot get available frequencies of "
				"lcore %u", lcore_id);
		goto fail;
	}

	/* Init for setting lcore frequency */
	if (power_init_for_setting_freq(pi) < 0) {
		POWER_LOG(ERR, "Cannot init for setting frequency for "
				"lcore %u", lcore_id);
		goto fail;
	}

	/* Set freq to max by default */
	if (power_amd_pstate_cpufreq_freq_max(lcore_id) < 0) {
		POWER_LOG(ERR, "Cannot set frequency of lcore %u "
				"to max", lcore_id);
		goto fail;
	}

	POWER_LOG(INFO, "Initialized successfully for lcore %u "
			"power management", lcore_id);

	rte_atomic_store_explicit(&(pi->state), POWER_USED, rte_memory_order_release);

	return 0;

fail:
	rte_atomic_store_explicit(&(pi->state), POWER_UNKNOWN, rte_memory_order_release);
	return -1;
}

/**
 * It is to check the governor and then set the original governor back if
 * needed by writing the sys file.
 */
static int
power_set_governor_original(struct amd_pstate_power_info *pi)
{
	return power_set_governor(pi->lcore_id, pi->governor_ori, NULL, 0);
}

int
power_amd_pstate_cpufreq_exit(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;
	uint32_t exp_state;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Lcore id %u can not exceeds %u",
				lcore_id, RTE_MAX_LCORE - 1U);
		return -1;
	}
	pi = &lcore_power_info[lcore_id];
	exp_state = POWER_USED;
	/* The power in use state works as a guard variable between
	 * the CPU frequency control initialization and exit process.
	 * The ACQUIRE memory ordering here pairs with the RELEASE
	 * ordering below as lock to make sure the frequency operations
	 * in the critical section are done under the correct state.
	 */
	if (!rte_atomic_compare_exchange_strong_explicit(&(pi->state),
					&exp_state, POWER_ONGOING,
					rte_memory_order_acquire, rte_memory_order_relaxed)) {
		POWER_LOG(INFO, "Power management of lcore %u is "
				"not used", lcore_id);
		return -1;
	}

	/* Close FD of setting freq */
	fclose(pi->f);
	pi->f = NULL;

	/* Set the governor back to the original */
	if (power_set_governor_original(pi) < 0) {
		POWER_LOG(ERR, "Cannot set the governor of %u back "
				"to the original", lcore_id);
		goto fail;
	}

	POWER_LOG(INFO, "Power management of lcore %u has exited from "
			"'userspace' mode and been set back to the "
			"original", lcore_id);
	rte_atomic_store_explicit(&(pi->state), POWER_IDLE, rte_memory_order_release);

	return 0;

fail:
	rte_atomic_store_explicit(&(pi->state), POWER_UNKNOWN, rte_memory_order_release);

	return -1;
}

uint32_t
power_amd_pstate_cpufreq_freqs(unsigned int lcore_id, uint32_t *freqs, uint32_t num)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return 0;
	}

	if (freqs == NULL) {
		POWER_LOG(ERR, "NULL buffer supplied");
		return 0;
	}

	pi = &lcore_power_info[lcore_id];
	if (num < pi->nb_freqs) {
		POWER_LOG(ERR, "Buffer size is not enough");
		return 0;
	}
	rte_memcpy(freqs, pi->freqs, pi->nb_freqs * sizeof(uint32_t));

	return pi->nb_freqs;
}

uint32_t
power_amd_pstate_cpufreq_get_freq(unsigned int lcore_id)
{
	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return RTE_POWER_INVALID_FREQ_INDEX;
	}

	return lcore_power_info[lcore_id].curr_idx;
}

int
power_amd_pstate_cpufreq_set_freq(unsigned int lcore_id, uint32_t index)
{
	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	return set_freq_internal(&(lcore_power_info[lcore_id]), index);
}

int
power_amd_pstate_cpufreq_freq_down(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	pi = &lcore_power_info[lcore_id];
	if (pi->curr_idx + 1 == pi->nb_freqs)
		return 0;

	/* Frequencies in the array are from high to low. */
	return set_freq_internal(pi, pi->curr_idx + 1);
}

int
power_amd_pstate_cpufreq_freq_up(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	pi = &lcore_power_info[lcore_id];
	if (pi->curr_idx == 0 || (pi->curr_idx == pi->nom_idx &&
		pi->turbo_available && !pi->turbo_enable))
		return 0;

	/* Frequencies in the array are from high to low. */
	return set_freq_internal(pi, pi->curr_idx - 1);
}

int
power_amd_pstate_cpufreq_freq_max(unsigned int lcore_id)
{
	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	/* Frequencies in the array are from high to low. */
	if (lcore_power_info[lcore_id].turbo_available) {
		if (lcore_power_info[lcore_id].turbo_enable)
			/* Set to Turbo */
			return set_freq_internal(
				&lcore_power_info[lcore_id], 0);
		else
			/* Set to max non-turbo */
			return set_freq_internal(
				&lcore_power_info[lcore_id],
				lcore_power_info[lcore_id].nom_idx);
	} else
		return set_freq_internal(&lcore_power_info[lcore_id], 0);
}

int
power_amd_pstate_cpufreq_freq_min(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	pi = &lcore_power_info[lcore_id];

	/* Frequencies in the array are from high to low. */
	return set_freq_internal(pi, pi->nb_freqs - 1);
}

int
power_amd_pstate_turbo_status(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	pi = &lcore_power_info[lcore_id];

	return pi->turbo_enable;
}

int
power_amd_pstate_enable_turbo(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	pi = &lcore_power_info[lcore_id];

	if (pi->turbo_available)
		pi->turbo_enable = 1;
	else {
		pi->turbo_enable = 0;
		POWER_LOG(ERR,
			"Failed to enable turbo on lcore %u",
			lcore_id);
		return -1;
	}

	/* TODO: must set to max once enabling Turbo? Considering add condition:
	 * if ((pi->turbo_available) && (pi->curr_idx <= 1))
	 */
	/* Max may have changed, so call to max function */
	if (power_amd_pstate_cpufreq_freq_max(lcore_id) < 0) {
		POWER_LOG(ERR,
			"Failed to set frequency of lcore %u to max",
			lcore_id);
		return -1;
	}

	return 0;
}

int
power_amd_pstate_disable_turbo(unsigned int lcore_id)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}

	pi = &lcore_power_info[lcore_id];

	pi->turbo_enable = 0;

	if ((pi->turbo_available) && (pi->curr_idx <= pi->nom_idx)) {
		/* Try to set freq to max by default coming out of turbo */
		if (power_amd_pstate_cpufreq_freq_max(lcore_id) < 0) {
			POWER_LOG(ERR,
				"Failed to set frequency of lcore %u to max",
				lcore_id);
			return -1;
		}
	}

	return 0;
}

int
power_amd_pstate_get_capabilities(unsigned int lcore_id,
		struct rte_power_core_capabilities *caps)
{
	struct amd_pstate_power_info *pi;

	if (lcore_id >= RTE_MAX_LCORE) {
		POWER_LOG(ERR, "Invalid lcore ID");
		return -1;
	}
	if (caps == NULL) {
		POWER_LOG(ERR, "Invalid argument");
		return -1;
	}

	pi = &lcore_power_info[lcore_id];
	caps->capabilities = 0;
	caps->turbo = !!(pi->turbo_available);

	return 0;
}

static struct rte_power_cpufreq_ops amd_pstate_ops = {
	.name = "amd-pstate",
	.init = power_amd_pstate_cpufreq_init,
	.exit = power_amd_pstate_cpufreq_exit,
	.check_env_support = power_amd_pstate_cpufreq_check_supported,
	.get_avail_freqs = power_amd_pstate_cpufreq_freqs,
	.get_freq = power_amd_pstate_cpufreq_get_freq,
	.set_freq = power_amd_pstate_cpufreq_set_freq,
	.freq_down = power_amd_pstate_cpufreq_freq_down,
	.freq_up = power_amd_pstate_cpufreq_freq_up,
	.freq_max = power_amd_pstate_cpufreq_freq_max,
	.freq_min = power_amd_pstate_cpufreq_freq_min,
	.turbo_status = power_amd_pstate_turbo_status,
	.enable_turbo = power_amd_pstate_enable_turbo,
	.disable_turbo = power_amd_pstate_disable_turbo,
	.get_caps = power_amd_pstate_get_capabilities
};

RTE_POWER_REGISTER_CPUFREQ_OPS(amd_pstate_ops);
