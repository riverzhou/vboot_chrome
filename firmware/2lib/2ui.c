/* Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * User interfaces for developer and recovery mode menus.
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2return_codes.h"
#include "2secdata.h"
#include "2ui.h"
#include "2ui_private.h"
#include "vboot_api.h"  /* For VB_SHUTDOWN_REQUEST_POWER_BUTTON */
#include "vboot_kernel.h"
#include "vboot_ui_legacy.h"  /* TODO(b/144969088): VbDisplayDebugInfo */

#define KEY_DELAY_MS 20  /* Delay between key scans in UI loops */

/*****************************************************************************/
/* Utility functions */

/**
 * Check GBB flags against VbExIsShutdownRequested() shutdown request,
 * and check for VB_BUTTON_POWER_SHORT_PRESS key, to determine if a
 * shutdown is required.
 *
 * @param ui		UI context pointer
 * @return VB2_REQUEST_SHUTDOWN if shutdown needed, or VB2_REQUEST_UI_CONTINUE
 */
vb2_error_t check_shutdown_request(struct vb2_ui_context *ui)
{
	struct vb2_gbb_header *gbb = vb2_get_gbb(ui->ctx);
	uint32_t shutdown_request = VbExIsShutdownRequested();

	/*
	 * Ignore power button push until after we have seen it released.
	 * This avoids shutting down immediately if the power button is still
	 * being held on startup. After we've recognized a valid power button
	 * push then don't report the event until after the button is released.
	 */
	if (shutdown_request & VB_SHUTDOWN_REQUEST_POWER_BUTTON) {
		shutdown_request &= ~VB_SHUTDOWN_REQUEST_POWER_BUTTON;
		if (ui->power_button == VB2_POWER_BUTTON_RELEASED)
			ui->power_button = VB2_POWER_BUTTON_PRESSED;
	} else {
		if (ui->power_button == VB2_POWER_BUTTON_PRESSED)
			shutdown_request |= VB_SHUTDOWN_REQUEST_POWER_BUTTON;
		ui->power_button = VB2_POWER_BUTTON_RELEASED;
	}

	if (ui->key == VB_BUTTON_POWER_SHORT_PRESS)
		shutdown_request |= VB_SHUTDOWN_REQUEST_POWER_BUTTON;

	/* If desired, ignore shutdown request due to lid closure. */
	if (gbb->flags & VB2_GBB_FLAG_DISABLE_LID_SHUTDOWN)
		shutdown_request &= ~VB_SHUTDOWN_REQUEST_LID_CLOSED;

	/*
	 * In detachables, disable shutdown due to power button.
	 * It is used for menu selection instead.
	 */
	if (DETACHABLE)
		shutdown_request &= ~VB_SHUTDOWN_REQUEST_POWER_BUTTON;

	if (shutdown_request)
		return VB2_REQUEST_SHUTDOWN;

	return VB2_REQUEST_UI_CONTINUE;
}

/*****************************************************************************/
/* Error action functions */

vb2_error_t error_exit_action(struct vb2_ui_context *ui)
{
	/*
	 * If the only difference is the error message, then just
	 * redraw the screen without the error string.
	 */
	if (ui->key && ui->error_code != VB2_UI_ERROR_NONE)
		ui->error_code = VB2_UI_ERROR_NONE;
	return VB2_REQUEST_UI_CONTINUE;
}

/*****************************************************************************/
/* Menu navigation functions */

const struct vb2_menu *get_menu(struct vb2_ui_context *ui)
{
	const struct vb2_menu *menu;
	static const struct vb2_menu empty_menu = {
		.num_items = 0,
		.items = NULL,
	};
	if (ui->state->screen->get_menu) {
		menu = ui->state->screen->get_menu(ui);
		return menu ? menu : &empty_menu;
	} else {
		return &ui->state->screen->menu;
	}
}

