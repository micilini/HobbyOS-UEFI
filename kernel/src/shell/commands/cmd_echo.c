#include "cmd_echo.h"
#include "../../graphics/console.h"
#include "../../libc/string.h"

static const char *g_aliases_echo[] = {"eco", 0};

static int is_flag_n(const char *s)
{
    return (s && strcmp(s, "-n") == 0);
}

int cmd_echo(int argc, char **argv)
{

    int i = 1;
    int newline = 1;

    if (i < argc && is_flag_n(argv[i]))
    {
        newline = 0;
        i++;
    }

    if (i >= argc)
    {
        if (newline)
            console_write("\n");
        return 0;
    }

    for (; i < argc; i++)
    {
        const char *s = argv[i];
        if (!s)
            continue;

        size_t len = strlen(s);
        if (len >= 2)
        {
            char q0 = s[0];
            char q1 = s[len - 1];
            if ((q0 == '"' && q1 == '"') || (q0 == '\'' && q1 == '\''))
            {

                for (size_t k = 1; k < len - 1; k++)
                {
                    console_put_char(s[k]);
                }
            }
            else
            {
                console_write(s);
            }
        }
        else
        {
            console_write(s);
        }

        if (i + 1 < argc)
            console_write(" ");
    }

    if (newline)
        console_write("\n");
    return 0;
}

const char *const *cmd_echo_aliases(void) { return g_aliases_echo; }