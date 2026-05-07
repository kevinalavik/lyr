#ifndef PING_H
#define PING_H

#define PING_DATA_SIZE 56
#define PING_DEFAULT_COUNT 4
#define PING_TIMEOUT_SEC 1

int ping(const char *host, int count);

#endif