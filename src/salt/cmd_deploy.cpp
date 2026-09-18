#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/db.h"
#include "salt/txn.h"
#include "salt/deploy.h"
#include "salt/toml.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct BootConf {
  std::string loader = "none";
  std::string root_label;
  std::string root_subvol = "@";
  std::string snapshots_subvol = "@snapshots";
  std::string cmdline;
  std::string title = "saltOS";
  std::string serial;
  std::string grub_cfg = "/boot/grub/grub.cfg";
  std::string grubenv = "/boot/efi/EFI/saltos/grubenv";
  std::string grubenv_label = "saltos-esp";
  std::string tryboot_tool = "/usr/lib/saltos/ab-update.sh";
  long long timeout = 3;
  long long max_snapshots = 10;
};

BootConf load_boot_conf(const std::string &root) {
  BootConf c;
  std::string path = path_join(root, "etc/salt/boot.conf");
  salt_toml *t = salt_toml_parse_file(path.c_str());
  if (!t) return c;
  auto str = [&](const char *k, std::string &dst) {
    const char *v = salt_toml_string(t, k, nullptr);
    if (v) dst = v;
  };
  str("boot.loader", c.loader);
  str("boot.root_label", c.root_label);
  str("boot.root_subvol", c.root_subvol);
  str("boot.snapshots_subvol", c.snapshots_subvol);
  str("boot.cmdline", c.cmdline);
  str("boot.title", c.title);
  str("boot.serial", c.serial);
  str("boot.grub_cfg", c.grub_cfg);
  str("boot.grubenv", c.grubenv);
  str("boot.grubenv_label", c.grubenv_label);
  str("boot.tryboot_tool", c.tryboot_tool);
  c.timeout = salt_toml_int(t, "boot.timeout", c.timeout);
  c.max_snapshots = salt_toml_int(t, "boot.max_snapshots", c.max_snapshots);
  salt_toml_free(t);
  return c;
}

long long deploy_keep(const std::string &root) {
  std::string conf = path_join(root, "etc/salt/salt.conf");
  long long keep = 5;
  salt_toml *t = salt_toml_parse_file(conf.c_str());
  if (t) {
    keep = salt_toml_int(t, "deploy.keep", keep);
    salt_toml_free(t);
  }
  return keep < 1 ? 1 : keep;
}

std::string kernel_release(const std::string &file) {
  size_t dash = file.find('-');
  return dash == std::string::npos ? "" : file.substr(dash + 1);
}

std::string running_kernel(const std::string &bootdir) {
  struct utsname u;
  if (uname(&u) != 0) return "";
  salt_strlist l;
  salt_strlist_init(&l);
  salt_boot_list_kernels(bootdir.c_str(), &l);
  std::string found;
  for (size_t i = 0; i < l.len; i++) {
    if (kernel_release(l.items[i]) == u.release) {
      found = l.items[i];
      break;
    }
  }
  salt_strlist_free(&l);
  return found;
}

std::string initrd_for(const std::string &bootdir, const std::string &kernel) {
  std::string rel = kernel_release(kernel);
  const char *cands[] = {"initramfs-%s.img", "initrd.img-%s", "initrd-%s", "initramfs-%s", nullptr};
  for (int i = 0; cands[i]; i++) {
    char buf[512];
    snprintf(buf, sizeof(buf), cands[i], rel.c_str());
    if (salt_path_exists(path_join(bootdir, buf).c_str())) return buf;
  }
  return "";
}

std::map<std::string, std::string> grubenv_read(const std::string &path) {
  std::map<std::string, std::string> kv;
  salt_buf b;
  if (salt_read_file(path.c_str(), &b) != SALT_OK) return kv;
  std::string s(b.data, b.len);
  salt_buf_free(&b);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? s.size() : nl + 1;
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return kv;
}

int grubenv_write(const std::string &path, const std::map<std::string, std::string> &kv) {
  std::string s = "# GRUB Environment Block\n";
  for (auto &e : kv) s += e.first + "=" + e.second + "\n";
  if (s.size() > 1024) {
    fprintf(stderr, "salt: grubenv block is full\n");
    return 1;
  }
  s.resize(1024, '#');
  std::string dir = path.substr(0, path.rfind('/'));
  salt_mkdirs(dir.c_str(), 0755);
  if (salt_write_file(path.c_str(), s.data(), s.size(), 0644) != SALT_OK) {
    fprintf(stderr, "salt: cannot write %s: %s\n", path.c_str(), salt_last_error());
    return 1;
  }
  return 0;
}

