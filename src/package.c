#include "module.h"

#include <ctype.h>
#include <string.h>

const char *vcpkg_tree(Project *p, const Module *m, bool configured) {
  const Toolchain *tools = m->tools;
  if (!tools->triplet || !tools->vcpkg_root)
    failf("{}: vcpkg requires vcpkgTriplet and vcpkgRoot on {}", m->name,
          tools->id);
  const char *problem = NULL;
  const char *root = expand_env(p, tools->vcpkg_root, tools->id, &problem, true);
  if (!root)
    fail(problem);
  const char *prefix = path(path(p->root, root), tools->triplet);
  if (configured && same(p->config, "debug"))
    prefix = path(prefix, "debug");
  if (!is_dir(prefix))
    failf("{}: missing vcpkg tree for {} at {}", m->name, tools->id, prefix);
  return prefix;
}

static void merge_package_usage(Usage *target, const Usage *source) {
  const Strings *from[] = {&source->includes, &source->lib_dirs, &source->libs,
                           &source->flags, &source->link_flags};
  Strings *to[] = {&target->includes, &target->lib_dirs, &target->libs,
                   &target->flags, &target->link_flags};
  for (size_t i = 0; i < sizeof(from) / sizeof(*from); ++i)
    for (size_t j = 0; j < from[i]->count; ++j)
      unique(to[i], from[i]->items[j]);
  for (size_t i = 0; i < source->defines.count; ++i) {
    const Setting *value = &source->defines.items[i];
    if (!setting(&target->defines, value->name))
      put(&target->defines, value->name, value->value, value->origin);
  }
}

static void import_sdk(Project *p, Module *m) {
  const char *file =
      module_path(p, m, string(member(m->package, "file"), m->name));
  const Json *sdk = load_json(file);
  check_fields(sdk, file,
               " name kind version os arch abi config public runtimeBins ");
  if (!same(json_get_string(sdk, "kind"), "prebuilt") ||
      !same(json_get_string(sdk, "os"), m->tools->os) ||
      !same(json_get_string(sdk, "arch"), m->tools->arch) ||
      !same(json_get_string(sdk, "abi"), package_abi(m->tools)) ||
      (m->tools->msvc && (same(p->config, "debug") !=
                          same(json_get_string(sdk, "config"), "debug"))))
    failf("{}: SDK '{}' is incompatible with {}/{} ({})", m->name, file,
          m->tools->os, m->tools->arch, package_abi(m->tools));
  unique(&p->inputs, file);
  Module context = {.directory = parent(file),
                    .manifest = file,
                    .name = m->name,
                    .tools = m->tools};
  Usage usage = {0};
  Strings deps = {0};
  read_usage(p, &context, &usage, &deps, member(sdk, "public"), file);
  if (deps.count)
    failf("{}: SDK usage must be self-contained", file);
  merge_package_usage(&m->public_usage, &usage);
  Strings bins = {0};
  read_strings(&bins, member(sdk, "runtimeBins"), file);
  for (size_t i = 0; i < bins.count; ++i)
    unique(&m->runtime_bins, module_path(p, &context, bins.items[i]));
}

const char *package_abi(const Toolchain *tools) {
  return !same(tools->os, "windows") ? tools->os
         : tools->msvc               ? "msvc"
                                     : "mingw";
}

static Strings split_pkg_flags(const char *text, const char *where) {
  Strings args = {0};
  while (*text) {
    while (isspace((unsigned char)*text))
      ++text;
    if (!*text)
      break;
    Text arg = {0};
    char quote = 0;
    while (*text && (quote || !isspace((unsigned char)*text))) {
      const char c = *text++;
      if (c == quote) {
        quote = 0;
      } else if (!quote && (c == '"' || c == '\'')) {
        quote = c;
      } else {
        if (c == '\\' && *text && quote != '\'')
          append_n(&arg, text++, 1);
        else
          append_n(&arg, &c, 1);
      }
    }
    if (quote)
      failf("{}: unterminated quote in pkgconf output", where);
    push(&args, native_tool_text(contents(&arg)));
  }
  return args;
}

static void pc_arguments(Module *m, Usage *usage, const Strings *args,
                         const char *base, bool link) {
  for (size_t i = 0; i < args->count; ++i) {
    const char *arg = args->items[i];
    const char *value = arg + (arg[0] ? 1 : 0);
    const char flag = arg[0] == '-' ? *value : '\0';
    if ((link && strchr("Ll", flag) && flag) ||
        (!link && (flag == 'I' || flag == 'D'))) {
      if (!*++value) {
        if (++i == args->count)
          failf("{}: pkgconf returned an incomplete '{}' option", m->name,
                arg);
        value = args->items[i];
      }
      if (flag == 'I')
        unique(&usage->includes, path(base, value));
      else if (flag == 'L')
        unique(&usage->lib_dirs, path(base, value));
      else if (flag == 'l')
        push(&usage->libs, *value == ':' && !m->tools->msvc
                               ? format_string("-l{}", value)
                               : value);
      else {
        const char *equals = strchr(value, '=');
        const char *name =
            equals ? slice(value, (size_t)(equals - value)) : value;
        put(&usage->defines, name,
            own(equals ? json_create_string(equals + 1) : json_create_null()),
            m->name);
      }
    } else if (link &&
               (arg[0] == '/' || arg[0] == '.' || (arg[0] && arg[1] == ':')) &&
               is_file(path(base, arg))) {
      push(&usage->libs, path(base, arg));
    } else {
      if (m->tools->msvc && *arg == '-')
        failf("{}: pkgconf option '{}' requires a GNU-style driver", m->name,
              arg);
      push(link ? &usage->link_flags : &usage->flags, arg);
    }
  }
}

