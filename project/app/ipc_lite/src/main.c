#include <getopt.h>
#include <stdio.h>

#include "app.h"

static void print_usage(const char *program) {
  fprintf(stderr, "Usage: %s [-c ipc_lite.ini]\n", program);
}

int main(int argc, char **argv) {
  const char *config_path = "ipc_lite.ini";
  int option = 0;

  while ((option = getopt(argc, argv, "c:h")) != -1) {
    switch (option) {
      case 'c':
        config_path = optarg;
        break;
      case 'h':
      default:
        print_usage(argv[0]);
        return option == 'h' ? 0 : 1;
    }
  }

  return ipc_lite_run(config_path);
}

