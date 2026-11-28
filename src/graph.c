#include "module.h"
#include "template.h"

#include <string.h>

Module *find_module(const Graph *g, const char *name) {
  for (size_t i = 0; i < g->count; ++i)
    if (same(g->modules[i]->name, name))
      return g->modules[i];
  return NULL;
}

bool module_runnable(const Project *p, const Module *m) {
  return m->kind == MODULE_EXECUTABLE && (p->native || m->tools == &p->host);
}

Task *producer(const Graph *g, const char *output) {
  Task *match = NULL;
  for (size_t i = 0; i < g->task_count; ++i) {
    Task *task = g->tasks[i];
    if (!contains(&task->outputs, output))
      continue;
    if (match)
      failf("{} has two producers: {}/{} and {}/{}", output, match->owner->name,
            match->name, task->owner->name, task->name);
    match = task;
  }
  return match;
}

static bool visible(const Module *dependency, const Module *consumer) {
  const Json *rules = member(dependency->data, "visibility");
  if (!rules || dependency == consumer)
    return true;
  array(rules, dependency->manifest);
  for (size_t i = 0; i < json_array_size(rules); ++i) {
    const char *rule = string(json_array_get(rules, i), dependency->manifest);
    if (same(rule, "*") || same(rule, consumer->name))
      return true;
  }
  return false;
}

static Module *dependency(const Graph *g, const Module *m, const char *name) {
  Module *dep = find_module(g, name);
  if (!dep)
    failf("{} references missing target {}", m->name, name);
  if (!visible(dep, m))
    failf("{} is not visible to {}", name, m->name);
  return dep;
}

static _Noreturn void cycle(Strings *stack, const char *node) {
  Text trace = {0};
  size_t i = 0;
  while (i < stack->count && !same(stack->items[i], node))
    ++i;
  for (; i < stack->count; ++i)
    append_format(&trace, "{} -> ", stack->items[i]);
  failf("build dependency cycle: {}{}", contents(&trace), node);
}

static void visit_module(Graph *g, Module *m, Strings *stack);

static void read_task_inputs(Project *p, const Task *task, Strings *inputs) {
  read_module_paths(p, task->owner, inputs, task->input_paths);
  if (task->template)
    unique(inputs, task->template);
  if (task->data)
    unique(inputs, task->data);
}

static void visit_task(Graph *g, Task *task, Strings *stack) {
  const char *node = format_string("{}/{}", task->owner->name, task->name);
  if (task->state == 1)
    cycle(stack, node);
  if (task->state == 2)
    return;
  task->state = 1;
  task->selected = true;
  push(stack, node);
  Project *p = g->project;
  read_task_inputs(p, task, &task->inputs);
  if (same(task->engine, "copy") && task->inputs.count != 1)
    failf("{}: copy requires exactly one input", node);
  const char *program_root = task->owner->directory;
  if (task->tool && !find_module(g, task->tool)) {
    const Json *declared = member(member(p->manifest, "tools"), task->tool);
    if (declared) {
      const char *where = format_string("tools.{}", task->tool);
      check_fields(declared, where, " program args ");
      task->program = string(member(declared, "program"), where);
      task->tool = NULL;
      program_root = p->root;
      Strings args = {0};
      read_strings(&args, member(declared, "args"), where);
      for (size_t i = 0; i < task->args.count; ++i)
        push(&args, task->args.items[i]);
      task->args = args;
    }
  }
  for (size_t i = 0; i < task->args.count; ++i)
    task->args.items[i] = module_text(p, task->owner, task->args.items[i]);
  if (task->program) {
    const char *program = module_text(p, task->owner, task->program);
    task->program = find_program(program_root, program);
    if (!task->program)
      failf("{}: host program not found: {}", node, program);
  }
  if (task->tool) {
    Module *tool = dependency(g, task->owner, task->tool);
    bool native = same(p->host.platform, p->target.platform) &&
                  same(p->host.id, p->target.id);
    if (tool->kind != MODULE_EXECUTABLE || (tool->tools != &p->host && !native))
      failf("{}: task tool {} must be an executable built for the host", node,
            task->tool);
    visit_module(g, tool, stack);
  }
  for (size_t i = 0; i < task->inputs.count; ++i) {
    const char *input = task->inputs.items[i];
    Task *source = producer(g, input);
    if (source) {
      if (!visible(source->owner, task->owner))
        failf("{} cannot consume {} from {}", node, input, source->owner->name);
      visit_task(g, source, stack);
    } else if (!is_file(input)) {
      failf("{}: missing input {}", node, input);
    }
  }
  for (size_t i = 0; i < task->outputs.count; ++i)
    (void)producer(g, task->outputs.items[i]);
  --stack->count;
  task->state = 2;
}