vb2_error_t menu_navigation_action(struct vb2_ui_context *ui)
{
	uint32_t key = ui->key;

	/* Map detachable button presses for simplicity. */
	if (DETACHABLE) {
		if (key == VB_BUTTON_VOL_UP_SHORT_PRESS)
			key = VB_KEY_UP;
		else if (key == VB_BUTTON_VOL_DOWN_SHORT_PRESS)
			key = VB_KEY_DOWN;
		else if (key == VB_BUTTON_POWER_SHORT_PRESS)
			key = VB_KEY_ENTER;
	}

	switch (key) {
	case VB_KEY_UP:
		return vb2_ui_menu_prev(ui);
	case VB_KEY_DOWN:
		return vb2_ui_menu_next(ui);
	case VB_KEY_ENTER:
		return vb2_ui_menu_select(ui);
	case VB_KEY_ESC:
		return vb2_ui_screen_back(ui);
	default:
		if (key != 0)
			VB2_DEBUG("Pressed key %#x, trusted? %d\n",
				  ui->key, ui->key_trusted);
	}

	return VB2_REQUEST_UI_CONTINUE;
}

vb2_error_t vb2_ui_menu_prev(struct vb2_ui_context *ui)
{
	int item;

	if (!DETACHABLE && ui->key == VB_BUTTON_VOL_UP_SHORT_PRESS)
		return VB2_REQUEST_UI_CONTINUE;

	item = ui->state->selected_item - 1;
	while (item >= 0 &&
	       ((1 << item) & ui->state->disabled_item_mask))
		item--;
	/* Only update if item is valid */
	if (item >= 0)
		ui->state->selected_item = item;

	return VB2_REQUEST_UI_CONTINUE;
}

vb2_error_t vb2_ui_menu_next(struct vb2_ui_context *ui)
{
	int item;
	const struct vb2_menu *menu;

	if (!DETACHABLE && ui->key == VB_BUTTON_VOL_DOWN_SHORT_PRESS)
		return VB2_REQUEST_UI_CONTINUE;

	menu = get_menu(ui);
	item = ui->state->selected_item + 1;
	while (item < menu->num_items &&
	       ((1 << item) & ui->state->disabled_item_mask))
		item++;
	/* Only update if item is valid */
	if (item < menu->num_items)
		ui->state->selected_item = item;

	return VB2_REQUEST_UI_CONTINUE;
}

vb2_error_t vb2_ui_menu_select(struct vb2_ui_context *ui)
{
	const struct vb2_menu *menu;
	const struct vb2_menu_item *menu_item;

	if (!DETACHABLE && ui->key == VB_BUTTON_POWER_SHORT_PRESS)
		return VB2_REQUEST_UI_CONTINUE;

	menu = get_menu(ui);
	if (menu->num_items == 0)
		return VB2_REQUEST_UI_CONTINUE;

	menu_item = &menu->items[ui->state->selected_item];

	if (menu_item->action) {
		VB2_DEBUG("Menu item <%s> run action\n", menu_item->text);
		return menu_item->action(ui);
	} else if (menu_item->target) {
		VB2_DEBUG("Menu item <%s> to target screen %#x\n",
			  menu_item->text, menu_item->target);
		return vb2_ui_screen_change(ui, menu_item->target);
	}

	VB2_DEBUG("Menu item <%s> no action or target screen\n",
		  menu_item->text);
	return VB2_REQUEST_UI_CONTINUE;
}

/*****************************************************************************/
/* Screen navigation functions */

vb2_error_t vb2_ui_screen_back(struct vb2_ui_context *ui)
{
	struct vb2_screen_state *tmp;

	if (ui->state && ui->state->prev) {
		tmp = ui->state->prev;
		free(ui->state);
		ui->state = tmp;
	} else {
		VB2_DEBUG("ERROR: No previous screen; ignoring\n");
	}

	return VB2_REQUEST_UI_CONTINUE;
}

static vb2_error_t default_screen_init(struct vb2_ui_context *ui)
{
	const struct vb2_menu *menu = get_menu(ui);
	ui->state->selected_item = 0;
	if (menu->num_items > 1 && menu->items[0].is_language_select)
		ui->state->selected_item = 1;
	return VB2_REQUEST_UI_CONTINUE;
}

