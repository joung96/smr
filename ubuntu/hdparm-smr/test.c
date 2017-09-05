#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
	printf("usage: %s offset\n", argv[0]);
	return 1;
    }

    int fd = open("/dev/sda", O_WRONLY);
    if (fd < 0) {
    	printf("Couldn't open disk for writing!\n");
    	return 2;
    }

    int i;
    off_t offset = atoll(argv[1]);
    for (i = 0; i < 1; i++) {
    	char x = 0xfd;
	int write = pwrite(fd, &x, 1, offset);
	if (write < 0) {
	    perror("Couldn't write to disk!\n");
	    return 3;
	}
    }

    return 0;
}
