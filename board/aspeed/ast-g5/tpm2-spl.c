/*
 * (C) Copyright 2016-Present, Facebook, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */
#include <common.h>
#include <dm.h>
#include <tpm-v2.h>
#include <asm/arch/ast-sdk/vbs.h>
#include "tpm-spl.h"

static const u32 RB_NV_PROPERTY_CREATE =
	(TPMA_NV_PPREAD | TPMA_NV_PPWRITE | TPMA_NV_PLATFORMCREATE);
static const u32 RB_NV_PROPERTY_PROVISIONED =
	(RB_NV_PROPERTY_CREATE | TPMA_NV_WRITTEN);
#define RB_NV_INDEX_HANDLE                                                     \
	((TPM_HT_NV_INDEX << 24) | (VBS_TPM_ROLLBACK_INDEX & 0xFFFFF))

static int get_tpm(struct udevice **devp)
{
	int rc;

	rc = uclass_first_device_err(UCLASS_TPM, devp);
	if (rc) {
		printf("SPL Could not find TPM (ret=%d)\n", rc);
		return CMD_RET_FAILURE;
	}

	return 0;
}

static inline void set_tpm_error(struct vbs *vbs, uint32_t r)
{
	vbs->error_tpm2 = r & 0xFFFF;
}

int ast_tpm_provision(struct vbs *vbs)
{
	uint32_t result;
	int err;
	uint32_t retries;
	struct udevice *dev;

	/* Get the TPM device handler */
	err = get_tpm(&dev);
	if (err) {
		debug("get tpm failed (%d)\n", err);
		return VBS_ERROR_TPM_NOT_ENABLED;
	}

	/* The Infineon TPM needs 35ms */
	debug("start udelay 35ms\n");
	udelay(35 * 1000);

	/* The SPL should init (software-only setup), startup-clear, and test. */
	result = tpm_init(dev);
	if (result) {
		set_tpm_error(vbs, result);
		return VBS_ERROR_TPM_NOT_ENABLED;
	}

	result = tpm2_startup(dev, TPM2_SU_CLEAR);
	if (result) {
		/* If this returns invalid postinit (38) then the TPM was not reset. */
		log_err("TPM2 startup failed (0x%08x)\n", result);
		set_tpm_error(vbs, result);
		return VBS_ERROR_TPM_SETUP;
	}

	/* run full selftest */
	result = tpm2_self_test(dev, TPMI_YES);

	/* Wait until test done. */
	for (retries = 0; (TPM2_RC_TESTING == result) && (retries < 3);
	     retries++) {
		log_warning("delay 10ms wait selftest done.\n");
		udelay(10 * 1000);
		result = tpm2_self_test(dev, TPMI_NO);
	}

	if (TPM2_RC_SUCCESS != result) {
		log_err("TPM2 self_test failed (0x%08x)\n", result);
		set_tpm_error(vbs, result);
		return VBS_ERROR_TPM_SETUP;
	}

	return VBS_SUCCESS;
}

int ast_tpm_owner_provision(struct vbs *vbs)
{
	/* nothing need for TPM2 as we use platform hierarchy */
	return VBS_SUCCESS;
}

