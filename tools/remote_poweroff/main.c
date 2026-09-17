#include <coreinit/launch.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    OSLaunchTitlev(OS_TITLE_ID_REBOOT, 0, NULL);

    return 0;
}
