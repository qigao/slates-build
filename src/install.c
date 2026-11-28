#include "template.h"

#include <string.h>

static const char *const targets[] = {
    [INSTALL_RUNTIME] = "install/runtime",
    [INSTALL_DEV] = "install/dev",
    [INSTALL_SYMBOLS] = "install/symbols",
};

const char *install_target(InstallComponent component) {
  return targets[component];
}

static const char *component_name(InstallComponent component) {
  return targets[component] + sizeof("install/") - 1;
}

unsigned parse_install_components(const char *names) {
  if (!names)
    return INSTALL_ALL;
  unsigned components = 0;
  for (const char *start = names;;) {
    const char *end = strchr(start, ',');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    unsigned i = 0;
    for (; i < INSTALL_COMPONENT_COUNT; ++i) {
      const char *name = component_name((InstallComponent)i);
      if (strlen(name) == length && !strncmp(name, start, length))
        break;
    }
    if (i == INSTALL_COMPONENT_COUNT)
      failf("unknown install component '{}'; expected runtime, dev or symbols",
            slice(start, length));
    components |= 1u << i;
    if (!end)
      return components;
    start = end + 1;
  }
}

const char *install_prefix(const Project *p, const char *override) {
  if (!override)
    return path(p->build_dir, "stage");
  return path(path(p->root, override), ".");
}

static void check_destination(const char *to, const char *where) {
  if (!*to || to[0] == '/' || strpbrk(to, "\\:*?[]<>|\"\r\n"))
    failf("{}: install destination must be relative to the prefix: '{}'", where,
          to);
  for (const char *start = to; *start;) {
    const char *end = strchr(start, '/');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (length == 2 && !strncmp(start, "..", 2))
      failf("{}: install destination escapes the prefix: '{}'", where, to);
    if (length &&
        (start[length - 1] == ' ' || (start[length - 1] == '.' && length != 1)))
      failf("{}: install destination has a trailing dot or space: '{}'", where,
            to);
    if (!end)
      break;
    start = end + 1;
  }
}

void add_install_mapping(Module *m, InstallComponent component,
                         const char *from, const char *to, const char *where) {
  check_destination(to, where);
  if (strpbrk(from, "?[]\r\n") || strchr(parent(from), '*'))
    failf("{}: install source only supports '*' in the last segment: '{}'",
          where, from);
  m->install = reserve(m->install, m->install_count + 1, &m->install_capacity,
                       sizeof(*m->install));
  m->install[m->install_count++] = (InstallMapping){from, to, component};
}

static bool same_path(const char *left, const char *right) {
  return left && right && strlen(left) == strlen(right) && inside(left, right);
}

static bool source_matches(const char *from, const char *output) {
  if (!output)
    return false;
  if (!strchr(from, '*'))
    return inside(from, output);
  return same_path(parent(from), parent(output)) &&
         filename_matches(filename(from), filename(output));
}

static void plan_entry(Json *entries, const char *from, const char *to) {
  Json *entry = json_create_object();
  json_add(entries, entry);
  json_set(entry, "from", json_create_string(from));
  json_set(entry, "to", json_create_string(to));
}

const char *artifact_install_dir(const Graph *g, const char *artifact) {
  const char *directory = parent(artifact) + strlen(g->project->build_dir);
  return *directory == '/' ? directory + 1 : directory;
}

static void plan_artifact(const Graph *g, Json *entries, Strings *inputs,
                          const char *artifact, const char *destination) {
  if (!artifact)
    return;
  if (!destination)
    destination = artifact_install_dir(g, artifact);
  plan_entry(entries, artifact, destination);
  unique(inputs, artifact);
}

static void plan_mapping(const Graph *g, const Module *m, Json *entries,
                         Strings *inputs, const char *from, const char *to) {
  plan_entry(entries, from, to);
  if (source_matches(from, m->artifact))
    unique(inputs, m->artifact);
  if (source_matches(from, m->import_lib))
    unique(inputs, m->import_lib);
  for (size_t i = 0; i < g->task_count; ++i) {
    const Task *task = g->tasks[i];
    if (!task->selected)
      continue;
    for (size_t j = 0; j < task->outputs.count; ++j) {
      const char *output = task->outputs.items[j];
      if (source_matches(from, output))
        unique(inputs, output);
    }
  }
}

