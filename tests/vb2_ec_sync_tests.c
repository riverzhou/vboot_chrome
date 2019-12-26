/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Tests for vboot_api_kernel, part 3 - software sync
 */

#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2sysincludes.h"
#include "host_common.h"
#include "load_kernel_fw.h"
#include "secdata_tpm.h"
#include "test_common.h"
#include "vboot_audio.h"
#include "vboot_display.h"
#include "vboot_kernel.h"
#include "vboot_struct.h"

/* Mock data */
static uint8_t shared_data[VB_SHARED_DATA_MIN_SIZE];
static VbSharedDataHeader *shared = (VbSharedDataHeader *)shared_data;

static int mock_in_rw;
static vb2_error_t in_rw_retval;
static int protect_retval;
static int ec_ro_protected;
static int ec_rw_protected;
static int run_retval;
static int ec_run_image;
static int update_retval;
static int ec_ro_updated;
static int ec_rw_updated;
static int get_expected_retval;
static int shutdown_request_calls_left;

static uint8_t mock_ec_ro_hash[32];
static uint8_t mock_ec_rw_hash[32];
static int mock_ec_ro_hash_size;
static int mock_ec_rw_hash_size;
static uint8_t want_ec_hash[32];
static uint8_t update_hash;
static int want_ec_hash_size;
static uint8_t workbuf[VB2_KERNEL_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static struct vb2_context *ctx;
static struct vb2_shared_data *sd;
static struct vb2_gbb_header gbb;
static vb2_error_t ec_vboot_done_retval;

static uint32_t screens_displayed[8];
static uint32_t screens_count = 0;

/* Reset mock data (for use before each test) */
static void ResetMocks(void)
{
	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init failed");

	ctx->flags = VB2_CONTEXT_EC_SYNC_SUPPORTED;
	vb2_nv_init(ctx);

	sd = vb2_get_sd(ctx);
	sd->vbsd = shared;
	sd->flags |= VB2_SD_FLAG_DISPLAY_AVAILABLE;

	memset(&gbb, 0, sizeof(gbb));

	memset(&shared_data, 0, sizeof(shared_data));

	mock_in_rw = 0;
	ec_ro_protected = 0;
	ec_rw_protected = 0;
	ec_run_image = 0;   /* 0 = RO, 1 = RW */
	ec_ro_updated = 0;
	ec_rw_updated = 0;
	in_rw_retval = VB2_SUCCESS;
	protect_retval = VB2_SUCCESS;
	update_retval = VB2_SUCCESS;
	run_retval = VB2_SUCCESS;
	get_expected_retval = VB2_SUCCESS;
	shutdown_request_calls_left = -1;

	memset(mock_ec_ro_hash, 0, sizeof(mock_ec_ro_hash));
	mock_ec_ro_hash[0] = 42;
	mock_ec_ro_hash_size = sizeof(mock_ec_ro_hash);

	memset(mock_ec_rw_hash, 0, sizeof(mock_ec_rw_hash));
	mock_ec_rw_hash[0] = 42;
	mock_ec_rw_hash_size = sizeof(mock_ec_rw_hash);

	memset(want_ec_hash, 0, sizeof(want_ec_hash));
	want_ec_hash[0] = 42;
	want_ec_hash_size = sizeof(want_ec_hash);

	update_hash = 42;

	// TODO: ensure these are actually needed

	memset(screens_displayed, 0, sizeof(screens_displayed));
	screens_count = 0;
}

/* Mock functions */
struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *c)
{
	return &gbb;
}

uint32_t VbExIsShutdownRequested(void)
{
	if (shutdown_request_calls_left == 0)
		return 1;
	else if (shutdown_request_calls_left > 0)
		shutdown_request_calls_left--;

	return 0;
}

int vb2ex_ec_trusted(void)
{
	return !mock_in_rw;
}

vb2_error_t vb2ex_ec_running_rw(int *in_rw)
{
	*in_rw = mock_in_rw;
	return in_rw_retval;
}

vb2_error_t vb2ex_ec_protect(enum vb2_firmware_selection select)
{
	if (select == VB_SELECT_FIRMWARE_READONLY)
		ec_ro_protected = 1;
	else
		ec_rw_protected = 1;
	return protect_retval;
}

vb2_error_t vb2ex_ec_disable_jump(void)
{
	return run_retval;
}

vb2_error_t vb2ex_ec_jump_to_rw(void)
{
	ec_run_image = 1;
	mock_in_rw = 1;
	return run_retval;
}

vb2_error_t vb2ex_ec_hash_image(enum vb2_firmware_selection select,
				const uint8_t **hash, int *hash_size)
{
	*hash = select == VB_SELECT_FIRMWARE_READONLY ?
		mock_ec_ro_hash : mock_ec_rw_hash;
	*hash_size = select == VB_SELECT_FIRMWARE_READONLY ?
		     mock_ec_ro_hash_size : mock_ec_rw_hash_size;
	return *hash_size ? VB2_SUCCESS : VB2_ERROR_MOCK;
}