static void merge_usage(Usage *out, const Usage *in) {
  for (size_t i = 0; i < in->includes.count; ++i)
    unique(&out->includes, in->includes.items[i]);
  for (size_t i = 0; i < in->flags.count; ++i)
    push(&out->flags, in->flags.items[i]);
  for (size_t i = 0; i < in->defines.count; ++i) {
    const Setting *entry = &in->defines.items[i];
    put(&out->defines, entry->name, entry->value, entry->origin);
  }
}

/* Artifact references denote graph edges even before their files exist. */
static const char *test_artifact(Graph *g, Module *m, const char *value) {
  const char *prefix = "${artifact:";
  if (!starts(value, prefix))
    return NULL;
  if (!ends(value, "}"))
    failf("{}: artifact references must occupy the complete test value", m->name);
  const char *name = slice(value + strlen(prefix),
                           strlen(value) - strlen(prefix) - 1);
  Module *dep = dependency(g, m, name);
  if (!dep->artifact || dep->tools != m->tools)
    failf("{}: test artifact {} must have an artifact in the same role", m->name,
          name);
  if (dep != m)
    unique(&m->runtime_deps, name);
  return dep->artifact;
}

static void resolve_test(Graph *g, Module *m) {
  Project *p = g->project;
  const char *program_root = m->directory;
  if (m->test_tool) {
    Module *tool = find_module(g, m->test_tool);
    if (tool) {
      if (!module_runnable(p, tool) || tool->kind != MODULE_EXECUTABLE)
        failf("{}: test tool {} must be a host-runnable executable", m->name,
              m->test_tool);
      m->test_program = test_artifact(
          g, m, format_string("{}{}{}", "${artifact:", m->test_tool, "}"));
    } else {
      const Json *declared = member(member(p->manifest, "tools"), m->test_tool);
      if (!declared)
        failf("{}: unknown test tool {}", m->name, m->test_tool);
      const char *where = format_string("tools.{}", m->test_tool);
      check_fields(declared, where, " program args ");
      m->test_program = string(member(declared, "program"), where);
      program_root = p->root;
      Strings args = {0};
      read_strings(&args, member(declared, "args"), where);
      for (size_t i = 0; i < m->test_args.count; ++i)
        push(&args, m->test_args.items[i]);
      m->test_args = args;
    }
  }
  if (m->test_program) {
    const char *artifact = test_artifact(g, m, m->test_program);
    if (artifact)
      m->test_program = artifact;
    else if (!(m->test_tool && find_module(g, m->test_tool))) {
      const char *program = module_text(p, m, m->test_program);
      m->test_program = find_program(program_root, program);
      if (!m->test_program)
        failf("{}: test program not found: {}", m->name, program);
    }
  }
  for (size_t i = 0; i < m->test_args.count; ++i) {
    const char *artifact = test_artifact(g, m, m->test_args.items[i]);
    m->test_args.items[i] = artifact ? artifact
                                    : module_text(p, m, m->test_args.items[i]);
  }
  for (size_t i = 0; i < m->test_environment.count; ++i)
    m->test_environment.items[i] = module_text(p, m, m->test_environment.items[i]);
}