static void plan_module(const Graph *g, const Module *m,
                        InstallComponent component, Json *entries,
                        Strings *inputs, Strings *visited, Strings *metadata,
                        const BuildTemplate *sdk_json,
                        const BuildTemplate *sdk_pc) {
  if (m->tools != &g->project->target || m->test_only)
    return;
  if (contains(visited, m->name))
    return;
  push(visited, m->name);
  if (component == INSTALL_RUNTIME) {
    if (m->kind == MODULE_EXECUTABLE || m->kind == MODULE_SHARED)
      plan_artifact(g, entries, inputs, m->artifact, NULL);
    const char *directory = same(m->tools->os, "windows") ? "bin" : "lib";
    for (size_t i = 0; i < m->runtime_bins.count; ++i)
      plan_mapping(g, m, entries, inputs, m->runtime_bins.items[i], directory);
  } else if (component == INSTALL_DEV) {
    if (m->kind == MODULE_STATIC ||
        (m->kind == MODULE_SHARED && !same(m->tools->os, "windows") &&
         !same(json_get_string(m->data, "kind"), "cmeta_plugin")))
      plan_artifact(g, entries, inputs, m->artifact, NULL);
    plan_artifact(g, entries, inputs, m->import_lib, NULL);
    if (m->export_target) {
      const char *dir = path(g->project->build_dir,
                             format_string("sdk/{}", m->name));
      const char *json = path(dir, "pkg.json");
      const char *pc = path(dir, format_string("{}.pc", m->name));
      if (contains(metadata, json) || producer(g, json) || producer(g, pc))
        failf("{}: duplicate SDK export '{}'", m->name, m->name);
      push(metadata, json);
      push(metadata, pc);
      emit_sdk(g, m, sdk_json, sdk_pc, json, pc);
      plan_artifact(g, entries, inputs, json,
                    format_string("share/" SLATE_NAME "/{}", m->name));
      plan_artifact(g, entries, inputs, pc, "lib/pkgconfig");
      if (m->cmake_target) {
        const char *scope = strstr(m->cmake_target, "::");
        const char *package = slice(m->cmake_target, (size_t)(scope - m->cmake_target));
        const char *cmake = path(dir, format_string("{}-target.cmake", m->name));
        push(metadata, cmake);
        plan_artifact(g, entries, inputs, cmake, format_string("lib/cmake/{}", package));
      }
    }
  } else {
    plan_artifact(g, entries, inputs, module_pdb(m), "symbols");
    for (size_t i = 0; i < m->debug_symbols.count; ++i)
      plan_mapping(g, m, entries, inputs, m->debug_symbols.items[i], "symbols");
    const char *format = symbol_format(m);
    if (format) {
      const char *output = symbol_output(g, m);
      if (same(format, "macho")) {
        plan_entry(entries, output, format_string("symbols/{}", filename(output)));
        unique(inputs, dsym_dwarf(output, m->artifact));
      } else {
        plan_artifact(g, entries, inputs, output, "symbols");
      }
    }
  }
  for (size_t i = 0; i < m->install_count; ++i) {
    const InstallMapping *mapping = &m->install[i];
    if (mapping->component == component)
      plan_mapping(g, m, entries, inputs, mapping->from, mapping->to);
  }
  const Strings *edges[] = {&m->public_deps, &m->private_deps,
                            &m->runtime_deps};
  size_t count = component != INSTALL_DEV                     ? 3
                 : (m->kind == MODULE_STATIC || m->kind == MODULE_PREBUILT ||
                    m->kind == MODULE_HEADER_ONLY)            ? 2
                                                              : 1;
  for (size_t j = 0; j < count; ++j)
    for (size_t i = 0; i < edges[j]->count; ++i)
      plan_module(g, find_module(g, edges[j]->items[i]), component, entries,
                  inputs, visited, metadata, sdk_json, sdk_pc);
}

void write_install_plan(const Graph *g, Strings *inputs, Strings *metadata,
                        const BuildTemplate *sdk_json,
                        const BuildTemplate *sdk_pc) {
  const Project *p = g->project;
  Json *plan = own(json_create_object());
  for (unsigned i = 0; i < INSTALL_COMPONENT_COUNT; ++i) {
    const InstallComponent component = (InstallComponent)i;
    Json *entries = json_create_array();
    json_set(plan, component_name(component), entries);
    Strings visited = {0};
    for (size_t j = 0; j < g->count; ++j) {
      const Module *m = g->modules[j];
      if (m->selected && (!p->request->targets.count ||
                          contains(&p->request->targets, m->name)))
        plan_module(g, m, component, entries, &inputs[i], &visited, metadata,
                    sdk_json, sdk_pc);
    }
  }
  write_json(path(p->build_dir, INSTALL_PLAN), plan);
}

