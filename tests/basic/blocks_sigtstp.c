#include <signal.h>
#include <unistd.h>

int
main()
{
    signal(SIGTSTP, SIG_IGN);
    while (1);
}