static void visit_module(Graph *g, Module *m, Strings *stack) {
  if (m->state == 1)
    cycle(stack, m->name);
  if (m->state == 2)
    return;
  m->state = 1;
  m->selected = true;
  push(stack, m->name);
  if (m->package)
    resolve_package(g->project, m);
  if (m->kind == MODULE_EXECUTABLE || m->kind == MODULE_SHARED) {
    Project *p = g->project;
    Strings runtime = {0};
    read_strings(&runtime, m->tools->runtime_bins, m->tools->id);
    for (size_t i = 0; i < runtime.count; ++i)
      unique(&m->runtime_bins,
             path(p->root, module_text(p, m, runtime.items[i])));
  }
  if (m->test_name)
    resolve_test(g, m);
  Usage inherited = {0};
  const Strings *edges[] = {&m->public_deps, &m->private_deps};
  for (size_t j = 0; j < 2; ++j) {
    for (size_t i = 0; i < edges[j]->count; ++i) {
      Module *dep = dependency(g, m, edges[j]->items[i]);
      if (dep->tools != m->tools)
        failf("{} cannot link across Host/Target roles to {}", m->name,
              dep->name);
      if (dep->kind == MODULE_EXECUTABLE ||
          (dep->kind == MODULE_SHARED &&
           same(json_get_string(dep->data, "kind"), "cmeta_plugin")))
        failf("{} cannot link to {}; declare a tool or runtime dependency",
              m->name, dep->name);
      visit_module(g, dep, stack);
      merge_usage(&m->compile_usage, &dep->public_usage);
      if (j == 0)
        merge_usage(&inherited, &dep->public_usage);
    }
  }
  merge_usage(&m->compile_usage, &m->public_usage);
  merge_usage(&m->compile_usage, &m->private_usage);
  merge_usage(&inherited, &m->public_usage);
  inherited.libs = m->public_usage.libs;
  inherited.lib_dirs = m->public_usage.lib_dirs;
  inherited.link_flags = m->public_usage.link_flags;
  m->public_usage = inherited;
  for (size_t i = 0; i < g->task_count; ++i)
    if (g->tasks[i]->owner == m)
      visit_task(g, g->tasks[i], stack);
  const Usage *usages[] = {&m->public_usage, &m->private_usage};
  for (size_t i = 0; i < sizeof(usages) / sizeof(*usages); ++i)
    for (size_t j = 0; j < usages[i]->libs.count; ++j) {
      const char *library = usages[i]->libs.items[j];
      Task *task = producer(g, library);
      if (task) {
        if (!visible(task->owner, m))
          failf("{} cannot consume generated library {}", m->name, library);
        visit_task(g, task, stack);
      }
    }
  for (size_t i = 0; i < m->sources.count; ++i) {
    const char *source = m->sources.items[i];
    Task *task = producer(g, source);
    if (task) {
      if (!visible(task->owner, m))
        failf("{} cannot consume generated source {}", m->name, source);
      visit_task(g, task, stack);
    } else if (!is_file(source)) {
      failf("{}: source does not exist: {}", m->name, source);
    }
    if (contains(&m->objc_sources, source) &&
        (m->tools->msvc || !ends(source, ".c")))
      failf(
          "{}: sources.objc requires a .c file and an Objective-C compiler: {}",
          m->name, source);
    if (ends(source, ".cu")) {
      if (same(m->tools->os, "windows") && !m->tools->msvc)
        failf("{}: Windows CUDA compilation requires the MSVC host toolchain",
              m->name);
      const char *declared = optional_string(get(&m->options, "cudaCompiler"),
                                              m->name);
      if (!declared)
        failf("{}: CUDA sources require options.cudaCompiler", m->name);
      m->cuda_compiler = find_program(g->project->root,
                                      module_text(g->project, m, declared));
      if (!m->cuda_compiler)
        failf("{}: CUDA compiler not found: {}", m->name, declared);
      m->cxx = true;
    } else if (ends(source, ".cc") || ends(source, ".cpp") || ends(source, ".cxx") ||
        ends(source, ".C"))
      m->cxx = true;
    else if (!ends(source, ".c") && (m->tools->msvc || !ends(source, ".m")))
      failf("{}: unsupported source language: {}", m->name, source);
  }
  for (size_t i = 0; i < m->objc_sources.count; ++i)
    if (!contains(&m->sources, m->objc_sources.items[i]))
      failf("{}: sources.objc entry is not a selected source: {}", m->name,
            m->objc_sources.items[i]);
  if ((m->artifact != NULL) != (m->sources.count != 0))
    failf("{}: source list does not match its module kind", m->name);
  --stack->count;
  m->state = 2;
}

