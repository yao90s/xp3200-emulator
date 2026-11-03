# SPDX-License-Identifier: GPL-2.0-or-later

s1c33_ss = ss.source_set()
s1c33_ss.add(when: 'CONFIG_BBK9288S', if_true: files('bbk9288s.c'))
s1c33_ss.add(when: 'CONFIG_XP3200', if_true: files('xp3200.c'))

hw_arch += {'s1c33': s1c33_ss}
