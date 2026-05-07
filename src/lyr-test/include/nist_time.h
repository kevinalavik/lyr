#ifndef NIST_TIME_H
#define NIST_TIME_H

struct nist_time {
	int mjd;

	int year;
	int month;
	int day;

	int hour;
	int minute;
	int second;

	int dst;
	int leap;
	int health;

	double ut1;
	char zone[32];
	char sync;
};

int get_nist_time(struct nist_time *out);
void print_nist_time(const struct nist_time *t);

#endif