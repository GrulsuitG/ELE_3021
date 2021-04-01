#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
	int pid = fork();
	if(pid == 0){
		for(int i =0; i<10; i++){
			printf(1,"child\n");
			yields();
		}
	}
	else{
		for(int i=0; i<10; i++){
			printf(1, "parent\n");
			yields();
		}
	}
}
