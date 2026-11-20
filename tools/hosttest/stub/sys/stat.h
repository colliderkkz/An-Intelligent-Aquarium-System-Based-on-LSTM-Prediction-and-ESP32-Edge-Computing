#ifndef STUB_SYS_STAT_H
#define STUB_SYS_STAT_H

/* 主机语法检查用的最小 <sys/stat.h> 替身（MSVC 没有这个头）。
 * 只声明本工程实际用到的符号。 */

struct stat {
    unsigned int st_mode;
    unsigned int st_size;
};

int stat(const char *path, struct stat *buf);
int mkdir(const char *path, int mode);

#endif