typedef struct {
  const char *source;
  const char *destination;
} InstallFile;

typedef struct {
  InstallFile *files;
  size_t count;
  size_t capacity;
} InstallFiles;

static void add_file(InstallFiles *files, const char *source,
                     const char *directory, const char *name) {
  const char *destination = path(directory, name);
  for (size_t i = 0; i < files->count; ++i) {
    const InstallFile *existing = &files->files[i];
    if (same_path(existing->destination, destination) &&
        same_path(existing->source, source))
      return;
    if (inside(existing->destination, destination) ||
        inside(destination, existing->destination))
      failf("install destination collision at '{}' between '{}' and '{}'",
            destination, existing->source, source);
  }
  if (is_dir(destination))
    failf("install destination is a directory: '{}'", destination);
  files->files = reserve(files->files, files->count + 1, &files->capacity,
                         sizeof(*files->files));
  files->files[files->count++] = (InstallFile){source, destination};
}

static void add_tree(InstallFiles *files, const char *source,
                     const char *destination, unsigned depth) {
  if (depth > 128)
    failf("install source tree is too deep at '{}'", source);
  Strings entries = {0};
  list_dir(source, &entries);
  for (size_t i = 0; i < entries.count; ++i) {
    const char *name = entries.items[i];
    const char *child = path(source, name);
    if (is_dir(child))
      add_tree(files, child, path(destination, name), depth + 1);
    else if (is_file(child))
      add_file(files, child, destination, name);
  }
}

static void expand_mapping(InstallFiles *files, const char *from,
                           const char *destination) {
  if (strchr(from, '*')) {
    const char *directory = parent(from), *pattern = filename(from);
    Strings entries = {0};
    if (is_dir(directory))
      list_dir(directory, &entries);
    bool matched = false;
    for (size_t i = 0; i < entries.count; ++i) {
      const char *name = entries.items[i];
      const char *source = path(directory, name);
      if (filename_matches(pattern, name) && is_file(source)) {
        add_file(files, source, destination, name);
        matched = true;
      }
    }
    if (!matched)
      failf("install source pattern matches no files: '{}'", from);
  } else if (is_dir(from)) {
    if (inside(from, destination))
      failf("install destination '{}' is inside source directory '{}'",
            destination, from);
    add_tree(files, from, destination, 0);
  } else if (is_file(from)) {
    add_file(files, from, destination, filename(from));
  } else {
    failf("install source does not exist: '{}'", from);
  }
}

static void check_install_parents(const char *prefix, const char *file) {
  for (const char *dir = parent(file); !same_path(dir, prefix);
       dir = parent(dir)) {
    if (!inside(prefix, dir) || is_link(dir) || is_file(dir))
      failf("invalid install destination directory '{}'", dir);
  }
}

void run_install(const char *file, const char *prefix, unsigned components) {
  const Json *plan = load_json(file);
  InstallFiles files = {0};
  for (unsigned i = 0; i < INSTALL_COMPONENT_COUNT; ++i) {
    if (!(components & (1u << i)))
      continue;
    const Json *entries = member(plan, component_name((InstallComponent)i));
    array(entries, file);
    for (size_t j = 0; j < json_array_size(entries); ++j) {
      const Json *entry = json_array_get(entries, j);
      const char *from = string(member(entry, "from"), file);
      const char *to = string(member(entry, "to"), file);
      check_destination(to, file);
      const char *destination = path(prefix, to);
      if (!inside(prefix, destination))
        failf("install destination escapes the prefix: '{}'", destination);
      expand_mapping(&files, from, destination);
    }
  }
  for (size_t i = 0; i < files.count; ++i)
    check_install_parents(prefix, files.files[i].destination);
  for (size_t i = 0; i < files.count; ++i) {
    const InstallFile *entry = &files.files[i];
    if (!same_path(entry->source, entry->destination))
      copy_install_file(entry->source, entry->destination);
  }
  print_line("Installed {} files to {}", files.count, prefix);
}