std::string grubenv_path(const Options &o, const BootConf &c) {
  return path_join(o.root, c.grubenv.c_str() + (c.grubenv[0] == '/' ? 1 : 0));
}

std::string entry_id(const std::string &kernel) {
  return "k-" + kernel;
}

struct SnapEntry {
  int64_t id;
  int64_t time;
  std::string kernel;
  std::string initrd;
};

std::string fmt_time(int64_t t) {
  char ts[64];
  time_t tt = (time_t)t;
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", localtime(&tt));
  return ts;
}

int64_t current_deployment(salt_db *db) {
  salt_deployment_list l;
  salt_deployment_list_init(&l);
  salt_db_deployments(db, &l);
  int64_t cur = 0;
  for (size_t i = 0; i < l.len && !cur; i++)
    if (strcmp(l.items[i].status, "ok") == 0) cur = l.items[i].id;
  salt_deployment_list_free(&l);
  return cur;
}

std::vector<SnapEntry> snapshot_entries(const std::string &snapdir, salt_db *db, long long max) {
  std::vector<SnapEntry> out;
  salt_deployment_list l;
  salt_deployment_list_init(&l);
  salt_db_deployments(db, &l);
  for (size_t i = 0; i < l.len && (long long)out.size() < max; i++) {
    const salt_deployment &d = l.items[i];
    if (!d.snapshot || strncmp(d.snapshot, "root-", 5) != 0) continue;
    std::string boot = path_join(path_join(snapdir, d.snapshot), "boot");
    char *k = salt_boot_newest_kernel(boot.c_str());
    if (!k) continue;
    SnapEntry e;
    e.id = d.id;
    e.time = d.time;
    e.kernel = k;
    e.initrd = initrd_for(boot, e.kernel);
    free(k);
    out.push_back(e);
  }
  salt_deployment_list_free(&l);
  return out;
}

std::string grub_entry(const BootConf &c, const std::string &title, const std::string &id,
                       const std::string &subvol, const std::string &kernel,
                       const std::string &initrd) {
  std::string s;
  s += "menuentry \"" + title + "\" --id " + id + " {\n";
  s += "  search --no-floppy --set=root --label " + c.root_label + "\n";
  s += "  linux /" + subvol + "/boot/" + kernel + " root=LABEL=" + c.root_label +
       " rootflags=subvol=" + subvol + " rootfstype=btrfs " + c.cmdline + "\n";
  if (!initrd.empty()) s += "  initrd /" + subvol + "/boot/" + initrd + "\n";
  s += "}\n";
  return s;
}

