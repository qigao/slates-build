#include "module.h"

#include <limits.h>
#include <string.h>

const char *module_text(Project *p, const Module *m, const char *text) {
  const char *problem = NULL;
  const char *expanded = expand_env(p, text, m->manifest, &problem, true);
  if (!expanded)
    fail(problem);
  Text value = {0};
  for (const char *c = expanded; *c;) {
    if (starts(c, "$root") && (!c[5] || c[5] == '/')) {
      append(&value, p->root);
      c += 5;
    } else if (starts(c, "$moduledir") && (!c[10] || c[10] == '/')) {
      append(&value, m->directory);
      c += 10;
    } else if (starts(c, "$builddir") && (!c[9] || c[9] == '/')) {
      append(&value, p->build_dir);
      c += 9;
    } else if (starts(c, "$target") && (!c[7] || c[7] == '/')) {
      if (!m->artifact)
        failf("{}: $target requires a module artifact", m->name);
      append(&value, m->artifact);
      c += 7;
    } else if ((starts(c, "$vcpkg") && (!c[6] || c[6] == '/')) ||
               (starts(c, "$vcpkgConfig") && (!c[12] || c[12] == '/'))) {
      const bool configured = c[6] == 'C';
      append(&value, vcpkg_tree(p, m, configured));
      c += configured ? 12 : 6;
    } else {
      append_n(&value, c++, 1);
    }
  }
  return contents(&value);
}

const char *module_path(Project *p, const Module *m, const char *name) {
  return path(m->directory, module_text(p, m, name));
}

void read_module_paths(Project *p, const Module *m, Strings *list,
                       const Json *value) {
  Strings names = {0};
  read_strings(&names, value, m->manifest);
  for (size_t i = 0; i < names.count; ++i)
    unique(list, module_path(p, m, names.items[i]));
}

void read_usage(Project *p, const Module *m, Usage *u, Strings *deps,
                const Json *value, const char *origin) {
  if (!value)
    return;
  check_fields(value, origin,
               " includeDirs requiredIncludeDirs libDirs defines libs flags "
               "linkFlags deps ");
  read_module_paths(p, m, &u->includes, member(value, "includeDirs"));
  Strings required = {0};
  read_module_paths(p, m, &required, member(value, "requiredIncludeDirs"));
  for (size_t i = 0; i < required.count; ++i) {
    const char *directory = required.items[i];
    if (!is_dir(directory))
      failf("{}.requiredIncludeDirs: directory not found '{}'", origin,
            directory);
    unique(&u->includes, directory);
  }
  read_module_paths(p, m, &u->lib_dirs, member(value, "libDirs"));
  Strings libs = {0};
  read_strings(&libs, member(value, "libs"), origin);
  for (size_t i = 0; i < libs.count; ++i) {
    const char *lib = libs.items[i];
    const char *item = module_text(p, m, lib);
    if (strchr(item, '/') || strchr(item, '\\'))
      item = path(m->directory, item);
    unique(&u->libs, item);
  }
  const Json *defines = member(value, "defines");
  if (defines) {
    object(defines, origin);
    for (size_t i = 0; i < json_object_size(defines); ++i) {
      const Json *definition = json_object_value(defines, i);
      if (json_type(definition) != JSON_NULL) {
        const char *raw = string(definition, origin);
        const char *expanded = module_text(p, m, raw);
        if (!same(raw, expanded))
          definition = own(json_create_string(expanded));
      }
      put(&u->defines, json_object_key(defines, i), definition, origin);
    }
  }
  read_strings(&u->flags, member(value, "flags"), origin);
  read_strings(&u->link_flags, member(value, "linkFlags"), origin);
  read_strings(deps, member(value, "deps"), origin);
}

