/*
 * Author: Paul Reioux aka Faux123 <reioux@gmail.com>
 *
 * WCD93xx sound control module
 * Copyright 2013 Paul Reioux
 *
 * Adapted to WCD9330 TomTom codec driver
 * Pafcholini <pafcholini@gmail.com>
 * Thanks to Thehacker911 for the tip
 *
 * max98925 speaker gain and cleanup by flar2
 * analog headphone gain by flar2 with thanks to chdloc
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kallsyms.h>
#include <linux/mfd/wcd9xxx/wcd9330_registers.h>
#include <sound/soc.h>

#define SOUND_CONTROL_MAJOR_VERSION	3
#define SOUND_CONTROL_MINOR_VERSION	0

extern struct snd_soc_codec *fauxsound_codec_ptr;

extern int speaker_gain_lval;
extern int speaker_gain_rval;

unsigned int tomtom_read(struct snd_soc_codec *codec, unsigned int reg);
int tomtom_write(struct snd_soc_codec *codec, unsigned int reg,
		unsigned int value);

static ssize_t cam_mic_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
        return sprintf(buf, "%u\n",
		tomtom_read(fauxsound_codec_ptr,
			TOMTOM_A_CDC_TX4_VOL_CTL_GAIN));

}

static ssize_t cam_mic_gain_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int lval;

	sscanf(buf, "%u", &lval);

	tomtom_write(fauxsound_codec_ptr,
		TOMTOM_A_CDC_TX4_VOL_CTL_GAIN, lval);

	return count;
}

static ssize_t mic_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n",
		tomtom_read(fauxsound_codec_ptr,
			TOMTOM_A_CDC_TX6_VOL_CTL_GAIN));
}

static ssize_t mic_gain_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int lval;

	sscanf(buf, "%u", &lval);

	tomtom_write(fauxsound_codec_ptr,
		TOMTOM_A_CDC_TX6_VOL_CTL_GAIN, lval);

	return count;

}

static ssize_t speaker_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
        return sprintf(buf, "%u %u\n",
			speaker_gain_lval,
			speaker_gain_rval);

}

static ssize_t speaker_gain_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int lval, rval;

	sscanf(buf, "%u %u", &lval, &rval);

	if (lval >= 0 && lval < 31)
		speaker_gain_lval = lval;
	else
		speaker_gain_lval = 20;

	if (rval >= 0 && rval < 31)
		speaker_gain_rval = rval;
	else
		speaker_gain_rval = 20;

	return count;
}

static ssize_t headphone_gain_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u %u\n",
			tomtom_read(fauxsound_codec_ptr,
				TOMTOM_A_CDC_RX1_VOL_CTL_B2_CTL),
			tomtom_read(fauxsound_codec_ptr,
				TOMTOM_A_CDC_RX2_VOL_CTL_B2_CTL));
}

static ssize_t headphone_gain_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int lval, rval;

	sscanf(buf, "%d %d", &lval, &rval);

	if (lval < 0)
		lval -= 256;
	else if (lval > 20)
		lval = 0;

	if (rval < 0)
		rval -= 256;
	else if (rval > 20)
		rval = 0;

	tomtom_write(fauxsound_codec_ptr,
		TOMTOM_A_CDC_RX1_VOL_CTL_B2_CTL, lval);
	tomtom_write(fauxsound_codec_ptr,
		TOMTOM_A_CDC_RX2_VOL_CTL_B2_CTL, rval);

	return count;
}

static ssize_t headphone_gain_pa_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{

	u8 hph_l_gain = snd_soc_read(fauxsound_codec_ptr, TOMTOM_A_RX_HPH_L_GAIN);
	u8 hph_r_gain = snd_soc_read(fauxsound_codec_ptr, TOMTOM_A_RX_HPH_R_GAIN);

	return sprintf(buf, "%u %u\n",
			hph_l_gain & 0x1F, hph_r_gain & 0x1F);
}

static ssize_t headphone_gain_pa_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int lval, rval;

	sscanf(buf, "%u %u", &lval, &rval);

	snd_soc_update_bits(fauxsound_codec_ptr, TOMTOM_A_RX_HPH_L_GAIN, 0x1f, lval);
	snd_soc_update_bits(fauxsound_codec_ptr, TOMTOM_A_RX_HPH_R_GAIN, 0x1f, rval);

	return count;
}

static struct kobj_attribute cam_mic_gain_attribute =
	__ATTR(cam_mic_gain,
		0666,
		cam_mic_gain_show,
		cam_mic_gain_store);

static struct kobj_attribute mic_gain_attribute =
	__ATTR(mic_gain,
		0666,
		mic_gain_show,
		mic_gain_store);

static struct kobj_attribute speaker_gain_attribute =
	__ATTR(speaker_gain,
		0666,
		speaker_gain_show,
		speaker_gain_store);

static struct kobj_attribute headphone_gain_attribute =
	__ATTR(headphone_gain,
		0666,
		headphone_gain_show,
		headphone_gain_store);

static struct kobj_attribute headphone_pa_gain_attribute =
	__ATTR(headphone_pa_gain,
		0666,
		headphone_gain_pa_show,
		headphone_gain_pa_store);

static struct attribute *sound_control_attrs[] =
	{
		&cam_mic_gain_attribute.attr,
		&mic_gain_attribute.attr,
		&speaker_gain_attribute.attr,
		&headphone_gain_attribute.attr,
		&headphone_pa_gain_attribute.attr,
		NULL,
	};

static struct attribute_group sound_control_attr_group =
	{
		.attrs = sound_control_attrs,
	};

static struct kobject *sound_control_kobj;

static int sound_control_init(void)
{
	int sysfs_result;

	sound_control_kobj =
		kobject_create_and_add("sound_control", kernel_kobj);

	if (!sound_control_kobj) {
		pr_err("%s sound_control_kobj create failed!\n",
			__FUNCTION__);
		return -ENOMEM;
        }

	sysfs_result = sysfs_create_group(sound_control_kobj,
			&sound_control_attr_group);

	if (sysfs_result) {
		pr_info("%s sysfs create failed!\n", __FUNCTION__);
		kobject_put(sound_control_kobj);
	}
	return sysfs_result;
}

static void sound_control_exit(void)
{
	if (sound_control_kobj != NULL)
		kobject_put(sound_control_kobj);
}

module_init(sound_control_init);
module_exit(sound_control_exit);
MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("Sound Control Module 3.x");