u32 tpm2_check_rollback_nv(struct udevice *dev)
{
	u32 result;
	u32 attributes;
	u16 hash_alg, policy_sz, data_sz, nv_name_sz;
	u8 policy[TPM2_DIGEST_LEN] = { 0 };
	u8 nv_name[TPM2_DIGEST_LEN] = { 0 };

	result = tpm2_nv_readpublic(dev, RB_NV_INDEX_HANDLE, &hash_alg,
				    &attributes, &policy_sz, policy, &data_sz,
				    &nv_name_sz, nv_name);

	if (TPM2_RC_SUCCESS != result) {
		log_warning("Read NV-index public fail (0x%x)\n", result);
		return result;
	}
	/* check the size */
	if (data_sz != VBS_TPM_ROLLBACK_SIZE) {
		log_warning("NV defined in wrong size (%d)\n", data_sz);
		log_warning("undefine it\n");
		result = tpm2_nv_undefinespace(dev, NULL, 0, TPM2_RH_PLATFORM,
					       RB_NV_INDEX_HANDLE);
		if (TPM2_RC_SUCCESS != result) {
			return result;
		}
		return TPM2_RC_HANDLE1;
	}
	/* check the property PPREAD|PPWRITE|PLATFORMCREATE|WRITTEN */
	if (RB_NV_PROPERTY_PROVISIONED != attributes) {
		log_warning("NV defined in wrong attributes(0x%08x)\n",
			    attributes);
		log_warning("undefine it\n");
		result = tpm2_nv_undefinespace(dev, NULL, 0, TPM2_RH_PLATFORM,
					       RB_NV_INDEX_HANDLE);
		if (TPM2_RC_SUCCESS != result) {
			return result;
		}
		return TPM2_RC_HANDLE1;
	}

	return TPM2_RC_SUCCESS;
}

static u32 ast_tpm_write(struct udevice *dev, const void *data, size_t length)
{
	u32 result = VBS_SUCCESS;

	result = tpm2_nv_write(dev, NULL, 0, TPM2_RH_PLATFORM,
			       RB_NV_INDEX_HANDLE, (u8 *)data, length, 0);

	return result;
}

#pragma GCC push_options
#pragma GCC optimize ("O0")
int ast_tpm_nv_provision(struct vbs *vbs)
{
	struct udevice *dev;
	int err;
	u32 result;
	char blank[VBS_TPM_ROLLBACK_SIZE];

	err = get_tpm(&dev);
	if (err) {
		return err;
	}

	result = tpm2_check_rollback_nv(dev);
	if (TPM2_RC_SUCCESS == result) {
		log_info("NV already provisioned\n");
		return VBS_SUCCESS;
	}

	if (TPM2_RC_HANDLE1 == result) {
		log_info("Define rollback NV-index 0x%08x\n",
			 RB_NV_INDEX_HANDLE);
		result =
			tpm2_nv_definespace(dev, NULL, 0, TPM2_RH_PLATFORM,
					    RB_NV_INDEX_HANDLE, TPM2_ALG_SHA256,
					    RB_NV_PROPERTY_CREATE,
					    VBS_TPM_ROLLBACK_SIZE);
	}

	if (TPM2_RC_SUCCESS != result) {
		log_err("define rollback nv failed (0x%08x)\n", result);
		set_tpm_error(vbs, result);
		return VBS_ERROR_TPM_NV_SPACE;
	}

	memset(blank, 0x0, sizeof(blank));
	result = ast_tpm_write(dev, blank, sizeof(blank));
	if (result) {
		log_err("blank rollback nv failed (0x%08x)\n", result);
		set_tpm_error(vbs, result);
		return VBS_ERROR_TPM_NV_BLANK;
	}

	return VBS_SUCCESS;
}
#pragma GCC pop_options

int ast_tpm_extend(uint32_t index, unsigned char *data, uint32_t data_len)
{
	unsigned char value[TPM2_DIGEST_LEN] = {
		0,
	};
	struct udevice *dev;
	u32 result;
	int err;

	err = get_tpm(&dev);
	if (err) {
		return err;
	}

	log_debug("sha256(%d) start\n", data_len);
	sha256_csum_wd(data, data_len, value, CHUNKSZ_SHA256);
	log_debug("sha256(%d) done\n", data_len);

	result = tpm2_pcr_extend(dev, index, value);
	if (TPM2_RC_SUCCESS != result) {
		log_err("Extend PCR(%d) failed (0x%08x)\n", index, result);
		return -1;
	}

	return 0;
}

static void ast_tpm_update_vbs_times(struct tpm_rollback_t *rb, struct vbs *vbs)
{
	/* This copies the TPM NV data into the verified-boot status structure. */
	vbs->subordinate_last = rb->fallback_subordinate;
	vbs->subordinate_current = rb->subordinate;
	vbs->uboot_last = rb->fallback_uboot;
	vbs->uboot_current = rb->uboot;
	vbs->kernel_last = rb->fallback_kernel;
	vbs->kernel_current = rb->kernel;
}