static void add_tasks(Graph *g, Module *m, const Json *value) {
  if (!value)
    return;
  array(value, m->manifest);
  for (size_t i = 0; i < json_array_size(value); ++i) {
    const Json *data = json_array_get(value, i);
    check_fields(
        data, m->manifest,
        " name builtin template data inputs outputs restat tool program args ");
    Task *task = take(sizeof(*task));
    task->owner = m;
    task->name = string(member(data, "name"), m->manifest);
    if (!identifier(task->name))
      failf("{}: invalid task name '{}'", m->name, task->name);
    for (size_t j = 0; j < g->task_count; ++j)
      if (g->tasks[j]->owner == m && same(g->tasks[j]->name, task->name))
        failf("{}: duplicate task '{}'", m->name, task->name);
    task->engine = optional_string(member(data, "builtin"), task->name);
    task->tool = optional_string(member(data, "tool"), task->name);
    task->program = optional_string(member(data, "program"), task->name);
    const char *template =
        optional_string(member(data, "template"), task->name);
    const char *context = optional_string(member(data, "data"), task->name);
    if (template)
      task->template = module_path(g->project, m, template);
    if (context)
      task->data = module_path(g->project, m, context);
    if ((task->engine != NULL) + (task->tool != NULL) +
            (task->program != NULL) !=
        1)
      failf("{}/{}: requires exactly one of builtin, tool or program", m->name,
            task->name);
    const bool mustache = same(task->engine, "mustache");
    const bool copy_task = same(task->engine, "copy");
    if (task->engine && !mustache && !copy_task)
      failf("{}/{}: unsupported builtin '{}'", m->name, task->name,
            task->engine);
    if (mustache != (task->template != NULL) || (!mustache && task->data))
      failf("{}/{}: Mustache requires template and accepts optional data",
            m->name, task->name);
    task->input_paths = member(data, "inputs");
    read_module_paths(g->project, m, &task->outputs, member(data, "outputs"));
    if (!task->outputs.count || (task->engine && task->outputs.count != 1))
      failf("{}/{}: expected declared outputs (one for a builtin)", m->name,
            task->name);
    for (size_t j = 0; j < task->outputs.count; ++j)
      if (!inside(g->project->build_dir, task->outputs.items[j]) ||
          same(g->project->build_dir, task->outputs.items[j]))
        failf("{}/{}: outputs must be inside buildDir", m->name, task->name);
    read_strings(&task->args, member(data, "args"), task->name);
    g->tasks = reserve(g->tasks, g->task_count + 1, &g->task_capacity,
                       sizeof(*g->tasks));
    g->tasks[g->task_count++] = task;
  }
}

static bool matches(const Module *m, const Project *p, const Json *when) {
  check_fields(when, m->manifest, " os arch driver config native ");
  for (size_t i = 0; i < json_object_size(when); ++i) {
    const char *field = json_object_key(when, i);
    const Json *expected = json_object_value(when, i);
    if (same(field, "native")) {
      if (json_type(expected) != JSON_BOOL)
        failf("{}: when.native must be a boolean", m->manifest);
      if (json_bool(expected) != (m->tools == &p->host || p->native))
        return false;
      continue;
    }
    const char *actual = same(field, "os")       ? m->tools->os
                         : same(field, "arch")   ? m->tools->arch
                         : same(field, "driver") ? m->tools->driver
                                                 : p->config;
    if (json_type(expected) == JSON_ARRAY) {
      Strings candidates = {0};
      read_strings(&candidates, expected, m->manifest);
      if (!contains(&candidates, actual))
        return false;
    } else if (!same(actual, string(expected, m->manifest))) {
      return false;
    }
  }
  return true;
}

