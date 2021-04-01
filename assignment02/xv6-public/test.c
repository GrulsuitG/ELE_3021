#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
	printf(1, "my pid is %d\n", getpid());
	printf(1, "my ppid is %d\n", getppid());
	exit();
}