vb2_error_t vb2ex_ec_get_expected_image_hash(enum vb2_firmware_selection select,
					     const uint8_t **hash, int *hash_size)
{
	*hash = want_ec_hash;
	*hash_size = want_ec_hash_size;

	return want_ec_hash_size ? VB2_SUCCESS : VB2_ERROR_MOCK;
}

vb2_error_t vb2ex_ec_update_image(enum vb2_firmware_selection select)
{
	if (select == VB_SELECT_FIRMWARE_READONLY) {
		ec_ro_updated = 1;
		mock_ec_ro_hash[0] = update_hash;
	 } else {
		ec_rw_updated = 1;
		mock_ec_rw_hash[0] = update_hash;
	}
	return update_retval;
}

vb2_error_t VbDisplayScreen(struct vb2_context *c, uint32_t screen, int force,
			    const VbScreenData *data)
{
	if (screens_count < ARRAY_SIZE(screens_displayed))
		screens_displayed[screens_count++] = screen;

	return VB2_SUCCESS;
}

vb2_error_t vb2ex_ec_vboot_done(struct vb2_context *c)
{
	return ec_vboot_done_retval;
}

static void test_ssync(vb2_error_t retval, int recovery_reason,
		       const char *desc)
{
	TEST_EQ(vb2api_ec_sync(ctx), retval, desc);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST),
		recovery_reason, "  recovery reason");
}

/* Tests */