static void apply_module_fields(Graph *g, Module *m, const Json *value,
                                Strings *exclusions, const char *origin) {
  Project *p = g->project;
  const Json *sources = member(value, "sources");
  if (sources) {
    check_fields(sources, origin, " files exclude objc ");
    read_module_paths(p, m, &m->sources, member(sources, "files"));
    read_module_paths(p, m, exclusions, member(sources, "exclude"));
    read_module_paths(p, m, &m->objc_sources, member(sources, "objc"));
  }
  read_usage(p, m, &m->public_usage, &m->public_deps, member(value, "public"),
             origin);
  read_usage(p, m, &m->private_usage, &m->private_deps, member(value, "private"),
             origin);
  const Json *package = member(value, "package");
  if (package)
    m->package = package;
  overlay(&m->options, member(value, "options"), origin);
  const Json *rules = member(value, "ninjaRules");
  if (rules) {
    const char *where = format_string("{}.ninjaRules", origin);
    check_fields(rules, where, " compile archive link task ");
    for (size_t i = 0; i < json_object_size(rules); ++i) {
      const char *key = json_object_key(rules, i);
      const Json *rule = json_object_value(rules, i);
      const char *name = string(rule, where);
      if (!identifier(name))
        failf("{}: invalid Ninja rule name '{}'", where, name);
      put(&m->ninja_rules, key, rule, where);
    }
  }
  read_strings(&m->runtime_deps, member(value, "runtimeDeps"), origin);
  read_module_paths(p, m, &m->runtime_bins, member(value, "runtimeBins"));
  const Json *install = member(value, "install");
  if (install) {
    const char *where = format_string("{}.install", origin);
    check_fields(install, where, " headers assets exportTarget version ");
    const Json *export_target = member(install, "exportTarget");
    if (export_target) {
      if (json_type(export_target) != JSON_BOOL)
        failf("{}: exportTarget must be a boolean", where);
      m->export_target = json_bool(export_target);
    }
    const char *version = optional_string(member(install, "version"), where);
    if (version) {
      if (!identifier(version))
        failf("{}: invalid SDK version '{}'", where, version);
      m->export_version = version;
    }
    const char *fields[] = {"assets", "headers"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
      const Json *mappings = member(install, fields[i]);
      if (!mappings)
        continue;
      array(mappings, where);
      for (size_t j = 0; j < json_array_size(mappings); ++j) {
        const Json *mapping = json_array_get(mappings, j);
        check_fields(mapping, where, " from to ");
        const char *from = string(member(mapping, "from"), where);
        const char *to = string(member(mapping, "to"), where);
        if (!*from)
          failf("{}: install source cannot be empty", where);
        add_install_mapping(m, i ? INSTALL_DEV : INSTALL_RUNTIME,
                            module_path(p, m, from), to, where);
      }
    }
  }
  add_tasks(g, m, member(value, "preTasks"));
}

static ModuleKind kind(const char *name, const char *label) {
  if (same(name, "executable") || same(name, "exec"))
    return MODULE_EXECUTABLE;
  if (same(name, "static") || same(name, "static_lib"))
    return MODULE_STATIC;
  if (same(name, "shared") || same(name, "shared_lib") ||
      same(name, "cmeta_plugin"))
    return MODULE_SHARED;
  if (same(name, "header_only"))
    return MODULE_HEADER_ONLY;
  if (same(name, "prebuilt") || same(name, "prebuilt_lib") ||
      same(name, "external"))
    return MODULE_PREBUILT;
  failf("{}: module kind '{}' is not implemented", label, name);
}

static void init_artifact(Project *p, Module *m, const char *name,
                          const char *prefix, bool plugin) {
  bool windows = same(m->tools->os, "windows");
  const ArtifactNames *names = &m->tools->artifacts;
  const char *base =
      m->tools == &p->host ? path(p->build_dir, "host") : p->build_dir;
  if (m->kind == MODULE_EXECUTABLE)
    m->artifact =
        path(base, format_string("bin/{}{}", name, names->executable_suffix));
  else if (m->kind == MODULE_STATIC)
    m->artifact =
        path(base, format_string("lib/{}{}{}", names->static_prefix, name,
                                 names->static_suffix));
  else if (m->kind == MODULE_SHARED) {
    const char *directory = plugin ? "plugins" : windows ? "bin" : "lib";
    if (!prefix)
      prefix = names->shared_prefix;
    m->artifact = path(base, format_string("{}/{}{}{}", directory, prefix, name,
                                           names->shared_suffix));
    if (windows && !plugin)
      m->import_lib =
          path(base, format_string("lib/{}{}{}", names->import_prefix, name,
                                   names->import_suffix));
  }
}