static void watch_dir(Project *p, const char *dir) {
  if (is_dir(dir))
    unique(&p->inputs, dir);
  else if (!same(dir, parent(dir)))
    watch_dir(p, parent(dir));
}

static bool read_pc(Project *p, Module *m, Usage *usage, const char *tree,
                    const char *name, bool static_link) {
  const char *dirs[] = {same(p->config, "debug") ? "debug/lib/pkgconfig"
                                               : "lib/pkgconfig",
                        "share/pkgconfig"};
  const char *file = format_string("{}.pc", name);
  Text search_path = {0};
  const char *where = NULL;
  for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); ++i) {
    const char *dir = path(tree, dirs[i]);
    watch_dir(p, dir);
    if (!is_dir(dir))
      continue;
    Strings entries = {0};
    list_dir(dir, &entries);
    for (size_t j = 0; j < entries.count; ++j) {
      const char *pc = path(dir, entries.items[j]);
      if (ends(pc, ".pc") && is_file(pc))
        unique(&p->inputs, pc);
    }
    if (!where && is_file(path(dir, file)))
      where = path(dir, file);
    if (search_path.length)
#ifdef _WIN32
      append(&search_path, ";");
#else
      append(&search_path, ":");
#endif
    // Keep pcfiledir relative to the triplet. pkgconf can double-escape
    // spaces when a .pc file quotes a path expanded from pcfiledir.
    append(&search_path, dirs[i]);
  }
  if (!where)
    return false;
  const char *program = find_program(p->root, "pkgconf");
  if (!program)
    failf("{}: pkgconf is required to read '{}'", m->name, where);
  unique(&p->inputs, program);
  const char *env[] = {
      format_string("PKG_CONFIG_LIBDIR={}", contents(&search_path)),
      "PKG_CONFIG_PATH=", "PKG_CONFIG_SYSROOT_DIR=",
      "PKG_CONFIG_PRELOADED_FILES=", NULL};
  const char *args[] = {"--no-uninstalled",
                        "--dont-define-prefix",
                        "--keep-system-cflags",
                        "--keep-system-libs",
                        "--cflags",
                        name,
                        static_link ? "--static" : NULL,
                        NULL};
  for (unsigned link = 0; link <= 1; ++link) {
    args[4] = link ? "--libs" : "--cflags";
    const char *out, *err;
    const char *problem =
        capture_program(program, args, tree, env, false, &out, &err);
    if (problem)
      failf("{}: vcpkg pkgconf query for '{}': {}", m->name, where, problem);
    Strings flags = split_pkg_flags(out, where);
    pc_arguments(m, usage, &flags, tree, link != 0);
  }
  return true;
}

static const char *vcpkg_list(Project *p, const Module *m, const char *tree,
                              const char *name) {
  const char *info = path(parent(tree), "vcpkg/info");
  watch_dir(p, info);
  if (!is_dir(info))
    return NULL;
  const char *prefix = format_string("{}_", name);
  const char *suffix = format_string("_{}.list", m->tools->triplet);
  Strings entries = {0};
  list_dir(info, &entries);
  const char *file = NULL;
  for (size_t i = 0; i < entries.count; ++i) {
    const char *entry = entries.items[i];
    if (!starts(entry, prefix) || !ends(entry, suffix))
      continue;
    if (file)
      failf("{}: multiple vcpkg file lists match '{}'", m->name, name);
    file = path(info, entry);
  }
  if (file)
    unique(&p->inputs, file);
  return file;
}

typedef struct {
  const char *name;
  const char *triplet;
  const char *depends;
  const char *status;
} InstalledPackage;

static void read_status(Project *p, const char *tree, const char *triplet,
                        Settings *packages) {
  const char *status = path(parent(tree), "vcpkg/status");
  if (!is_file(status)) {
    watch_dir(p, parent(status));
    return;
  }
  unique(&p->inputs, status);
  size_t length;
  const char *data = read_file(status, &length);
  vstr lines = vstr_from_buf(data, length);
  InstalledPackage record = {0};
  while (lines.len || record.name) {
    vstr line = vstr_trim(vstr_split_next(&lines, vstr_from_cstr("\n")), "\r ");
    if (!line.len) {
      if (record.name && same(record.triplet, triplet) &&
          same(record.status, "install ok installed") && record.depends) {
        const Setting *previous = setting(packages, record.name);
        const char *deps =
            previous ? format_string("{},{}", string(previous->value, status),
                                     record.depends)
                     : record.depends;
        put(packages, record.name, own(json_create_string(deps)), status);
      }
      record = (InstalledPackage){0};
      continue;
    }
    const char *field = slice(line.data, line.len);
    if (starts(field, "Package: "))
      record.name = field + sizeof("Package: ") - 1;
    else if (starts(field, "Architecture: "))
      record.triplet = field + sizeof("Architecture: ") - 1;
    else if (starts(field, "Depends: "))
      record.depends = field + sizeof("Depends: ") - 1;
    else if (starts(field, "Status: "))
      record.status = field + sizeof("Status: ") - 1;
  }
}