static void VbSoftwareSyncTest(void)
{
	/* AP-RO cases */
	ResetMocks();
	in_rw_retval = VB2_ERROR_MOCK;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_UNKNOWN_IMAGE, "Unknown EC image");

	/* Calculate hashes */
	ResetMocks();
	mock_ec_rw_hash_size = 0;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_HASH_FAILED, "Bad EC hash");

	ResetMocks();
	mock_ec_rw_hash_size = 16;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_HASH_SIZE, "Bad EC hash size");

	ResetMocks();
	want_ec_hash_size = 0;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_EXPECTED_HASH, "Bad precalculated hash");

	ResetMocks();
	want_ec_hash_size = 16;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_HASH_SIZE,
		   "Hash size mismatch");

	ResetMocks();
	want_ec_hash_size = 4;
	mock_ec_rw_hash_size = 4;
	test_ssync(0, 0, "Custom hash size");

	/* Updates required */
	ResetMocks();
	mock_in_rw = 1;
	mock_ec_rw_hash[0]++;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   0, "Pending update needs reboot");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	vb2_nv_set(ctx, VB2_NV_TRY_RO_SYNC, 1);
	test_ssync(0, 0, "Update rw without reboot");
	TEST_EQ(ec_rw_protected, 1, "  ec rw protected");
	TEST_EQ(ec_run_image, 1, "  ec run image");
	TEST_EQ(ec_rw_updated, 1, "  ec rw updated");
	TEST_EQ(ec_ro_protected, 1, "  ec ro protected");
	TEST_EQ(ec_ro_updated, 0, "  ec ro updated");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	mock_ec_ro_hash[0]++;
	vb2_nv_set(ctx, VB2_NV_TRY_RO_SYNC, 1);
	test_ssync(0, 0, "Update rw and ro images without reboot");
	TEST_EQ(ec_rw_protected, 1, "  ec rw protected");
	TEST_EQ(ec_run_image, 1, "  ec run image");
	TEST_EQ(ec_rw_updated, 1, "  ec rw updated");
	TEST_EQ(ec_ro_protected, 1, "  ec ro protected");
	TEST_EQ(ec_ro_updated, 1, "  ec ro updated");

	ResetMocks();
	vb2_nv_set(ctx, VB2_NV_TRY_RO_SYNC, 1);
	mock_ec_ro_hash[0]++;
	vb2_nv_set(ctx, VB2_NV_DISPLAY_REQUEST, 1);
	test_ssync(0, 0, "rw update not needed");
	TEST_EQ(ec_rw_protected, 1, "  ec rw protected");
	TEST_EQ(ec_run_image, 1, "  ec run image");
	TEST_EQ(ec_rw_updated, 0, "  ec rw not updated");
	TEST_EQ(ec_ro_protected, 1, "  ec ro protected");
	TEST_EQ(ec_ro_updated, 1, "  ec ro updated");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DISPLAY_REQUEST), 1,
		"  DISPLAY_REQUEST left untouched");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	mock_ec_ro_hash[0]++;
	test_ssync(0, 0, "ro update not requested");
	TEST_EQ(ec_rw_protected, 1, "  ec rw protected");
	TEST_EQ(ec_run_image, 1, "  ec run image");
	TEST_EQ(ec_rw_updated, 1, "  ec rw updated");
	TEST_EQ(ec_ro_protected, 1, "  ec ro protected");
	TEST_EQ(ec_ro_updated, 0, "  ec ro updated");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	update_hash++;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_UPDATE, "updated hash mismatch");
	TEST_EQ(ec_rw_protected, 0, "  ec rw protected");
	TEST_EQ(ec_run_image, 0, "  ec run image");
	TEST_EQ(ec_rw_updated, 1, "  ec rw updated");
	TEST_EQ(ec_ro_protected, 0, "  ec ro protected");
	TEST_EQ(ec_ro_updated, 0, "  ec ro updated");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	update_retval = VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   0, "Reboot after rw update");
	TEST_EQ(ec_rw_updated, 1, "  ec rw updated");
	TEST_EQ(ec_ro_updated, 0, "  ec rw updated");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	update_retval = VB2_ERROR_MOCK;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_UPDATE, "Update failed");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	ctx->flags |= VB2_CONTEXT_EC_SYNC_SLOW;
	test_ssync(0, 0, "Slow update");
	TEST_EQ(screens_displayed[0], VB_SCREEN_WAIT, "  wait screen");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	ctx->flags |= VB2_CONTEXT_EC_SYNC_SLOW;
	sd->flags &= ~VB2_SD_FLAG_DISPLAY_AVAILABLE;
	test_ssync(VBERROR_REBOOT_REQUIRED, 0,
		   "Slow update - reboot for display");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	ctx->flags |= VB2_CONTEXT_EC_SYNC_SLOW;
	vb2_nv_set(ctx, VB2_NV_DISPLAY_REQUEST, 1);
	test_ssync(VB2_SUCCESS, 0,
		   "Slow update with display request");
	TEST_EQ(screens_displayed[0], VB_SCREEN_WAIT, "  wait screen");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DISPLAY_REQUEST), 1,
		"  DISPLAY_REQUEST left untouched");

	ResetMocks();
	mock_ec_rw_hash[0]++;
	ctx->flags |= VB2_CONTEXT_EC_SYNC_SLOW;
	vb2_nv_set(ctx, VB2_NV_DISPLAY_REQUEST, 0);
	test_ssync(VB2_SUCCESS, 0,
		   "Slow update without display request (no reboot needed)");
	TEST_EQ(screens_displayed[0], VB_SCREEN_WAIT, "  wait screen");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DISPLAY_REQUEST), 0,
		"  DISPLAY_REQUEST left untouched");

	/* RW cases, no update */
	ResetMocks();
	mock_in_rw = 1;
	test_ssync(0, 0, "AP-RW, EC-RW");

	ResetMocks();
	test_ssync(0, 0, "AP-RW, EC-RO -> EC-RW");
	TEST_EQ(ec_rw_protected, 1, "  ec rw protected");
	TEST_EQ(ec_run_image, 1, "  ec run image");
	TEST_EQ(ec_rw_updated, 0, "  ec rw updated");
	TEST_EQ(ec_ro_protected, 1, "  ec ro protected");
	TEST_EQ(ec_ro_updated, 0, "  ec ro updated");

	ResetMocks();
	run_retval = VB2_ERROR_MOCK;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   VB2_RECOVERY_EC_JUMP_RW, "Jump to RW fail");

	ResetMocks();
	run_retval = VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	test_ssync(VBERROR_EC_REBOOT_TO_RO_REQUIRED,
		   0, "Jump to RW fail because locked");

	ResetMocks();
	protect_retval = VB2_ERROR_MOCK;
	test_ssync(VB2_ERROR_MOCK,
		   VB2_RECOVERY_EC_PROTECT, "Protect error");

	/* No longer check for shutdown requested */
	ResetMocks();
	shutdown_request_calls_left = 0;
	test_ssync(0, 0,
		   "AP-RW, EC-RO -> EC-RW shutdown requested");

	ResetMocks();
	mock_in_rw = 1;
	shutdown_request_calls_left = 0;
	test_ssync(0, 0, "AP-RW shutdown requested");

	/* EC sync not allowed in recovery mode */
	ResetMocks();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	test_ssync(0, 0, "No sync in recovery mode");
	TEST_EQ(ec_ro_protected, 0, "ec ro not protected");
	TEST_EQ(ec_rw_protected, 0, "ec rw not protected");
	TEST_EQ(ec_run_image, 0, "ec in ro");
	TEST_EQ(ec_ro_updated, 0, "ec ro not updated");
	TEST_EQ(ec_rw_updated, 0, "ec rw not updated");
}

int main(void)
{
	VbSoftwareSyncTest();

	return gTestSuccess ? 0 : 255;
}