static void add_module(Graph *g, Settings *declared, const char *file,
                       const char *default_name, const Json *data,
                       const char *matched_source, bool direct) {
  check_fields(
      data, file,
      " $schema include modules name kind visibility toolchainRole extends sources "
      "private public options variants runtimeDeps runtimeBins preTasks targets test "
      "outputPrefix forEach when testOnly ninjaRules install package ");
  if (!direct && (member(data, "name") || member(data, "include") ||
                  member(data, "modules")))
    failf("{}: name, include and modules belong at the top level of the manifest",
          file);
  const char *name = optional_string(member(data, "name"), file);
  if (!name)
    name = default_name;
  if (!identifier(name))
    failf("{}: invalid target name '{}'", file, name);
  const Setting *other = setting(declared, name);
  if (other)
    failf("duplicate target name '{}' in '{}' and '{}'", name, other->origin,
          file);
  put(declared, name, data, file);
  Project *p = g->project;
  const Json *test_only = member(data, "testOnly");
  if (test_only && json_type(test_only) != JSON_BOOL)
    failf("{}: testOnly must be a boolean", name);
  if (!p->tests &&
      (test_only ? json_bool(test_only) : member(data, "test") != NULL))
    return;
  Module *m = take(sizeof(*m));
  m->manifest = file;
  m->directory = parent(file);
  m->name = name;
  m->data = data;
  m->test_only = test_only ? json_bool(test_only) : member(data, "test") != NULL;
  const char *type = string(member(data, "kind"), name);
  m->kind = kind(type, name);
  const char *prefix = optional_string(member(data, "outputPrefix"), name);
  if (prefix && (m->kind != MODULE_SHARED || (*prefix && !identifier(prefix))))
    failf("{}: outputPrefix requires a shared library and a valid filename "
          "prefix",
          name);
  const char *role = optional_string(member(data, "toolchainRole"), name);
  if (role && !same(role, "host") && !same(role, "target"))
    failf("{}: invalid toolchainRole '{}'", name, role);
  m->tools = same(role, "host") ? &p->host : &p->target;
  const Json *when = member(data, "when");
  if (when && !matches(m, p, when))
    return;
  init_artifact(p, m, name, prefix, same(type, "cmeta_plugin"));
  const Json *test = p->tests ? member(data, "test") : NULL;
  if (test) {
    check_fields(test, name,
                 " name namePrefix program args timeout workingDirectory "
                 "environment labels ");
    m->test_name = optional_string(member(test, "name"), name);
    const char *test_prefix =
        optional_string(member(test, "namePrefix"), name);
    if (m->test_name && test_prefix)
      failf("{}: test.name and test.namePrefix are mutually exclusive", name);
    if (!m->test_name)
      m->test_name =
          test_prefix ? format_string("{}{}", test_prefix, name) : name;
    if (m->kind != MODULE_EXECUTABLE || !*m->test_name ||
        strpbrk(m->test_name, "\r\n"))
      failf("{}: test requires an executable and a valid test.name", name);
    m->test_program = optional_string(member(test, "program"), name);
    read_strings(&m->test_args, member(test, "args"), name);
    const char *directory =
        optional_string(member(test, "workingDirectory"), name);
    if (directory)
      m->test_directory = module_path(p, m, directory);
    read_strings(&m->test_environment, member(test, "environment"), name);
    read_strings(&m->test_labels, member(test, "labels"), name);
    const Json *timeout = member(test, "timeout");
    if (timeout) {
      double seconds =
          json_type(timeout) == JSON_NUMBER ? json_number(timeout) : 0;
      if (!(seconds >= 1 && seconds <= UINT_MAX) ||
          seconds != (unsigned)seconds)
        failf("{}: test.timeout must be a positive whole number of seconds",
              name);
      m->test_timeout = (unsigned)seconds;
    }
  }
  const char *standards[] = {"cStd", "cxxStd"};
  for (size_t i = 0; i < sizeof(standards) / sizeof(*standards); ++i) {
    const Setting *entry = setting(&p->fields, standards[i]);
    if (entry)
      put(&m->options, entry->name, entry->value, entry->origin);
  }
  for (size_t i = 0; i < p->options.count; ++i) {
    Setting entry = p->options.items[i];
    put(&m->options, entry.name, entry.value, entry.origin);
  }
  for (size_t i = 0; i < p->definitions.count; ++i) {
    Setting entry = p->definitions.items[i];
    put(&m->compile_usage.defines, entry.name, entry.value, entry.origin);
  }
  read_strings(&m->compile_usage.flags, get(&p->fields, "flags"), p->name);
  Strings styles = {0}, stack = {0}, exclusions = {0};
  read_strings(&styles, member(data, "extends"), name);
  for (size_t i = 0; i < p->preset_styles.count; ++i)
    apply_style(p, p->preset_styles.items[i], &m->options, &m->compile_usage,
                &stack);
  for (size_t i = 0; i < styles.count; ++i)
    apply_style(p, styles.items[i], &m->options, &m->compile_usage, &stack);
  apply_module_fields(g, m, data, &exclusions, name);
  const Json *variants = member(data, "variants");
  if (variants) {
    array(variants, name);
    for (size_t i = 0; i < json_array_size(variants); ++i) {
      const Json *variant = json_array_get(variants, i);
      const char *origin = format_string("{}.variants[{}]", name, i);
      check_fields(variant, origin,
                   " when sources public private options runtimeDeps "
                   "runtimeBins preTasks ninjaRules install package ");
      if (matches(m, p, member(variant, "when")))
        apply_module_fields(g, m, variant, &exclusions, origin);
    }
  }
  m->declared_public_includes = m->public_usage.includes;
  if ((m->package && m->kind != MODULE_PREBUILT) ||
      (!m->package && same(type, "external")))
    failf("{}: external requires a package; package requires external or prebuilt",
          m->name);
  if (m->export_target) {
    if (m->kind == MODULE_EXECUTABLE || same(type, "cmeta_plugin"))
      failf("{}: only linkable libraries can export an SDK target", m->name);
    if (same(name, ".") || same(name, ".."))
      failf("{}: invalid SDK export name '{}'", m->name, name);
  }
  size_t used = 0;
  for (size_t i = 0; i < m->sources.count; ++i)
    if (!contains(&exclusions, m->sources.items[i]))
      m->sources.items[used++] = m->sources.items[i];
  m->sources.count = used;
  if (matched_source)
    unique(&m->sources, matched_source);
  g->modules =
      reserve(g->modules, g->count + 1, &g->capacity, sizeof(*g->modules));
  g->modules[g->count++] = m;
}

