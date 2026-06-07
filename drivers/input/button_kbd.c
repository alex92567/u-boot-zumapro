// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2023 Dzmitry Sankouski <dsankouski@gmail.com>
 */

#include <stdlib.h>
#include <dm.h>
#include <asm/global_data.h>
#include <fdtdec.h>
#include <input.h>
#include <keyboard.h>
#include <button.h>
#include <dm/device-internal.h>
#include <log.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <linux/delay.h>
#include <linux/input.h>

DECLARE_GLOBAL_DATA_PTR;

/**
 * struct button_kbd_priv - driver private data
 *
 * @input: input configuration
 * @started: true if GPIO buttons have been probed
 * @button_size: number of buttons found
 * @old_state: a pointer to old button states array. Used to determine button state change.
 */
struct button_kbd_priv {
	struct input_config *input;
	bool started;
	u32 button_size;
	u32 *old_state;
};

static int button_kbd_probe_buttons(struct udevice *dev)
{
	struct button_kbd_priv *priv = dev_get_priv(dev);
	int i = 0, state;
	struct udevice *button_gpio_devp, *next_devp;
	struct uclass *uc;

	if (priv->started)
		return 0;

	uclass_foreach_dev_probe(UCLASS_BUTTON, button_gpio_devp) {
		struct button_uc_plat *uc_plat =
			dev_get_uclass_plat(button_gpio_devp);
		/* Ignore the top-level button node */
		if (!uc_plat->label)
			continue;
		debug("Found button %s #%d - %s, probing...\n",
		      uc_plat->label, i, button_gpio_devp->name);
		i++;
	}

	if (uclass_get(UCLASS_BUTTON, &uc))
		return -ENOENT;

	/*
	 * Unbind any buttons that failed to probe so we don't iterate over
	 * them when polling.
	 */
	uclass_foreach_dev_safe(button_gpio_devp, next_devp, uc) {
		if (!(dev_get_flags(button_gpio_devp) & DM_FLAG_ACTIVATED)) {
			log_warning("Button %s failed to probe\n",
				    button_gpio_devp->name);
			device_unbind(button_gpio_devp);
		}
	}

	priv->button_size = i;
	priv->old_state = calloc(i, sizeof(*priv->old_state));
	if (i && !priv->old_state)
		return -ENOMEM;

	i = 0;
	uclass_id_foreach_dev(UCLASS_BUTTON, button_gpio_devp, uc) {
		struct button_uc_plat *uc_plat =
			dev_get_uclass_plat(button_gpio_devp);

		if (!uc_plat->label)
			continue;
		if (i >= priv->button_size)
			break;

		/*
		 * Do not synthesize input for buttons already held while the
		 * keyboard starts. Phones often enter U-Boot with Power still
		 * pressed, and treating that as a fresh key aborts autoboot.
		 */
		state = button_get_state(button_gpio_devp);
		priv->old_state[i++] = state < 0 ? BUTTON_OFF : state;
	}

	priv->started = true;

	return 0;
}

static int button_kbd_start(struct udevice *dev)
{
	/*
	 * input_stdio_register() may start this device while stdio devices are
	 * still being registered. Defer GPIO probing until the first real input
	 * poll so a bad/slow GPIO path cannot hide all console output.
	 */
	if (!(gd->flags & GD_FLG_DEVINIT))
		return 0;

	return button_kbd_probe_buttons(dev);
}

int button_read_keys(struct input_config *input)
{
	struct button_kbd_priv *priv = dev_get_priv(input->dev);
	struct udevice *button_gpio_devp;
	struct uclass *uc;
	int i = 0, idx;
	int code, state;
	int ret, keys = 0;

	if (!(gd->flags & GD_FLG_DEVINIT))
		return 0;

	ret = button_kbd_probe_buttons(input->dev);
	if (ret)
		return ret;

	uclass_id_foreach_dev(UCLASS_BUTTON, button_gpio_devp, uc) {
		struct button_uc_plat *uc_plat =
			dev_get_uclass_plat(button_gpio_devp);
		/* Ignore the top-level button node */
		if (!uc_plat->label)
			continue;
		if (i >= priv->button_size)
			break;
		idx = i++;

		code = button_get_code(button_gpio_devp);
		if (code < 0)
			continue;

		state = button_get_state(button_gpio_devp);
		if (state < 0)
			continue;

		if (state != priv->old_state[idx]) {
			debug("%s: %d\n", uc_plat->label, code);
			priv->old_state[idx] = state;
			ret = input_add_keycode(input, code,
						state == BUTTON_OFF);
			if (ret > 0)
				keys += ret;
		}
	}
	return keys;
}

static const struct keyboard_ops button_kbd_ops = {
	.start	= button_kbd_start,
};

static int button_kbd_probe(struct udevice *dev)
{
	struct button_kbd_priv *priv = dev_get_priv(dev);
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct stdio_dev *sdev = &uc_priv->sdev;
	struct input_config *input = &uc_priv->input;
	int ret = 0;

	input_init(input, false);
	input_add_tables(input, false);

	/* Register the device. */
	priv->input = input;
	input->dev = dev;
	input->read_keys = button_read_keys;
	strcpy(sdev->name, "button-kbd");
	ret = input_stdio_register(sdev);
	if (ret) {
		debug("%s: input_stdio_register() failed\n", __func__);
		return ret;
	}

	return 0;
}

U_BOOT_DRIVER(button_kbd) = {
	.name		= "button_kbd",
	.id		= UCLASS_KEYBOARD,
	.ops		= &button_kbd_ops,
	.priv_auto	= sizeof(struct button_kbd_priv),
	.probe		= button_kbd_probe,
};

U_BOOT_DRVINFO(button_kbd) = {
	.name = "button_kbd"
};
