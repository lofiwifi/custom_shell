#include <signal.h>
#include <unistd.h>

int
main()
{
    signal(SIGTSTP, SIG_IGN);
    sleep(5);
}