int ast_tpm_try_version(struct vbs *vbs, uint8_t image, uint32_t version,
			bool no_fallback)
{
	uint32_t result;
	uint32_t *rb_target;
	uint32_t *rb_fallback_target;
	struct tpm_rollback_t rb;
	struct udevice *dev;
	int err;

	err = get_tpm(&dev);
	if (err) {
		return err;
	}

	/* Need to load the last-executed version of U-Boot. */
	rb.subordinate = -1;
	rb.uboot = -1;
	rb.kernel = -1;
	rb.fallback_subordinate = -1;
	rb.fallback_uboot = -1;
	rb.fallback_kernel = -1;
	if (image == AST_TPM_ROLLBACK_UBOOT) {
		rb_target = &rb.uboot;
		rb_fallback_target = &rb.fallback_uboot;
	} else if (image == AST_TPM_ROLLBACK_SUBORDINATE) {
		rb_target = &rb.subordinate;
		rb_fallback_target = &rb.fallback_subordinate;
	} else {
		rb_target = &rb.kernel;
		rb_fallback_target = &rb.fallback_kernel;
	}

	result = tpm2_nv_read(dev, NULL, 0, TPM2_RH_PLATFORM,
			      RB_NV_INDEX_HANDLE, sizeof(rb), 0, (u8 *)&rb);
	if (result) {
		log_err("Read rollback nv failed (0x%08x)\n", result);
		set_tpm_error(vbs, result);
		return VBS_ERROR_TPM_NV_READ_FAILED;
	}

	ast_tpm_update_vbs_times(&rb, vbs);
	if (*rb_target == -1) {
	/**
	* Content is still -1.
	* Alternatively someone had booted a payload signed at time UINT_MAX.
	* This is huge issue and will brick the system from future updates.
	* To save the system and put security/safety pressure on the signer, this
	* causes an intentional wrap-around.
	*/
		*rb_target = 0x0;
	}

	if (*rb_target > version) {
		/* This seems to be attempting a rollback. */
		if (no_fallback || *rb_fallback_target != version) {
			return VBS_ERROR_ROLLBACK_FAILED;
		}
	}

	if (*rb_target != 0 &&
	    version > *rb_target + (86400 * 365 * AST_TPM_MAX_YEARS)) {
		/* Do not allow fast-forwarding beyond 5 years. */
		return VBS_ERROR_ROLLBACK_HUGE;
	}

	if (version != *rb_target) {
		/* Only update the NV space if this is a new version or fallback 'promo'. */
		if (no_fallback) {
			/* Fallback is disabled, the fallback is always the current. */
			*rb_fallback_target = version;
		} else if (version != *rb_fallback_target) {
			/* This is not fallback 'promotion', previous version is now fallback. */
			*rb_fallback_target = *rb_target;
		}

		/* Current is now the promoted fallback or a later version. */
		*rb_target = version;
		if (vbs->error_code == VBS_SUCCESS) {
			/* Only update times if verification has not yet failed. */
			result = ast_tpm_write(dev, &rb, sizeof(rb));
			if (result) {
				set_tpm_error(vbs, result);
				return VBS_ERROR_TPM_NV_WRITE_FAILED;
			}
		}

		/* The times have changed. */
		ast_tpm_update_vbs_times(&rb, vbs);
	}

	return VBS_SUCCESS;
}

int ast_tpm_finish(void)
{
	uint32_t result;
	struct udevice *dev;
	int err;

	err = get_tpm(&dev);
	if (err) {
		return err;
	}
#ifdef CONFIG_ASPEED_TPM_LOCK
	result = tpm2_hierarchy_control(dev, NULL, 0, TPM2_RH_PLATFORM,
					TPM2_RH_PLATFORM, false);
	if (TPM2_RC_SUCCESS != result) {
		log_err("disable platform hierarchy failed(0x%08x)\n", result);
		return VBS_ERROR_ROLLBACK_FINISH;
	}
#endif
	return VBS_SUCCESS;
}