int write_grub_cfg(const BootConf &c, salt_db *db, const std::string &rootdir,
                   const std::string &snapdir, const std::string &default_kernel,
                   const std::string &try_kernel) {
  std::string bootdir = path_join(rootdir, "boot");
  salt_strlist kernels;
  salt_strlist_init(&kernels);
  salt_boot_list_kernels(bootdir.c_str(), &kernels);
  if (kernels.len == 0) {
    salt_strlist_free(&kernels);
    fprintf(stderr, "salt: no kernel found in %s\n", bootdir.c_str());
    return 1;
  }
  int64_t cur = current_deployment(db);
  std::string s;
  s += "set timeout=" + std::to_string(c.timeout) + "\n";
  s += "insmod all_video\ninsmod gfxterm\ninsmod part_gpt\ninsmod btrfs\ninsmod fat\ninsmod loadenv\n";
  if (!c.serial.empty())
    s += "serial " + c.serial + "\nterminal_input console serial\nterminal_output console serial\n";
  s += "set saltos_default=\"" + entry_id(default_kernel) + "\"\n";
  s += "set saltos_try=0\nset saltos_try_entry=\"" + (try_kernel.empty() ? std::string() : entry_id(try_kernel)) +
       "\"\n";
  std::string envrel = c.grubenv;
  std::string esp_prefix = "/boot/efi";
  if (envrel.compare(0, esp_prefix.size(), esp_prefix) == 0) envrel = envrel.substr(esp_prefix.size());
  s += "search --no-floppy --set=saltos_env --label " + c.grubenv_label + "\n";
  s += "if [ -n \"$saltos_env\" ]; then\n";
  s += "  load_env -f ($saltos_env)" + envrel + " saltos_default saltos_try saltos_try_entry\n";
  s += "fi\n";
  s += "set default=\"$saltos_default\"\n";
  s += "if [ \"$saltos_try\" = \"1\" -a -n \"$saltos_try_entry\" ]; then\n";
  s += "  set saltos_try=0\n";
  s += "  if [ -n \"$saltos_env\" ]; then save_env -f ($saltos_env)" + envrel + " saltos_try; fi\n";
  s += "  set default=\"$saltos_try_entry\"\n";
  s += "fi\n";
  for (size_t i = 0; i < kernels.len; i++) {
    std::string k = kernels.items[i];
    std::string title = c.title + " - kernel " + kernel_release(k) + " (deployment " +
                        std::to_string((long long)cur) + ")";
    s += grub_entry(c, title, entry_id(k), c.root_subvol, k, initrd_for(bootdir, k));
  }
  std::vector<SnapEntry> snaps = snapshot_entries(snapdir, db, c.max_snapshots);
  if (!snaps.empty()) {
    s += "submenu \"Previous deployments\" --id snapshots {\n";
    for (auto &e : snaps) {
      std::string title = c.title + " - deployment " + std::to_string((long long)e.id) + " (" +
                          fmt_time(e.time) + ", kernel " + kernel_release(e.kernel) + ")";
      std::string sub = c.snapshots_subvol + "/root-" + std::to_string((long long)e.id);
      s += grub_entry(c, title, "s" + std::to_string((long long)e.id), sub, e.kernel, e.initrd);
    }
    s += "}\n";
  }
  salt_strlist_free(&kernels);
  std::string cfg = path_join(rootdir, c.grub_cfg.c_str() + 1);
  std::string dir = cfg.substr(0, cfg.rfind('/'));
  salt_mkdirs(dir.c_str(), 0755);
  if (salt_write_file(cfg.c_str(), s.data(), s.size(), 0644) != SALT_OK) {
    fprintf(stderr, "salt: cannot write %s: %s\n", cfg.c_str(), salt_last_error());
    return 1;
  }
  return 0;
}

int tryboot_tool(const BootConf &c, const Options &o, const char *verb, const std::string &arg) {
  std::string cmd = "'" + c.tryboot_tool + "' " + verb;
  if (!arg.empty()) cmd += " '" + arg + "'";
  if (o.root != "/") cmd = "SALTOS_STAGE_ROOT='" + o.root + "' " + cmd;
  int rc = system(cmd.c_str());
  return rc == 0 ? 0 : 1;
}

std::string kernel_from_entry(const std::string &id) {
  return id.compare(0, 2, "k-") == 0 ? id.substr(2) : "";
}

struct BootState {
  std::string default_kernel;
  std::string pending_kernel;
  bool try_armed = false;
};

BootState grub_state(const Options &o, const BootConf &c, const std::string &bootdir) {
  BootState st;
  auto kv = grubenv_read(grubenv_path(o, c));
  st.default_kernel = kernel_from_entry(kv.count("saltos_default") ? kv["saltos_default"] : "");
  st.pending_kernel = kv.count("saltos_pending") ? kv["saltos_pending"] : "";
  st.try_armed = kv.count("saltos_try") && kv["saltos_try"] == "1";
  if (!st.default_kernel.empty() &&
      !salt_path_exists(path_join(bootdir, st.default_kernel).c_str()))
    st.default_kernel.clear();
  if (!st.pending_kernel.empty() &&
      !salt_path_exists(path_join(bootdir, st.pending_kernel).c_str()))
    st.pending_kernel.clear();
  return st;
}

