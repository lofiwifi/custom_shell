#include <signal.h>
#include <unistd.h>

int
main()
{
    signal(SIGTERM, SIG_IGN);
    sleep(100);
}
