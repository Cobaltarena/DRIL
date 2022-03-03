#ifndef COMMAND_H
#define COMMAND_H

#include <linux/kernel.h>
#include <linux/module.h>

#include "../mfrc.h"

struct mfrc_dev;

enum COMMAND_TYPE {
    COMMAND_READ = 0,
    COMMAND_WRITE,
    COMMAND_NOT_FOUND,
};

struct command {
    enum COMMAND_TYPE command_type;
    char **args;
    int nb_arg;
};


struct command *parse_command(const char *buf);
ssize_t process_command(struct command *command, struct mfrc_dev *mfrc_dev);
struct command *command_init(enum COMMAND_TYPE type, int nb_args);
void command_free(struct command *command);

#endif