int grub_update(const Options &o, const BootConf &c, salt_db *db, const std::string &rootdir,
                const std::string &snapdir, bool after_rollback) {
  std::string bootdir = path_join(rootdir, "boot");
  char *newest_c = salt_boot_newest_kernel(bootdir.c_str());
  if (!newest_c) {
    fprintf(stderr, "salt: no kernel found in %s\n", bootdir.c_str());
    return 1;
  }
  std::string newest = newest_c;
  free(newest_c);
  BootState st = grub_state(o, c, bootdir);
  std::string running = o.root == "/" ? running_kernel(bootdir) : "";
  std::string def = st.default_kernel;
  if (after_rollback)
    def = newest;
  else if (def.empty())
    def = running.empty() ? newest : running;
  std::string pending;
  if (!after_rollback && newest != def) pending = newest;
  std::map<std::string, std::string> kv;
  kv["saltos_default"] = entry_id(def);
  kv["saltos_try"] = pending.empty() ? "0" : "1";
  kv["saltos_try_entry"] = pending.empty() ? "" : entry_id(pending);
  kv["saltos_pending"] = pending;
  int rc = write_grub_cfg(c, db, rootdir, snapdir, def, pending);
  if (rc) return rc;
  std::string envp = grubenv_path(o, c);
  std::string espdir = envp.substr(0, envp.rfind('/'));
  struct stat sb;
  if (o.root != "/" && stat(espdir.c_str(), &sb) != 0) {
    printf("boot: wrote %s (grubenv %s not present under --root, skipped)\n", c.grub_cfg.c_str(),
           c.grubenv.c_str());
    return 0;
  }
  rc = grubenv_write(envp, kv);
  if (rc) return rc;
  if (!pending.empty())
    printf("boot: kernel %s will be tried once on next boot (fallback %s); run `salt-ota confirm` after booting\n",
           kernel_release(pending).c_str(), kernel_release(def).c_str());
  else
    printf("boot: default kernel %s\n", kernel_release(def).c_str());
  return 0;
}

int grub_confirm(const Options &o, const BootConf &c, salt_db *db) {
  std::string bootdir = path_join(o.root, "boot");
  BootState st = grub_state(o, c, bootdir);
  if (st.pending_kernel.empty()) {
    printf("boot: nothing to confirm (default kernel %s)\n", kernel_release(st.default_kernel).c_str());
    return 0;
  }
  std::string running = running_kernel(bootdir);
  if (running != st.pending_kernel) {
    fprintf(stderr, "salt: running kernel %s is not the pending kernel %s; not confirming\n",
            running.empty() ? "(unknown)" : kernel_release(running).c_str(),
            kernel_release(st.pending_kernel).c_str());
    return 1;
  }
  std::map<std::string, std::string> kv;
  kv["saltos_default"] = entry_id(running);
  kv["saltos_try"] = "0";
  kv["saltos_try_entry"] = "";
  kv["saltos_pending"] = "";
  int rc = write_grub_cfg(c, db, o.root, path_join(o.root, ".snapshots"), running, "");
  if (rc) return rc;
  rc = grubenv_write(grubenv_path(o, c), kv);
  if (rc) return rc;
  printf("boot: confirmed kernel %s as default\n", kernel_release(running).c_str());
  return 0;
}

int grub_try(const Options &o, const BootConf &c, salt_db *db) {
  std::string bootdir = path_join(o.root, "boot");
  BootState st = grub_state(o, c, bootdir);
  char *newest_c = salt_boot_newest_kernel(bootdir.c_str());
  std::string newest = newest_c ? newest_c : "";
  free(newest_c);
  if (newest.empty() || newest == st.default_kernel) {
    printf("boot: no newer kernel to try (default %s)\n", kernel_release(st.default_kernel).c_str());
    return 0;
  }
  std::map<std::string, std::string> kv;
  kv["saltos_default"] = entry_id(st.default_kernel.empty() ? newest : st.default_kernel);
  kv["saltos_try"] = "1";
  kv["saltos_try_entry"] = entry_id(newest);
  kv["saltos_pending"] = newest;
  int rc = write_grub_cfg(c, db, o.root, path_join(o.root, ".snapshots"),
                          st.default_kernel.empty() ? newest : st.default_kernel, newest);
  if (rc) return rc;
  rc = grubenv_write(grubenv_path(o, c), kv);
  if (rc) return rc;
  printf("boot: kernel %s armed for one boot\n", kernel_release(newest).c_str());
  return 0;
}

int grub_status(const Options &o, const BootConf &c) {
  std::string bootdir = path_join(o.root, "boot");
  BootState st = grub_state(o, c, bootdir);
  std::string running = running_kernel(bootdir);
  printf("loader:   grub\n");
  printf("running:  %s\n", running.empty() ? "(unknown)" : kernel_release(running).c_str());
  printf("default:  %s\n", st.default_kernel.empty() ? "(unset)" : kernel_release(st.default_kernel).c_str());
  printf("pending:  %s%s\n", st.pending_kernel.empty() ? "none" : kernel_release(st.pending_kernel).c_str(),
         st.pending_kernel.empty() ? "" : (st.try_armed ? " (armed for next boot)" : " (booted, awaiting confirm)"));
  return st.pending_kernel.empty() ? 0 : 3;
}