static void walk_link(const Graph *g, const Module *m, Strings *visited,
                      Strings *out) {
  if (contains(visited, m->name))
    return;
  push(visited, m->name);
  if (m->kind == MODULE_STATIC || m->kind == MODULE_HEADER_ONLY ||
      m->kind == MODULE_PREBUILT) {
    const Strings *edges[] = {&m->private_deps, &m->public_deps};
    for (size_t j = 0; j < 2; ++j)
      for (size_t i = edges[j]->count; i; --i)
        walk_link(g, find_module(g, edges[j]->items[i - 1]), visited, out);
  }
  push(out, m->name);
}

void link_modules(const Graph *g, const Module *m, Strings *order) {
  Strings reverse = {0}, visited = {0};
  const Strings *edges[] = {&m->private_deps, &m->public_deps};
  for (size_t j = 0; j < 2; ++j)
    for (size_t i = edges[j]->count; i; --i)
      walk_link(g, find_module(g, edges[j]->items[i - 1]), &visited, &reverse);
  for (size_t i = reverse.count; i; --i)
    push(order, reverse.items[i - 1]);
}

Graph load_graph(Project *p) {
  Graph g = {.project = p};
  load_artifact_names(&g);
  load_modules(&g);
  Strings stack = {0};
  if (p->request->targets.count) {
    for (size_t i = 0; i < p->request->targets.count; ++i) {
      const char *name = p->request->targets.items[i];
      Module *m = find_module(&g, name);
      if (!m)
        failf("unknown target '{}'", name);
      visit_module(&g, m, &stack);
    }
  } else {
    for (size_t i = 0; i < g.count; ++i)
      if (g.modules[i]->tools == &p->target ||
          (p->tests && g.modules[i]->test_name))
        visit_module(&g, g.modules[i], &stack);
  }
  bool changed;
  do {
    changed = false;
    for (size_t i = 0; i < g.count; ++i) {
      Module *m = g.modules[i];
      if (!m->selected)
        continue;
      for (size_t j = 0; j < m->runtime_deps.count; ++j) {
        Module *dep = dependency(&g, m, m->runtime_deps.items[j]);
        if (dep->tools != m->tools)
          failf("{}: incompatible runtime dependency {}", m->name, dep->name);
        if (!dep->selected) {
          visit_module(&g, dep, &stack);
          changed = true;
        }
      }
    }
  } while (changed);
  for (size_t i = 0; i < g.count; ++i) {
    Module *m = g.modules[i];
    if (m->selected && m->cmake_target)
      for (size_t j = 0; j < i; ++j) {
        const Module *other = g.modules[j];
        if (other->selected && same(m->cmake_target, other->cmake_target))
          failf("{} and {} export the same CMake target {}", m->name,
                other->name, m->cmake_target);
      }
    if (!m->selected || !m->artifact)
      continue;
    if (producer(&g, m->artifact) ||
        (m->import_lib && producer(&g, m->import_lib)))
      failf("{}: module artifact conflicts with a task output", m->name);
    for (size_t j = 0; j < i; ++j) {
      Module *other = g.modules[j];
      if (other->selected && (same(m->artifact, other->artifact) ||
                              same(m->artifact, other->import_lib) ||
                              same(m->import_lib, other->artifact) ||
                              same(m->import_lib, other->import_lib)))
        failf("{} and {} produce the same binary", m->name, other->name);
    }
  }
  return g;
}

static const char *dot_escape(const char *value) {
  Text out = {0};
  for (const char *c = value; *c; ++c) {
    if (*c == '\n' || *c == '\r') {
      append(&out, *c == '\n' ? "\\n" : "\\r");
      continue;
    }
    if (*c == '"' || *c == '\\')
      append(&out, "\\");
    append_n(&out, c, 1);
  }
  return contents(&out);
}

static const char *module_folder(const Project *p, const Module *m) {
  if (!inside(p->root, m->directory))
    return "external";
  const char *directory = m->directory + strlen(p->root);
  return *directory == '/' ? directory + 1 : directory;
}