vb2_error_t vb2_ui_screen_change(struct vb2_ui_context *ui, enum vb2_screen id)
{
	const struct vb2_screen_info *new_screen_info;
	struct vb2_screen_state *cur_state;
	int state_exists = 0;

	new_screen_info = vb2_get_screen_info(id);
	if (new_screen_info == NULL) {
		VB2_DEBUG("ERROR: Screen entry %#x not found; ignoring\n", id);
		return VB2_REQUEST_UI_CONTINUE;
	}

	/* Check to see if the screen state already exists in our stack. */
	cur_state = ui->state;
	while (cur_state != NULL) {
		if (cur_state->screen->id == id) {
			state_exists = 1;
			break;
		}
		cur_state = cur_state->prev;
	}

	if (state_exists) {
		/* Pop until the requested screen is at the top of stack. */
		while (ui->state->screen->id != id) {
			cur_state = ui->state;
			ui->state = cur_state->prev;
			free(cur_state);
		}
	} else {
		/* Allocate the requested screen on top of the stack. */
		cur_state = malloc(sizeof(*ui->state));
		memset(cur_state, 0, sizeof(*ui->state));
		if (cur_state == NULL) {
			VB2_DEBUG("WARNING: malloc failed; ignoring\n");
			return VB2_REQUEST_UI_CONTINUE;
		}
		cur_state->prev = ui->state;
		cur_state->screen = new_screen_info;
		ui->state = cur_state;
		if (ui->state->screen->init)
			return ui->state->screen->init(ui);
		else
			return default_screen_init(ui);
	}

	return VB2_REQUEST_UI_CONTINUE;
}

/*****************************************************************************/
/* Core UI loop */

vb2_error_t ui_loop(struct vb2_context *ctx, enum vb2_screen root_screen_id,
		    vb2_error_t (*global_action)(struct vb2_ui_context *ui))
{
	struct vb2_ui_context ui;
	struct vb2_screen_state prev_state;
	enum vb2_ui_error prev_error_code;
	const struct vb2_menu *menu;
	const struct vb2_screen_info *root_info;
	uint32_t key_flags;
	vb2_error_t rv;

	memset(&ui, 0, sizeof(ui));
	ui.ctx = ctx;
	root_info = vb2_get_screen_info(root_screen_id);
	if (root_info == NULL)
		VB2_DIE("Root screen not found.\n");
	ui.locale_id = vb2_nv_get(ctx, VB2_NV_LOCALIZATION_INDEX);
	rv = vb2_ui_screen_change(&ui, root_screen_id);
	if (rv != VB2_REQUEST_UI_CONTINUE)
		return rv;
	memset(&prev_state, 0, sizeof(prev_state));
	prev_error_code = VB2_UI_ERROR_NONE;

	while (1) {
		/* Draw if there are state changes. */
		if (memcmp(&prev_state, ui.state, sizeof(*ui.state)) ||
		    /* we want to redraw/beep on a transition */
		    prev_error_code != ui.error_code) {

			menu = get_menu(&ui);
			VB2_DEBUG("<%s> menu item <%s>\n",
				  ui.state->screen->name,
				  menu->num_items ?
				  menu->items[ui.state->selected_item].text :
				  "null");
			vb2ex_display_ui(ui.state->screen->id, ui.locale_id,
					 ui.state->selected_item,
					 ui.state->disabled_item_mask,
					 ui.error_code);
			/*
			 * Only beep if we're transitioning from no
			 * error to an error.
			 */
			if (prev_error_code == VB2_UI_ERROR_NONE &&
			    ui.error_code != VB2_UI_ERROR_NONE)
				vb2ex_beep(250, 400);

			/* Update prev variables. */
			memcpy(&prev_state, ui.state, sizeof(*ui.state));
			prev_error_code = ui.error_code;
		}

		/* Grab new keyboard input. */
		ui.key = VbExKeyboardReadWithFlags(&key_flags);
		ui.key_trusted = !!(key_flags & VB_KEY_FLAG_TRUSTED_KEYBOARD);

		/* Check for shutdown request. */
		rv = check_shutdown_request(&ui);
		if (rv != VB2_REQUEST_UI_CONTINUE) {
			VB2_DEBUG("Shutdown requested!\n");
			return rv;
		}

		/* Check if we need to exit an error box. */
		rv = error_exit_action(&ui);
		if (rv != VB2_REQUEST_UI_CONTINUE)
			return rv;

		/* Run screen action. */
		if (ui.state->screen->action) {
			rv = ui.state->screen->action(&ui);
			if (rv != VB2_REQUEST_UI_CONTINUE)
				return rv;
		}

		/* Run menu navigation action. */
		rv = menu_navigation_action(&ui);
		if (rv != VB2_REQUEST_UI_CONTINUE)
			return rv;

		/* Run global action function if available. */
		if (global_action) {
			rv = global_action(&ui);
			if (rv != VB2_REQUEST_UI_CONTINUE)
				return rv;
		}

		/* Delay. */
		vb2ex_msleep(KEY_DELAY_MS);
	}

	return VB2_SUCCESS;
}