static void package_order(const Settings *index, const char *name,
                          const char *triplet, Strings *visited,
                          Strings *order) {
  if (contains(visited, name))
    return;
  push(visited, name);
  const Setting *entry = setting(index, name);
  if (entry) {
    Strings deps = {0};
    vstr rest = vstr_from_cstr(string(entry->value, entry->origin));
    while (rest.len) {
      vstr token = vstr_trim(vstr_split_next(&rest, vstr_from_cstr(",")), " ");
      const char *spec = slice(token.data, token.len);
      const char *colon = strchr(spec, ':');
      const char *dependency = slice(spec, strcspn(spec, ": ["));
      if (identifier(dependency) && (!colon || same(colon + 1, triplet)))
        unique(&deps, dependency);
    }
    for (size_t i = deps.count; i; --i)
      package_order(index, deps.items[i - 1], triplet, visited, order);
  }
  push(order, name);
}

static void list_usage(Module *m, Usage *usage, const char *list,
                       const char *tree, const char *config, bool fallback) {
  size_t length;
  const char *data = read_file(list, &length);
  vstr lines = vstr_from_buf(data, length);
  const char *include = path(tree, "include");
  const char *lib = path(config, "lib");
  const char *bin = path(config, "bin");
  while (lines.len) {
    vstr line = vstr_trim(vstr_split_next(&lines, vstr_from_cstr("\n")), "\r ");
    if (!line.len)
      continue;
    const char *name = slice(line.data, line.len);
    const char *file = path(parent(tree), name);
    if (!inside(tree, file) || !is_file(file))
      continue;
    const char *directory = parent(file);
    if (same(directory, bin) && ends(file, ".dll"))
      unique(&m->runtime_bins, file);
    else if ((same(directory, bin) || same(directory, lib)) &&
             ends(file, ".pdb"))
      unique(&m->debug_symbols, file);
    else if (same(directory, lib)) {
      if (ends(file, ".so") || ends(file, ".dylib") ||
          strstr(filename(file), ".so."))
        unique(&m->runtime_bins, file);
      if (fallback && (ends(file, ".lib") || ends(file, ".a") ||
                       ends(file, ".so") || ends(file, ".dylib")))
        unique(&usage->libs, file);
    } else if (fallback && inside(include, file))
      unique(&usage->includes, include);
  }
}

void resolve_package(Project *p, Module *m) {
  const Json *package = m->package;
  const char *provider = string(member(package, "provider"), m->name);
  if (same(provider, "salts")) {
    check_fields(package, m->name, " provider file ");
    import_sdk(p, m);
    return;
  }
  if (!same(provider, "vcpkg"))
    failf("{}: unknown package provider '{}'", m->name, provider);
  check_fields(package, m->name, " provider name pc static ");
  const char *name = string(member(package, "name"), m->name);
  const char *pc = optional_string(member(package, "pc"), m->name);
  const Json *static_link = member(package, "static");
  if (!identifier(name) || *name == '-' ||
      (pc && (!identifier(pc) || *pc == '-')) ||
      (static_link && json_type(static_link) != JSON_BOOL))
    failf("{}: invalid vcpkg name, pc or static value", m->name);
  const char *tree = vcpkg_tree(p, m, false);
  if (same(p->request->command, "_env"))
    return;
  const char *config = same(p->config, "debug") ? path(tree, "debug") : tree;
  Usage usage = {0};
  const bool found = read_pc(p, m, &usage, tree, pc ? pc : name,
                             static_link && json_bool(static_link));
  const char *list = vcpkg_list(p, m, tree, name);
  if (list)
    list_usage(m, &usage, list, tree, config, !found);
  if (!found && (!list || pc))
    failf("{}: vcpkg package '{}' has no usable pkg-config metadata for {}/{}",
          m->name, pc ? pc : name, m->tools->triplet, p->config);
  if (!found && !usage.libs.count && !usage.includes.count &&
      !m->runtime_bins.count)
    failf("{}: vcpkg package '{}' has no files for {}/{}", m->name, name,
          m->tools->triplet, p->config);
  Settings index = {0};
  Strings visited = {0}, order = {0};
  read_status(p, tree, m->tools->triplet, &index);
  package_order(&index, name, m->tools->triplet, &visited, &order);
  for (size_t i = order.count - 1; i; --i) {
    const char *dependency = vcpkg_list(p, m, tree, order.items[i - 1]);
    if (dependency)
      list_usage(m, &usage, dependency, tree, config, !found);
  }
  merge_package_usage(&m->public_usage, &usage);
}
