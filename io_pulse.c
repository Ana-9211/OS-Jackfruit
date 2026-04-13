#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char *argv[])
{
    int seconds = 10;
    if (argc > 1) seconds = atoi(argv[1]);
    printf("io_pulse: doing I/O for %d seconds (PID=%d)\n", seconds, (int)getpid());
    fflush(stdout);
    char buf[4096]; memset(buf, 'A', sizeof(buf));
    long long count = 0; time_t start = time(NULL);
    while (time(NULL) - start < seconds) {
        FILE *f = fopen("/tmp/io_pulse_test", "w");
        if (f) { fwrite(buf, 1, sizeof(buf), f); fclose(f); }
        f = fopen("/tmp/io_pulse_test", "r");
        if (f) { fread(buf, 1, sizeof(buf), f); fclose(f); }
        count++;
    }
    printf("io_pulse: done. iterations=%lld\n", count);
    unlink("/tmp/io_pulse_test");
    return 0;
}
