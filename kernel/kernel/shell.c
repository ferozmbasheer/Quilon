#include <string.h>
#include <stdio.h>
#include <kernel/shell.h>
#include <kernel/keyboard.h>
#include <kernel/tty.h>

static void shell_execute(const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        printf("Commands: help, clear, halt\r\n");
    } else if (strcmp(cmd, "clear") == 0) {
        terminal_initialize();
    } else if (strcmp(cmd, "halt") == 0) {
        printf("Halting.\r\n");
        asm volatile("cli; hlt");
    } else if (cmd[0] != '\0') {
        printf("Unknown command: %s\r\n", cmd);
    }
}

void shell_run(void) {
    char line[256];
    int  pos = 0;

    printf("quilon> ");

    while (1) {
        char c = keyboard_getchar();

        if (c == '\n') {
            line[pos] = '\0';
            printf("\r\n");
            shell_execute(line);
            pos = 0;
            printf("quilon> ");
        } else if (c == '\b' && pos > 0) {
            pos--;
            printf("\b \b");
        } else if (pos < 255) {
            line[pos++] = c;
            printf("%c", c);
        }
    }
}