/*****************************************************************************/
/* Developer mode */

vb2_error_t vb2_developer_menu(struct vb2_context *ctx)
{
	return ui_loop(ctx, VB2_SCREEN_DEVELOPER_MODE, developer_action);
}

vb2_error_t developer_action(struct vb2_ui_context *ui)
{
	/* Developer mode keyboard shortcuts */
	if (ui->key == VB_KEY_CTRL('S'))
		return vb2_ui_screen_change(ui, VB2_SCREEN_DEVELOPER_TO_NORM);
	if (ui->key == VB_KEY_CTRL('U') ||
	    (DETACHABLE && ui->key == VB_BUTTON_VOL_UP_LONG_PRESS))
		return vb2_ui_developer_mode_boot_external_action(ui);
	if (ui->key == VB_KEY_CTRL('D') ||
	    (DETACHABLE && ui->key == VB_BUTTON_VOL_DOWN_LONG_PRESS))
		return vb2_ui_developer_mode_boot_internal_action(ui);

	/* TODO(b/144969088): Re-implement as debug info screen. */
	if (ui->key == '\t')
		VbDisplayDebugInfo(ui->ctx);

	return VB2_REQUEST_UI_CONTINUE;
}

/*****************************************************************************/
/* Broken recovery */

vb2_error_t vb2_broken_recovery_menu(struct vb2_context *ctx)
{
	return ui_loop(ctx, VB2_SCREEN_RECOVERY_BROKEN, broken_recovery_action);
}

vb2_error_t broken_recovery_action(struct vb2_ui_context *ui)
{
	/* TODO(b/144969088): Re-implement as debug info screen. */
	if (ui->key == '\t')
		VbDisplayDebugInfo(ui->ctx);

	return VB2_REQUEST_UI_CONTINUE;
}

/*****************************************************************************/
/* Manual recovery */

vb2_error_t vb2_manual_recovery_menu(struct vb2_context *ctx)
{
	return ui_loop(ctx, VB2_SCREEN_RECOVERY_SELECT, manual_recovery_action);
}

vb2_error_t manual_recovery_action(struct vb2_ui_context *ui)
{
	/* See if we have a recovery kernel available yet. */
	vb2_error_t rv = VbTryLoadKernel(ui->ctx, VB_DISK_FLAG_REMOVABLE);
	if (rv == VB2_SUCCESS)
		return rv;

	/* If disk validity state changed, switch to appropriate screen. */
	if (ui->recovery_rv != rv) {
		VB2_DEBUG("Recovery VbTryLoadKernel %#x --> %#x\n",
			  ui->recovery_rv, rv);
		ui->recovery_rv = rv;
		return vb2_ui_screen_change(ui,
					    rv == VB2_ERROR_LK_NO_DISK_FOUND ?
					    VB2_SCREEN_RECOVERY_SELECT :
					    VB2_SCREEN_RECOVERY_INVALID);
	}

	/* Manual recovery keyboard shortcuts */
	if (ui->key == VB_KEY_CTRL('D') ||
	    (DETACHABLE && ui->key == VB_BUTTON_VOL_UP_DOWN_COMBO_PRESS))
		return vb2_ui_screen_change(ui, VB2_SCREEN_RECOVERY_TO_DEV);

	/* TODO(b/144969088): Re-implement as debug info screen. */
	if (ui->key == '\t')
		VbDisplayDebugInfo(ui->ctx);

	return VB2_REQUEST_UI_CONTINUE;
}