salt_db *open_db_at(const std::string &db_path) {
  salt_db *db = nullptr;
  if (salt_db_open(db_path.c_str(), &db) != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    return nullptr;
  }
  return db;
}

int boot_update_root(const Options &o, salt_db *db, const std::string &rootdir,
                     const std::string &snapdir, bool after_rollback) {
  BootConf c = load_boot_conf(rootdir);
  if (c.loader == "grub") return grub_update(o, c, db, rootdir, snapdir, after_rollback);
  if (c.loader == "tryboot") return tryboot_tool(c, o, "kernel-update", after_rollback ? "rollback" : "");
  return 0;
}

}  // namespace

int deploy_post_txn(const Options &o, salt_ctx *ctx, salt_db *db, int64_t txn_id) {
  salt_deploy_record(ctx, db, txn_id);
  bool boot_changed = false;
  salt_deploy_root_changed(ctx, txn_id, "boot/", &boot_changed);
  int rc = 0;
  std::string snapdir = ctx->snapshot_dir;
  if (boot_changed) rc = boot_update_root(o, db, o.root, snapdir, false);
  salt_strlist removed;
  salt_strlist_init(&removed);
  salt_deploy_prune(ctx, db, (int)deploy_keep(o.root), &removed);
  if (removed.len) {
    BootConf c = load_boot_conf(o.root);
    if (c.loader == "grub" && !boot_changed) grub_update(o, c, db, o.root, snapdir, false);
  }
  salt_strlist_free(&removed);
  return rc;
}

int cmd_deployments(const Options &o, const std::vector<std::string> &args) {
  (void)args;
  salt_db *db = open_db_at(db_path_for(o));
  if (!db) return 1;
  salt_deployment_list l;
  salt_deployment_list_init(&l);
  salt_db_deployments(db, &l);
  int64_t cur = current_deployment(db);
  printf("   %-5s %-17s %-9s %-7s %-18s %s\n", "ID", "DATE", "OP", "STATUS", "KERNEL", "SNAPSHOT");
  for (size_t i = 0; i < l.len; i++) {
    const salt_deployment &d = l.items[i];
    salt_txn_meta m;
    salt_deploy_meta(db, d.id, &m);
    std::string kernel = m.kernel ? kernel_release(m.kernel) : "-";
    if (kernel.empty()) kernel = m.kernel;
    printf("%c%c %-5lld %-17s %-9s %-7s %-18s %s\n", d.id == cur ? '*' : ' ', m.pinned ? 'P' : ' ',
           (long long)d.id, fmt_time(d.time).c_str(), d.op ? d.op : "", d.status ? d.status : "",
           kernel.c_str(), d.snapshot && d.snapshot[0] ? d.snapshot : "-");
    salt_txn_change_list ch;
    salt_txn_change_list_init(&ch);
    salt_deploy_changes(db, d.id, &ch);
    for (size_t j = 0; j < ch.len; j++) {
      const salt_txn_change &c = ch.items[j];
      if (c.old_version && c.new_version)
        printf("       %s %s -> %s\n", c.name, c.old_version, c.new_version);
      else if (c.new_version)
        printf("       + %s %s\n", c.name, c.new_version);
      else if (c.old_version)
        printf("       - %s %s\n", c.name, c.old_version);
    }
    salt_txn_change_list_free(&ch);
    salt_txn_meta_free(&m);
  }
  if (l.len == 0) printf("no deployments\n");
  salt_deployment_list_free(&l);
  salt_db_close(db);
  return 0;
}