static void print_folder(const Graph *g, const char *folder,
                         const char *indent) {
  const char *prefix = *folder ? format_string("{}/", folder) : "";
  const size_t prefix_length = strlen(prefix);
  Strings children = {0};
  for (size_t i = 0; i < g->count; ++i) {
    const Module *m = g->modules[i];
    if (!m->selected)
      continue;
    const char *directory = module_folder(g->project, m);
    if (same(directory, folder)) {
      print_line("{}\"{}\";", indent, m->name);
    } else if (starts(directory, prefix)) {
      const char *child = directory + prefix_length;
      unique(&children, slice(child, strcspn(child, "/")));
    }
  }
  sort_strings(&children);
  const char *nested_indent = format_string("{}  ", indent);
  for (size_t i = 0; i < children.count; ++i) {
    const char *child = children.items[i];
    const char *child_folder = format_string("{}{}", prefix, child);
    print_line("{}subgraph \"cluster_{}\" {}", indent, dot_escape(child_folder),
               "{");
    print_line("{}label=\"{}\";", nested_indent, dot_escape(child));
    print_folder(g, child_folder, nested_indent);
    print_line("{}{}", indent, "}");
  }
}

void print_graph(const Graph *g) {
  puts("digraph build {");
  print_folder(g, "", "  ");
  for (size_t i = 0; i < g->count; ++i) {
    const Module *m = g->modules[i];
    if (!m->selected)
      continue;
    const Strings *edges[] = {&m->public_deps, &m->private_deps,
                              &m->runtime_deps};
    const char *types[] = {"public", "private", "runtime"};
    for (size_t j = 0; j < 3; ++j)
      for (size_t k = 0; k < edges[j]->count; ++k)
        print_line("  \"{}\" -> \"{}\" [label=\"{}\"];", m->name,
                   edges[j]->items[k], types[j]);
  }
  puts("}");
}

static Json *graph_strings(const Strings *strings) {
  Json *array = json_create_array();
  for (size_t i = 0; i < strings->count; ++i)
    json_add(array, json_create_string(strings->items[i]));
  return array;
}

static bool header_filename(const char *name) {
  const char *extension = strrchr(name, '.');
  const char *const extensions[] = {".h",   ".H",   ".hh",  ".hpp", ".hxx",
                                    ".inc", ".inl", ".ipp", ".tcc"};
  for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); ++i)
    if (same(extension, extensions[i]))
      return true;
  return false;
}

static void scan_headers(const Project *p, Strings *headers,
                         const char *directory, const char *pattern,
                         unsigned depth) {
  if (inside(p->build_dir, directory) || !is_dir(directory) || is_link(directory))
    return;
  Strings entries = {0};
  list_dir(directory, &entries);
  for (size_t i = 0; i < entries.count; ++i) {
    const char *name = entries.items[i];
    const char *file = path(directory, name);
    if (depth && is_dir(file))
      scan_headers(p, headers, file, pattern, depth - 1);
    else if ((pattern ? filename_matches(pattern, name) : header_filename(name)) &&
             is_file(file))
      unique(headers, file);
  }
}

static Json *graph_headers(const Project *p, const Module *m,
                           const Strings *outputs) {
  Strings headers = {0}, include_dirs = {0}, source_dirs = {0};
  const Strings *includes[] = {&m->declared_public_includes,
                              &m->private_usage.includes};
  for (size_t i = 0; i < sizeof(includes) / sizeof(*includes); ++i)
    for (size_t j = 0; j < includes[i]->count; ++j) {
      const char *dir = includes[i]->items[j];
      if (inside(m->directory, dir))
        unique(&include_dirs, dir);
    }
  for (size_t i = 0; i < include_dirs.count; ++i) {
    const char *dir = include_dirs.items[i];
    scan_headers(p, &headers, dir, NULL, same(dir, m->directory) ? 0 : 128);
  }
  for (size_t i = 0; i < m->sources.count; ++i) {
    const char *dir = parent(m->sources.items[i]);
    if (inside(m->directory, dir) && !contains(&include_dirs, dir))
      unique(&source_dirs, dir);
  }
  for (size_t i = 0; i < source_dirs.count; ++i)
    scan_headers(p, &headers, source_dirs.items[i], NULL, 0);

  for (size_t i = 0; i < m->install_count; ++i) {
    const InstallMapping *mapping = &m->install[i];
    if (mapping->component != INSTALL_DEV)
      continue;
    const char *from = mapping->from;
    if (strchr(from, '*'))
      scan_headers(p, &headers, parent(from), filename(from), 0);
    else if (is_file(from))
      unique(&headers, from);
    else
      scan_headers(p, &headers, from, "*", 128);
  }
  for (size_t i = 0; i < outputs->count; ++i)
    if (header_filename(filename(outputs->items[i])))
      unique(&headers, outputs->items[i]);
  sort_strings(&headers);
  return graph_strings(&headers);
}

