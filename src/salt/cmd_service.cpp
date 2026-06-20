#include "cli.hpp"

extern "C" {
#include "salt/util.h"
}

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void service_usage() {
  fprintf(stderr,
          "usage: salt service <subcommand>\n"
          "  import <stratum> <name>   create a runit service for a stratum daemon\n"
          "  enable <svc>              link a service into var/service\n"
          "  start <svc>               start a service via sv\n"
          "  list                      list available services\n");
}

int cmd_service(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    service_usage();
    return 2;
  }
  const std::string &sub = args[0];

  if (sub == "import") {
    if (args.size() < 3) {
      service_usage();
      return 2;
    }
    const std::string &stratum = args[1];
    const std::string &name = args[2];
    std::string svc = stratum + "-" + name;
    std::string dir = path_join(o.root, "etc/sv/" + svc);
    if (salt_mkdirs(dir.c_str(), 0755) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      return 1;
    }
    std::string run = dir + "/run";
    std::string content = "#!/bin/sh\nexec 2>&1\nexec salt run " + stratum + " " + name + "\n";
    if (salt_write_file(run.c_str(), content.data(), content.size(), 0755) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      return 1;
    }
    printf("imported service %s; enable with: salt service enable %s\n", svc.c_str(), svc.c_str());
    return 0;
  }

  if (sub == "enable") {
    if (args.size() < 2) {
      service_usage();
      return 2;
    }
    const std::string &svc = args[1];
    std::string svdir = path_join(o.root, "etc/sv/" + svc);
    std::string servicedir = path_join(o.root, "var/service");
    if (salt_mkdirs(servicedir.c_str(), 0755) != SALT_OK) {
      fprintf(stderr, "salt: %s\n", salt_last_error());
      return 1;
    }
    std::string link = servicedir + "/" + svc;
    if (symlink(svdir.c_str(), link.c_str()) != 0 && errno != EEXIST) {
      fprintf(stderr, "salt: %s: %s\n", link.c_str(), strerror(errno));
      return 1;
    }
    printf("enabled %s; runit will pick it up if active. start with: salt service start %s\n",
           svc.c_str(), svc.c_str());
    return 0;
  }

  if (sub == "start") {
    if (args.size() < 2) {
      service_usage();
      return 2;
    }
    std::string cmd = "sv up " + args[1];
    int rc = system(cmd.c_str());
    printf("note: runit must be active for sv to manage %s\n", args[1].c_str());
    return rc == 0 ? 0 : 1;
  }

  if (sub == "list") {
    std::string dir = path_join(o.root, "etc/sv");
    DIR *d = opendir(dir.c_str());
    if (!d) {
      printf("no services\n");
      return 0;
    }
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
      printf("%s\n", de->d_name);
    }
    closedir(d);
    return 0;
  }

  service_usage();
  return 2;
}