static bool matches_files(const Strings *patterns, const char *name) {
  for (size_t i = 0; i < patterns->count; ++i)
    if (filename_matches(patterns->items[i], name))
      return true;
  return false;
}

static void read_file_patterns(Strings *patterns, const Json *value,
                               const char *file) {
  read_strings(patterns, value, file);
  for (size_t i = 0; i < patterns->count; ++i) {
    const char *pattern = patterns->items[i];
    if (!*pattern || pattern[0] == '.' || strpbrk(pattern, "/\\:?[]"))
      failf("{}: forEach requires a filename or a local '*' pattern: '{}'",
            file, pattern);
  }
}

static void add_matching_targets(Graph *g, Settings *declared, const char *file,
                                 const Json *groups) {
  array(groups, file);
  const char *dir = parent(file);
  unique(&g->project->inputs, dir);
  Strings entries = {0}, files = {0};
  list_dir(dir, &entries);
  for (size_t i = 0; i < entries.count; ++i) {
    const char *name = entries.items[i];
    if (name[0] != '.' && is_file(path(dir, name)))
      push(&files, name);
  }
  for (size_t i = 0; i < json_array_size(groups); ++i) {
    const Json *group = json_array_get(groups, i);
    check_fields(group, file, " files exclude namePrefix target ");
    Strings patterns = {0}, excluded = {0};
    read_file_patterns(&patterns, member(group, "files"), file);
    read_file_patterns(&excluded, member(group, "exclude"), file);
    if (!patterns.count)
      failf("{}: forEach.files requires at least one filename or pattern", file);
    for (size_t j = 0; j < patterns.count; ++j) {
      const char *pattern = patterns.items[j];
      if (!strchr(pattern, '*') && !contains(&files, pattern))
        failf("{}: forEach source does not exist: {}", file, pattern);
    }
    const char *prefix = optional_string(member(group, "namePrefix"), file);
    const Json *target = member(group, "target");
    object(target, file);
    if (member(target, "name") || member(target, "targets") ||
        member(target, "forEach") || member(target, "include") ||
        member(target, "modules"))
      failf("{}: forEach.target declares a single target with a derived name",
            file);
    for (size_t j = 0; j < files.count; ++j) {
      const char *name = files.items[j];
      if (!matches_files(&patterns, name) || matches_files(&excluded, name))
        continue;
      const char *extension = strrchr(name, '.');
      const char *stem =
          extension ? slice(name, (size_t)(extension - name)) : name;
      const char *target_name =
          prefix ? format_string("{}{}", prefix, stem) : stem;
      if (!identifier(target_name))
        failf("{}: invalid forEach target name '{}' from '{}'", file,
              target_name, name);
      add_module(g, declared, file, target_name, target, path(dir, name),
                 false);
    }
  }
}

