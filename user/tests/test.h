#ifndef _TEST_H
#define _TEST_H

#include <sys/time.h>
#include <sys/types.h>

#define test_assert(x)	do {						\
	if (!(x)) {							\
		printf("%s: %s:%d:%s: assertion failed: %s\n",		\
		       test_series(), __FILE__, __LINE__,		\
		       __func__, #x);					\
		test_assert_failed();					\
	}								\
} while (0)

void test_init(const char *argv0);
const char *test_series(void);
void test_assert_failed(void);
int test_start(const char *name);
void test_end(void);
int test_failures(void);

#endif /* !_TEST_H */
