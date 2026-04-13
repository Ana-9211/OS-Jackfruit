#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char *argv[])
{
    int seconds = 10;
    if (argc > 1) seconds = atoi(argv[1]);
    printf("cpu_hog: burning CPU for %d seconds (PID=%d)\n", seconds, (int)getpid());
    fflush(stdout);
    time_t start = time(NULL);
    volatile long long counter = 0;
    while (time(NULL) - start < seconds) counter++;
    printf("cpu_hog: done. counter=%lld\n", counter);
    return 0;
}