int cmd_rollback(const Options &o, const std::vector<std::string> &args) {
  int64_t target = 0;
  if (!args.empty()) {
    char *end = nullptr;
    target = strtoll(args[0].c_str(), &end, 10);
    if (!end || *end || target <= 0) {
      fprintf(stderr, "usage: salt rollback [N]\n");
      return 2;
    }
  }
  salt_ctx ctx;
  salt_ctx_init(&ctx, o.root.c_str());
  salt_db *db = open_db_at(ctx.db_path);
  if (!db) {
    salt_ctx_free(&ctx);
    return 1;
  }
  int64_t rb = 0;
  char *new_root = nullptr;
  bool reboot = false;
  int rc = salt_rollback_to(&ctx, db, target, &rb, &new_root, &reboot);
  if (rc != SALT_OK) {
    fprintf(stderr, "salt: %s\n", salt_last_error());
    salt_db_close(db);
    salt_ctx_free(&ctx);
    return 1;
  }
  if (new_root) {
    std::string nr = new_root;
    std::string top = nr.substr(0, nr.rfind('/'));
    salt_db *ndb = open_db_at(path_join(nr, "var/lib/salt/db.sqlite"));
    if (ndb) {
      BootConf c = load_boot_conf(nr);
      boot_update_root(o, ndb, nr, path_join(top, c.snapshots_subvol), true);
      salt_db_close(ndb);
    }
    salt_btrfs_umount_toplevel(top.c_str());
    free(new_root);
    printf("rolled back to deployment %lld as deployment %lld; reboot to activate\n",
           (long long)(target ? target : rb), (long long)rb);
  } else {
    boot_update_root(o, db, o.root, ctx.snapshot_dir, true);
    printf("rolled back to deployment %lld as deployment %lld\n", (long long)(target ? target : rb),
           (long long)rb);
  }
  salt_db_close(db);
  salt_ctx_free(&ctx);
  return reboot ? 3 : 0;
}

int cmd_pin(const Options &o, const std::vector<std::string> &args) {
  bool unpin = false;
  std::vector<std::string> rest;
  for (auto &a : args) {
    if (a == "--unpin" || a == "-u")
      unpin = true;
    else
      rest.push_back(a);
  }
  if (rest.size() != 1) {
    fprintf(stderr, "usage: salt pin [--unpin] <N>\n");
    return 2;
  }
  char *end = nullptr;
  long long id = strtoll(rest[0].c_str(), &end, 10);
  if (!end || *end || id <= 0) {
    fprintf(stderr, "usage: salt pin [--unpin] <N>\n");
    return 2;
  }
  salt_db *db = open_db_at(db_path_for(o));
  if (!db) return 1;
  int rc = salt_deploy_pin(db, id, !unpin);
  if (rc != SALT_OK)
    fprintf(stderr, "salt: %s\n", salt_last_error());
  else
    printf("deployment %lld %s\n", id, unpin ? "unpinned" : "pinned (kept by pruning)");
  salt_db_close(db);
  return rc == SALT_OK ? 0 : 1;
}

int cmd_boot(const Options &o, const std::vector<std::string> &args) {
  std::string sub = args.empty() ? "status" : args[0];
  BootConf c = load_boot_conf(o.root);
  if (c.loader == "none") {
    if (sub == "status") {
      printf("loader:   none (no /etc/salt/boot.conf)\n");
      return 0;
    }
    fprintf(stderr, "salt: no boot loader configured in /etc/salt/boot.conf\n");
    return 1;
  }
  if (c.loader == "tryboot") {
    if (sub == "update") return tryboot_tool(c, o, "kernel-update", "");
    if (sub == "try") return tryboot_tool(c, o, "kernel-try", "");
    if (sub == "confirm") return tryboot_tool(c, o, "kernel-confirm", "");
    if (sub == "status") return tryboot_tool(c, o, "kernel-status", "");
    fprintf(stderr, "usage: salt boot [status|update|try|confirm]\n");
    return 2;
  }
  if (c.loader != "grub") {
    fprintf(stderr, "salt: unknown boot loader '%s' in boot.conf\n", c.loader.c_str());
    return 1;
  }
  if (sub == "status") return grub_status(o, c);
  salt_db *db = open_db_at(db_path_for(o));
  if (!db) return 1;
  int rc;
  if (sub == "update")
    rc = grub_update(o, c, db, o.root, path_join(o.root, ".snapshots"), false);
  else if (sub == "try")
    rc = grub_try(o, c, db);
  else if (sub == "confirm")
    rc = grub_confirm(o, c, db);
  else {
    fprintf(stderr, "usage: salt boot [status|update|try|confirm]\n");
    rc = 2;
  }
  salt_db_close(db);
  return rc;
}
