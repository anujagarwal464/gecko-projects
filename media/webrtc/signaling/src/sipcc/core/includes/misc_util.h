/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __MISC_UTIL_H__
#define __MISC_UTIL_H__
unsigned long gmt_string_to_seconds(char *gmt_string, unsigned long *seconds);
long diff_current_time(unsigned long t1, unsigned long *difference);
#endif