static void graph_task_files(const Graph *g, const Module *m, Strings *inputs,
                             Strings *outputs) {
  for (size_t i = 0; i < g->task_count; ++i) {
    const Task *task = g->tasks[i];
    if (task->owner != m)
      continue;
    Strings resolved_inputs = task->inputs;
    if (!task->selected)
      read_task_inputs(g->project, task, &resolved_inputs);
    for (size_t j = 0; j < resolved_inputs.count; ++j)
      unique(inputs, resolved_inputs.items[j]);
    for (size_t j = 0; j < task->outputs.count; ++j)
      unique(outputs, task->outputs.items[j]);
  }
}

void print_graph_json(const Graph *g) {
  const Project *p = g->project;
  Json *result = own(json_create_object());
  json_set(result, "preset", json_create_string(p->name));
  json_set(result, "platform", json_create_string(p->target.platform));
  json_set(result, "compiler", json_create_string(p->target.driver));
  json_set(result, "buildDirectory", json_create_string(p->build_dir));
  json_set(result, "inputs", graph_strings(&p->inputs));
  Json *targets = json_create_array();
  json_set(result, "targets", targets);
  for (size_t i = 0; i < g->count; ++i) {
    const Module *m = g->modules[i];
    if (!m->selected && p->request->targets.count)
      continue;
    Strings task_inputs = {0}, task_outputs = {0};
    graph_task_files(g, m, &task_inputs, &task_outputs);
    Json *target = json_create_object();
    json_add(targets, target);
    json_set(target, "name", json_create_string(m->name));
    json_set(target, "folder", json_create_string(module_folder(p, m)));
    json_set(target, "manifest", json_create_string(m->manifest));
    json_set(target, "sources", graph_strings(&m->sources));
    json_set(target, "headers", graph_headers(p, m, &task_outputs));
    json_set(target, "taskInputs", graph_strings(&task_inputs));
    json_set(target, "taskOutputs", graph_strings(&task_outputs));
    json_set(target, "kind",
             json_create_string(json_get_string(m->data, "kind")));
    json_set(target, "runnable", json_create_bool(module_runnable(p, m)));
    json_set(target, "test",
             json_create_bool(m->test_name && (p->native || m->tools == &p->host)));
    json_set(target, "testName", m->test_name ? json_create_string(m->test_name)
                                              : json_create_null());
    json_set(target, "publicDeps", graph_strings(&m->public_deps));
    json_set(target, "privateDeps", graph_strings(&m->private_deps));
    json_set(target, "runtimeDeps", graph_strings(&m->runtime_deps));
  }
  char *data = json_serialize_pretty(result, NULL);
  if (!data)
    fail("cannot serialize target graph");
  puts(data);
  json_serialize_free(data);
}

void explain_module(const Graph *g, const char *name) {
  const Module *m = find_module(g, name);
  if (!m || !m->selected)
    failf("target '{}' is outside the selected graph", name);
  print_line("{} ({})", m->name, m->tools->id);
  for (size_t i = 0; i < m->compile_usage.includes.count; ++i)
    print_line("  include {}", m->compile_usage.includes.items[i]);
  for (size_t i = 0; i < m->compile_usage.defines.count; ++i) {
    Setting entry = m->compile_usage.defines.items[i];
    const char *value = json_type(entry.value) == JSON_NULL
                            ? NULL
                            : string(entry.value, entry.origin);
    print_line("  define {}{}{} [{}]", entry.name, value ? "=" : "",
               value ? value : "", entry.origin);
  }
  for (size_t i = 0; i < m->options.count; ++i)
    print_line("  option {} [{}]", m->options.items[i].name,
               m->options.items[i].origin);
  Strings link = {0};
  link_modules(g, m, &link);
  for (size_t i = 0; i < link.count; ++i)
    print_line("  link {}", link.items[i]);
}
