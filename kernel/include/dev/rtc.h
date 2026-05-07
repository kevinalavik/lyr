#ifndef _LYR_DEV_RTC_H
#define _LYR_DEV_RTC_H

#include <stdint.h>

int rtc_init(void);
int rtc_read_time(int64_t *sec, long *nsec);

#endif /* _LYR_DEV_RTC_H */
