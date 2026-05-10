#include <dev/rtc.h>
#include <dev/time.h>
#include <dev/device.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <cpu/instr.h>
#include <errno.h>
#include <debug/log.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <stdint.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71
#define CMOS_NMI_DISABLE 0x80

#define RTC_REG_SECONDS 0x00
#define RTC_REG_MINUTES 0x02
#define RTC_REG_HOURS 0x04
#define RTC_REG_WEEKDAY 0x06
#define RTC_REG_DAY 0x07
#define RTC_REG_MONTH 0x08
#define RTC_REG_YEAR 0x09
#define RTC_REG_STATUS_A 0x0a
#define RTC_REG_STATUS_B 0x0b

#define RTC_UIP 0x80
#define RTC_STATUS_B_24H 0x02
#define RTC_STATUS_B_BINARY 0x04

extern int npf_snprintf_(char *buffer, size_t bufsz, const char *format, ...);

static device_t rtc_device;

static uint8_t rtc_cmos_read(uint8_t reg)
{
	outb(CMOS_ADDR, CMOS_NMI_DISABLE | reg);
	return inb(CMOS_DATA);
}

static int rtc_update_in_progress(void)
{
	return (rtc_cmos_read(RTC_REG_STATUS_A) & RTC_UIP) != 0;
}

static uint8_t bcd_to_bin(uint8_t v)
{
	return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

static int is_leap_year(int year)
{
	return ((year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0));
}

static int64_t days_before_year(int year)
{
	int64_t days = 0;
	for (int y = 1970; y < year; y++)
		days += is_leap_year(y) ? 366 : 365;
	return days;
}

static int days_before_month(int year, int month)
{
	static const int days_norm[] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
	};
	int days = days_norm[month - 1];
	if (month > 2 && is_leap_year(year))
		days++;
	return days;
}

static int64_t rtc_to_unix(int year, int month, int day, int hour, int minute,
						   int second)
{
	int64_t days = days_before_year(year) + days_before_month(year, month) +
				   (day - 1);
	return days * 86400LL + hour * 3600LL + minute * 60LL + second;
}

static int rtc_read_consistent(int *year, int *month, int *day, int *hour,
						   int *minute, int *second)
{
	uint8_t sec1, min1, hour1, day1, month1, year1;
	uint8_t sec2, min2, hour2, day2, month2, year2;
	uint8_t status_b;

	for (int i = 0; i < 1000000 && rtc_update_in_progress(); i++)
		__asm__ volatile("pause" ::: "memory");

	sec1 = rtc_cmos_read(RTC_REG_SECONDS);
	min1 = rtc_cmos_read(RTC_REG_MINUTES);
	hour1 = rtc_cmos_read(RTC_REG_HOURS);
	day1 = rtc_cmos_read(RTC_REG_DAY);
	month1 = rtc_cmos_read(RTC_REG_MONTH);
	year1 = rtc_cmos_read(RTC_REG_YEAR);
	status_b = rtc_cmos_read(RTC_REG_STATUS_B);

	for (int i = 0; i < 1000000 && rtc_update_in_progress(); i++)
		__asm__ volatile("pause" ::: "memory");

	sec2 = rtc_cmos_read(RTC_REG_SECONDS);
	min2 = rtc_cmos_read(RTC_REG_MINUTES);
	hour2 = rtc_cmos_read(RTC_REG_HOURS);
	day2 = rtc_cmos_read(RTC_REG_DAY);
	month2 = rtc_cmos_read(RTC_REG_MONTH);
	year2 = rtc_cmos_read(RTC_REG_YEAR);

	if (sec1 != sec2 || min1 != min2 || hour1 != hour2 || day1 != day2 ||
		month1 != month2 || year1 != year2)
		return -EAGAIN;

	if (!(status_b & RTC_STATUS_B_BINARY)) {
		sec1 = bcd_to_bin(sec1);
		min1 = bcd_to_bin(min1);
		hour1 = (uint8_t)((hour1 & 0x80) | bcd_to_bin(hour1 & 0x7f));
		day1 = bcd_to_bin(day1);
		month1 = bcd_to_bin(month1);
		year1 = bcd_to_bin(year1);
	}

	if (!(status_b & RTC_STATUS_B_24H) && (hour1 & 0x80))
		hour1 = (uint8_t)(((hour1 & 0x7f) + 12) % 24);

	int full_year = (year1 >= 70) ? (1900 + year1) : (2000 + year1);
	if (month1 < 1 || month1 > 12 || day1 < 1 || day1 > 31 || hour1 > 23 ||
		min1 > 59 || sec1 > 60 || full_year < 1970)
		return -EINVAL;

	*year = full_year;
	*month = month1;
	*day = day1;
	*hour = hour1;
	*minute = min1;
	*second = sec1;
	return 0;
}

int rtc_read_time(int64_t *sec, long *nsec)
{
	if (!sec || !nsec)
		return -EINVAL;

	for (int attempt = 0; attempt < 4; attempt++) {
		int year, month, day, hour, minute, second;
		int r = rtc_read_consistent(&year, &month, &day, &hour, &minute,
								&second);
		if (r == -EAGAIN)
			continue;
		if (r != 0)
			return r;
		*sec = rtc_to_unix(year, month, day, hour, minute, second);
		*nsec = 0;
		return 0;
	}

	return -EAGAIN;
}

static int rtc_wall_source_read(int64_t *sec, long *nsec, void *ctx)
{
	(void)ctx;
	return rtc_read_time(sec, nsec);
}

static int rtc_dev_read(void *ctx, uint64_t off, void *buf, size_t len,
						 size_t *done)
{
	(void)ctx;
	if (done)
		*done = 0;
	if (!buf)
		return -EINVAL;

	int64_t sec;
	long nsec;
	int r = rtc_read_time(&sec, &nsec);
	if (r != 0)
		return r;

	char tmp[96];
	int n = npf_snprintf(tmp, sizeof(tmp), "sec=%lld\nnsec=%ld\n",
					   (long long)sec, nsec);
	if (n < 0)
		return -EINVAL;

	size_t total = (size_t)n;
	if (total >= sizeof(tmp))
		total = sizeof(tmp) - 1;
	if (off >= total || len == 0)
		return 0;

	size_t copy = total - (size_t)off;
	if (copy > len)
		copy = len;
	memcpy(buf, tmp + off, copy);
	if (done)
		*done = copy;
	return 0;
}

int rtc_init(void)
{
	int64_t sec;
	long nsec;
	int r = rtc_read_time(&sec, &nsec);
	if (r != 0) {
		log_err("rtc", "RTC read failed: %s(%d)", errno_name(r), r);
		return r;
	}

	memset(&rtc_device, 0, sizeof(rtc_device));
	memcpy(rtc_device.name, "rtc0", 5);
	rtc_device.bus_type = DEVICE_BUS_PLATFORM;
	rtc_device.driver_data = NULL;
	r = device_register(&rtc_device);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr("/dev/rtc0", 0444, rtc_dev_read, NULL, NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	static time_source_t rtc_source = {
		.name = "rtc0",
		.read_wall = rtc_wall_source_read,
		.ctx = NULL,
	};
	r = time_register_source(&rtc_source);
	if (r != 0)
		return r;

	log_info("rtc", "rtc0 ok unix=%lld", (long long)sec);
	return 0;
}