static const char *module_file(const char *base, const char *directory,
                               const char *from) {
  if (!*directory)
    failf("{}: modules contains an empty directory path; use '.' for the current "
          "directory", from);
  return path(path(base, directory), "module.json");
}

static void collect_manifest(Project *p, Settings *loaded, Strings *stack,
                             const char *file, const char *from) {
  if (contains(stack, file))
    failf("{}: module include cycle at '{}'", from, file);
  if (setting(loaded, file))
    return;
  if (!inside(p->root, file) || !is_file(file))
    failf("{}: '{}' must be a module manifest file in the repository", from,
          file);
  if (stack->count >= 128)
    failf("{}: module includes are too deep", file);
  const Json *data = load_json(file);
  put(loaded, file, data, from);
  unique(&p->inputs, file);
  Strings includes = {0}, directories = {0};
  read_strings(&includes, member(data, "include"), file);
  read_strings(&directories, member(data, "modules"), file);
  const char *dir = parent(file);
  push(stack, file);
  for (size_t i = 0; i < includes.count; ++i)
    collect_manifest(p, loaded, stack, path(dir, includes.items[i]), file);
  for (size_t i = 0; i < directories.count; ++i)
    collect_manifest(p, loaded, stack,
                     module_file(dir, directories.items[i], file), file);
  --stack->count;
}

void load_modules(Graph *g) {
  Project *p = g->project;
  Settings loaded = {0};
  Strings directories = {0}, entries = {0}, stack = {0};
  read_strings(&directories, member(p->manifest, "modules"), "modules");
  for (size_t i = 0; i < directories.count; ++i)
    collect_manifest(p, &loaded, &stack,
                     module_file(p->root, directories.items[i], "slate.json"),
                     "modules");
  read_strings(&entries, member(p->manifest, "moduleFiles"), "moduleFiles");
  for (size_t i = 0; i < entries.count; ++i)
    collect_manifest(p, &loaded, &stack, path(p->root, entries.items[i]),
                     "moduleFiles");
  Strings manifests = {0};
  for (size_t i = 0; i < loaded.count; ++i)
    push(&manifests, loaded.items[i].name);
  sort_strings(&manifests);
  Settings declared = {0};
  for (size_t i = 0; i < manifests.count; ++i) {
    const char *file = manifests.items[i];
    const Json *data = setting(&loaded, file)->value;
    const Json *targets = member(data, "targets");
    const Json *for_each = member(data, "forEach");
    if (member(data, "kind"))
      add_module(g, &declared, file, filename(parent(file)), data, NULL, true);
    else
      check_fields(data, file, " $schema include modules targets forEach ");
    if (targets) {
      object(targets, file);
      for (size_t j = 0; j < json_object_size(targets); ++j) {
        const char *name = json_object_key(targets, j);
        if (!identifier(name))
          failf("{}: invalid subtarget name '{}'", file, name);
        add_module(g, &declared, file, name, json_object_value(targets, j),
                   NULL, false);
      }
    }
    if (for_each)
      add_matching_targets(g, &declared, file, for_each);
  }
}
