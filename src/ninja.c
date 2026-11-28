#include "build.h"
#include "module.h"
#include "template.h"

#include <string.h>

Struct(NinjaEdge, (vstr, rule), (vstr, outputs), (vstr, implicit_outputs),
       (vstr, inputs), (vstr, implicit_inputs), (vstr, order_only), (vstr, run),
       (vstr, response), (vstr, argv), (vstr, dep));
Struct(NinjaRule, (vstr, name), (vstr, kind));

typedef enum {
  RULE_COMPILE_GNU,
  RULE_COMPILE_MSVC,
  RULE_ARCHIVE,
  RULE_LINK,
  RULE_TASK,
  RULE_REGENERATE,
  RULE_PHONY,
  RULE_COUNT
} RuleKind;

static const char *const rule_kinds[RULE_COUNT] = {
    [RULE_COMPILE_GNU] = "compileGnu",
    [RULE_COMPILE_MSVC] = "compileMsvc",
    [RULE_ARCHIVE] = "archive",
    [RULE_LINK] = "link",
    [RULE_TASK] = "task",
    [RULE_REGENERATE] = "regenerate",
    [RULE_PHONY] = "phony"};

typedef struct {
  NinjaEdge *items;
  size_t count;
  size_t capacity;
  NinjaRule *rules;
  size_t rule_count;
  size_t rule_capacity;
  const char *rule_names[RULE_COUNT];
  int compile_depth;
} NinjaEdges;

typedef enum {
  NINJA_MANIFEST,
  NINJA_RULES,
  ARTIFACT_NAMES,
  COMPILE_MSVC,
  COMPILE_GNU,
  COMPILE_CUDA,
  LINK_MSVC,
  LINK_MINGW,
  LINK_ELF,
  LINK_MACHO,
  ARCHIVE_MSVC,
  ARCHIVE_GNU,
  SDK_JSON,
  SDK_PC,
  TEMPLATE_COUNT
} TemplateId;

static const char *const template_names[TEMPLATE_COUNT] = {
    [NINJA_MANIFEST] = "build.ninja.j2",
    [NINJA_RULES] = "ninja-rules.json",
    [COMPILE_MSVC] = "compile_msvc.json.j2",
    [COMPILE_GNU] = "compile_gnu.json.j2",
    [COMPILE_CUDA] = "compile_nvcc.json.j2",
    [LINK_MSVC] = "link_msvc.json.j2",
    [LINK_MINGW] = "link_mingw.json.j2",
    [LINK_ELF] = "link_elf.json.j2",
    [LINK_MACHO] = "link_macho.json.j2",
    [ARCHIVE_MSVC] = "archive_msvc.json.j2",
    [ARCHIVE_GNU] = "archive_gnu.json.j2",
    [SDK_JSON] = "pkg.json.j2",
    [SDK_PC] = "pkg.pc.j2",
};

static TemplateId compile_template(const Toolchain *tools) {
  return tools->msvc ? COMPILE_MSVC : COMPILE_GNU;
}

static TemplateId link_template(const Module *module) {
  const Toolchain *tools = module->tools;
  if (module->kind == MODULE_STATIC)
    return tools->msvc ? ARCHIVE_MSVC : ARCHIVE_GNU;
  if (tools->msvc)
    return LINK_MSVC;
  if (same(tools->os, "windows"))
    return LINK_MINGW;
  if (same(tools->os, "macos") || same(tools->os, "ios"))
    return LINK_MACHO;
  return LINK_ELF;
}

static void render_link_args(const BuildTemplate *templ, const Module *m,
                             const char *pdb, const Strings *lib_dirs,
                             Strings *args);

typedef struct {
  int link_pool_depth;
  int compile_pool_depth;
  cmeta_data_collection_view edges;
  cmeta_data_collection_view rules;
} NinjaTemplateData;

cmeta_reflect_data(NinjaTemplateData, "slate.ninja.template",
                   cmeta_data_field(int, link_pool_depth, &cmeta_data_int)
                       cmeta_data_field(int, compile_pool_depth, &cmeta_data_int)
                       cmeta_data_field(cmeta_data_collection_view, edges,
                                        &cmeta_data_sequence_view,
                                        &cmeta_type_collection_view)
                       cmeta_data_field(cmeta_data_collection_view, rules,
                                        &cmeta_data_sequence_view,
                                        &cmeta_type_collection_view));

static void add_items(Strings *out, const Strings *in) {
  for (size_t i = 0; i < in->count; ++i)
    push(out, in->items[i]);
}

static vstr edge_files(const Strings *files) {
  Text out = {0};
  for (size_t i = 0; i < files->count; ++i) {
    if (i)
      append(&out, " ");
    append(&out, ninja_path(files->items[i]));
  }
  return vstr_from_buf(contents(&out), out.length);
}

static void add_rule(NinjaEdges *edges, RuleKind kind, const char *name,
                     const char *where) {
  const char *category = rule_kinds[kind];
  for (size_t i = 0; i < edges->rule_count; ++i) {
    const NinjaRule *existing = &edges->rules[i];
    if (!same(existing->name.data, name))
      continue;
    if (!same(existing->kind.data, category))
      failf("{}: Ninja rule '{}' is already used for {}", where, name,
            existing->kind);
    return;
  }
  edges->rules = reserve(edges->rules, edges->rule_count + 1,
                         &edges->rule_capacity, sizeof(*edges->rules));
  edges->rules[edges->rule_count++] = (NinjaRule){
      .name = vstr_from_cstr(name), .kind = vstr_from_cstr(category)};
}

static void init_rules(NinjaEdges *edges, const BuildTemplate *source) {
  Json *data = json_parse(source->source.data, source->source.len);
  if (!data)
    failf("{}: {}", source->file, json_get_error());
  own(data);
  for (size_t i = 0; i < RULE_COUNT; ++i) {
    const char *name = string(member(data, rule_kinds[i]), source->file);
    if (!identifier(name))
      failf("{}: invalid Ninja rule name '{}'", source->file, name);
    add_rule(edges, (RuleKind)i, name, source->file);
    edges->rule_names[i] = name;
  }
}

static NinjaEdge *new_edge(NinjaEdges *edges, RuleKind kind, const Module *m) {
  const char *rule = edges->rule_names[kind];
  if (m) {
    const char *key = kind == RULE_COMPILE_MSVC || kind == RULE_COMPILE_GNU
                          ? "compile"
                          : rule_kinds[kind];
    const char *name = optional_string(get(&m->ninja_rules, key), m->name);
    if (name) {
      add_rule(edges, kind, name, m->name);
      rule = name;
    }
  }
  edges->items = reserve(edges->items, edges->count + 1, &edges->capacity,
                         sizeof(*edges->items));
  NinjaEdge *edge = &edges->items[edges->count++];
  edge->rule = vstr_from_cstr(rule);
  return edge;
}

static vstr edge_value(const char *value) {
  return vstr_from_cstr(ninja_value(value));
}

static void invoke(NinjaEdges *edges, const Module *m, const Strings *outputs,
                   const Strings *inputs, const Strings *args) {
  NinjaEdge *edge = new_edge(edges, RULE_TASK, m);
  edge->outputs = edge_files(outputs);
  edge->inputs = edge_files(inputs);
  edge->run = edge_value(command(args));
}

static const char *opt(const Module *m, const char *key, const char *fallback) {
  const Setting *value = setting(&m->options, key);
  return value ? string(value->value, value->origin) : fallback;
}

const char *module_pdb(const Module *m) {
  if (m->tools->msvc &&
      (m->kind == MODULE_EXECUTABLE || m->kind == MODULE_SHARED) &&
      !same(opt(m, "debugInfo", "none"), "none"))
    return format_string("{}{}", m->artifact, m->tools->artifacts.pdb_suffix);
  return NULL;
}

const char *symbol_format(const Module *m) {
  if ((m->kind != MODULE_EXECUTABLE && m->kind != MODULE_SHARED) ||
      !*m->tools->artifacts.symbols_suffix ||
      same(opt(m, "debugInfo", "none"), "none"))
    return NULL;
  const char *os = m->tools->os;
  if (same(os, "macos") || same(os, "ios"))
    return "macho";
  if (same(os, "linux") || same(os, "android"))
    return "elf";
  return NULL;
}

const char *symbol_output(const Graph *g, const Module *m) {
  return path(g->project->build_dir,
              format_string("debug/{}/{}{}", artifact_install_dir(g, m->artifact),
                            filename(m->artifact), m->tools->artifacts.symbols_suffix));
}

const char *dsym_dwarf(const char *bundle, const char *artifact) {
  return path(path(bundle, "Contents/Resources/DWARF"), filename(artifact));
}

static bool boolean(const Module *m, const char *key, bool fallback) {
  const Setting *value = setting(&m->options, key);
  if (!value)
    return fallback;
  if (json_type(value->value) != JSON_BOOL)
    failf("{}.{}: expected boolean", value->origin, key);
  return json_bool(value->value);
}

Struct(CompilerPolicy, (vstr, optimization), (vstr, debug_info),
       (vstr, warnings), (vstr, runtime_lib), (vstr, rtti), (vstr, exceptions),
       (vstr, lto), (vstr, sanitizers), (vstr, symbol_visibility),
       (bool, visibility_inlines_hidden), (bool, debug_crt), (bool, werror));

Struct(CompileOptions, (CompilerPolicy, policy), (vstr, standard),
       (vstr, triple), (vstr, darwin_arch), (vstr, sysroot), (vstr, source),
       (vstr, output), (vstr, depfile), (bool, cxx), (bool, objc), (bool, pic),
       (cmeta_data_collection_view, includes),
       (cmeta_data_collection_view, defines),
       (cmeta_data_collection_view, flags));

static void render_compile_args(const BuildTemplate *templ, const Module *m,
                                const CompileOptions *options, Strings *args);

static vstr option_choice(const Module *m, const char *key,
                          const char *const *choices) {
  const char *value = opt(m, key, choices[0]);
  for (size_t i = 0; choices[i]; ++i)
    if (same(value, choices[i]))
      return vstr_from_cstr(value);
  failf("{}: unsupported {} '{}'", m->name, key, value);
}

static vstr compile_toggle(const Module *m, const char *key, bool cxx) {
  if (!cxx || !get(&m->options, key))
    return vstr_from_cstr(NULL);
  return vstr_from_cstr(boolean(m, key, true) ? "enabled" : "disabled");
}

static cmeta_data_collection_view string_view(const Strings *strings) {
  vstr *values = NULL;
  size_t capacity = 0;
  if (strings->count)
    values = reserve(NULL, strings->count, &capacity, sizeof(*values));
  for (size_t i = 0; i < strings->count; ++i)
    values[i] = vstr_from_cstr(strings->items[i]);
  return (cmeta_data_collection_view){values, strings->count, sizeof(*values),
                                      jinja_cmeta_vstr_data()};
}

static CompileOptions compile_options(const Project *p, const Module *m) {
  const Toolchain *t = m->tools;
  Strings defines = {0};
  for (size_t i = 0; i < m->compile_usage.defines.count; ++i) {
    const Setting define = m->compile_usage.defines.items[i];
    const char *value = json_type(define.value) == JSON_NULL
                            ? NULL
                            : string(define.value, define.origin);
    push(&defines,
         value ? format_string("{}={}", define.name, value) : define.name);
  }
  CompileOptions options = {
      .policy =
          {
              .optimization = option_choice(
                  m, "optimization",
                  (const char *const[]){"none", "size", "speed", NULL}),
              .debug_info = option_choice(
                  m, "debugInfo",
                  (const char *const[]){"none", "full", "minimal", NULL}),
              .warnings = option_choice(
                  m, "warnings",
                  (const char *const[]){"default", "none", "strict", NULL}),
              .lto = option_choice(
                  m, "lto",
                  (const char *const[]){"none", "thin", "full", NULL}),
              .sanitizers = vstr_from_cstr(opt(m, "sanitizers", "")),
              .symbol_visibility = option_choice(
                  m, "symbolVisibility",
                  (const char *const[]){"default", "hidden", NULL}),
              .debug_crt = same(p->config, "debug"),
              .werror = boolean(m, "werror", false),
          },
      .triple = vstr_from_cstr(compiler_target(t)),
      .darwin_arch = vstr_from_cstr(darwin_arch(t)),
      .sysroot = vstr_from_cstr(t->sysroot),
      .pic = !same(t->os, "windows") &&
             boolean(m, "pic", m->kind != MODULE_EXECUTABLE),
      .includes = string_view(&m->compile_usage.includes),
      .defines = string_view(&defines),
      .flags = string_view(&m->compile_usage.flags)};
  if (same(t->os, "windows"))
    options.policy.symbol_visibility = vstr_from_cstr("default");
  if (same(options.policy.lto.data, "thin") && !same(t->driver, "clang") &&
      !same(t->driver, "llvm-mingw"))
    failf("{}: thin LTO requires a Clang driver", m->name);
  if (t->msvc)
    options.policy.runtime_lib = option_choice(
        m, "runtimeLib", (const char *const[]){"dynamic", "static", NULL});
  return options;
}

static void generated_headers(const Graph *g, const Module *m,
                              Strings *headers) {
  for (size_t i = 0; i < g->task_count; ++i) {
    const Task *task = g->tasks[i];
    if (!task->selected)
      continue;
    for (size_t j = 0; j < task->outputs.count; ++j) {
      const char *file = task->outputs.items[j];
      for (size_t k = 0; k < m->compile_usage.includes.count; ++k) {
        if (inside(m->compile_usage.includes.items[k], file)) {
          unique(headers, file);
          break;
        }
      }
    }
  }
}

static void response(NinjaEdge *edge, const Strings *program,
                     const Strings *args, const char *output,
                     const Toolchain *tools, const char *file_option) {
  const char *rsp =
      format_string("{}{}", output, tools->artifacts.response_suffix);
  Strings invoke = {0};
  add_items(&invoke, program);
  if (file_option) {
    push(&invoke, file_option);
    push(&invoke, rsp);
  } else
    push(&invoke, format_string("@{}", rsp));
  edge->run = edge_value(command(&invoke));
  edge->response = edge_value(rsp);
  // MSVC otherwise decodes response files in the active Windows code page.
  // NVCC's options file has its own parser and must retain its native format.
  edge->argv = edge_value(format_string("{}{}",
                                       tools->msvc && !file_option
                                           ? "\xef\xbb\xbf" : "",
                                       command(args)));
}

static void cuda_common_args(const Graph *g, const Module *m, Strings *args) {
  push(args, format_string("--compiler-bindir={}", m->tools->cxx));
  const char *arch = opt(m, "cudaArchitecture", NULL);
  if (arch) {
    if (!*arch || arch[strspn(arch, "0123456789")])
      failf("{}: cudaArchitecture must be a numeric compute capability", m->name);
    push(args, format_string("--generate-code=arch=compute_{},code=[compute_{},sm_{}]",
                             arch, arch, arch));
  }
  if (!same(opt(m, "lto", "none"), "none") ||
      *opt(m, "sanitizers", ""))
    failf("{}: CUDA compilation does not support lto or sanitizers", m->name);
  Strings flags = {0};
  read_strings(&flags, get(&m->options, "cudaFlags"), m->name);
  for (size_t i = 0; i < flags.count; ++i)
    push(args, module_text(g->project, m, flags.items[i]));
}

static const char *compile_path(const char *directory, const char *file) {
  size_t i = 0, shared = 0;
  while (directory[i] && file[i] && directory[i] == file[i]) {
    if (file[i] == '/')
      shared = i + 1;
    ++i;
  }
  if (!directory[i] && (file[i] == '/' || !file[i]))
    shared = i + (file[i] == '/');
  if (!shared)
    return file;
  Text relative = {0};
  for (const char *part = directory + (shared > i ? i : shared); *part; ++part)
    if (*part == '/' || !part[1])
      append(&relative, "../");
  append(&relative, file + shared);
  return relative.length ? contents(&relative) : ".";
}

static void compile_module(NinjaEdges *edges, Json *database, const Graph *g,
                           Module *m, const BuildTemplate *templates) {
  const Project *p = g->project;
  const Toolchain *t = m->tools;
  Strings headers = {0};
  generated_headers(g, m, &headers);
  CompileOptions options = compile_options(p, m);
  if (t->msvc) {
    // Keep MSVC's include output relative to Ninja's working directory. Its
    // Windows include normalizer otherwise mixes code pages for Unicode roots.
    Strings includes = {0};
    for (size_t i = 0; i < m->compile_usage.includes.count; ++i)
      push(&includes, compile_path(p->build_dir, m->compile_usage.includes.items[i]));
    options.includes = string_view(&includes);
  }
  const BuildTemplate *templ = &templates[compile_template(t)];
  const char *subdir = t == &p->host ? "host/obj" : "obj";
  for (size_t i = 0; i < m->sources.count; ++i) {
    const char *source = m->sources.items[i];
    const bool cuda = ends(source, ".cu");
    const bool cxx = !ends(source, ".c") && !ends(source, ".m");
    const char *output = path(
        p->build_dir, format_string("{}/{}/{}{}", subdir, m->name,
                                    i, t->artifacts.object_suffix));
    mkdirs(parent(output));
    if (producer(g, output))
      failf("{} conflicts with an object output", output);
    push(&m->objects, output);
    Strings program = {0}, args = {0};
    if (t->launcher)
      push(&program, t->launcher);
    push(&program, cuda ? m->cuda_compiler : cxx ? t->cxx : t->cc);
    const char *depfile =
        t->msvc && !cuda ? NULL
                : format_string("{}{}", output, t->artifacts.depfile_suffix);
    const char *standard = cxx ? "cxxStd" : "cStd";
    options.source = vstr_from_cstr(t->msvc && !cuda
                                       ? compile_path(p->build_dir, source) : source);
    options.output = vstr_from_cstr(output);
    options.depfile = vstr_from_cstr(depfile);
    options.cxx = cxx;
    options.objc = contains(&m->objc_sources, source);
    const char *dialect = cuda ? opt(m, "cudaStd", opt(m, "cxxStd", "c++17"))
                               : opt(m, standard, NULL);
    if (dialect && (!*dialect || dialect[strspn(
                                      dialect,
                                      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-")]))
      failf("{}: invalid {} '{}'", m->name, standard, dialect);
    options.standard = vstr_from_cstr(dialect);
    options.policy.visibility_inlines_hidden =
        cxx && !same(t->os, "windows") && boolean(m, "visibilityInlinesHidden", false);
    options.policy.rtti = compile_toggle(m, "rtti", cxx);
    options.policy.exceptions = compile_toggle(m, "exceptions", cxx);
    if (cuda) {
      CompileOptions cuda_options = options;
      Strings flags = {0};
      cuda_common_args(g, m, &flags);
      for (size_t j = 0; j < m->compile_usage.flags.count; ++j)
        push(&flags, format_string("-Xcompiler={}", m->compile_usage.flags.items[j]));
      Strings host_flags = {0};
      read_strings(&host_flags, get(&m->options, "cudaHostFlags"), m->name);
      for (size_t j = 0; j < host_flags.count; ++j)
        push(&flags, format_string("-Xcompiler={}",
                                  module_text(g->project, m, host_flags.items[j])));
      if (boolean(m, "cudaRdc", false))
        push(&flags, "--relocatable-device-code=true");
      cuda_options.flags = string_view(&flags);
      render_compile_args(&templates[COMPILE_CUDA], m, &cuda_options, &args);
    } else {
      Strings flags = {0}, language_flags = {0};
      add_items(&flags, &m->compile_usage.flags);
      read_strings(&language_flags, get(&m->options, cxx ? "cxxFlags" : "cFlags"),
                    m->name);
      for (size_t j = 0; j < language_flags.count; ++j)
        push(&flags, module_text(g->project, m, language_flags.items[j]));
      options.flags = string_view(&flags);
      render_compile_args(templ, m, &options, &args);
    }
    NinjaEdge *edge =
        new_edge(edges, t->msvc && !cuda ? RULE_COMPILE_MSVC : RULE_COMPILE_GNU, m);
    edge->outputs = vstr_from_cstr(ninja_path(output));
    edge->inputs = vstr_from_cstr(ninja_path(source));
    Strings order = {0};
    for (size_t j = 0; j < headers.count; ++j) {
      if (!same(source, headers.items[j]))
        push(&order, headers.items[j]);
    }
    // Ninja normalizes MSVC /showIncludes paths relative to the build directory.
    // Keep an explicit dependency when it differs from a task's output path.
    if (t->msvc && !cuda)
      edge->implicit_inputs = edge_files(&order);
    else
      edge->order_only = edge_files(&order);
    response(edge, &program, &args, output, t, cuda ? "--options-file" : NULL);
    if (depfile)
      edge->dep = edge_value(depfile);
    Json *entry = json_create_object();
    json_add(database, entry);
    json_set(entry, "directory", json_create_string(p->build_dir));
    json_set(entry, "file", json_create_string(source));
    json_set(entry, "output", json_create_string(output));
    Json *argv = json_create_array();
    json_set(entry, "arguments", argv);
    for (size_t j = 0; j < program.count; ++j)
      json_add(argv, json_create_string(program.items[j]));
    for (size_t j = 0; j < args.count; ++j)
      json_add(argv, json_create_string(args.items[j]));
  }
}

static void track_search_library(const Module *m, const Strings *dirs,
                                 const char *lib, Strings *inputs) {
  const ArtifactNames *names = &m->tools->artifacts;
  const char *candidates[] = {
      format_string("{}{}{}", names->static_prefix, lib, names->static_suffix),
      format_string("{}{}{}", names->shared_prefix, lib, names->shared_suffix),
      format_string("{}{}{}", names->import_prefix, lib, names->import_suffix)};
  for (size_t i = 0; i < dirs->count; ++i)
    for (size_t j = 0; j < sizeof(candidates) / sizeof(*candidates); ++j) {
      const char *file = path(dirs->items[i], candidates[j]);
      if (is_file(file))
        unique(inputs, file);
    }
}

static void library_flags(const Graph *g, const Module *m, const Usage *usage,
                          const Strings *dirs, Strings *args, Strings *inputs) {
  const Strings *libs = &usage->libs;
  for (size_t i = 0; i < libs->count; ++i) {
    const char *lib = libs->items[i];
    if (strchr(lib, '/') || strchr(lib, '\\')) {
      if (!is_file(lib) && !producer(g, lib))
        failf("{}: link library not found: {}", m->name, lib);
      unique(inputs, lib);
      push(args, lib);
    } else if (m->tools->msvc) {
      const char *name = ends(lib, ".lib") ? slice(lib, strlen(lib) - 4) : lib;
      if (dirs->count)
        track_search_library(m, dirs, name, inputs);
      push(args, format_string("{}.lib", name));
    } else {
      if (dirs->count && !starts(lib, "-"))
        track_search_library(m, dirs, lib, inputs);
      push(args, starts(lib, "-") ? lib : format_string("-l{}", lib));
    }
  }
  add_items(args, &usage->link_flags);
}

static void collect_lib_dirs(Strings *dirs, const Usage *usage) {
  for (size_t i = 0; i < usage->lib_dirs.count; ++i)
    unique(dirs, usage->lib_dirs.items[i]);
}

static bool link_private(const Module *m) {
  return m->kind == MODULE_STATIC || m->kind == MODULE_PREBUILT;
}

static void compare_abi_option(const Module *m, const Module *dep,
                               const char *key, const char *fallback) {
  const bool equal = fallback
                         ? same(opt(m, key, fallback), opt(dep, key, fallback))
                         : boolean(m, key, true) == boolean(dep, key, true);
  if (!equal)
    failf("{} and {} have incompatible {} in the same link unit",
          m->name, dep->name, key);
}

static void check_link_abi(const Graph *g, const Module *m,
                            const Strings *modules) {
  bool strict = boolean(m, "abiCritical", false);
  for (size_t i = 0; i < modules->count && !strict; ++i) {
    const Module *dep = find_module(g, modules->items[i]);
    strict = dep->kind == MODULE_STATIC && boolean(dep, "abiCritical", false);
  }
  if (!strict)
    return;
  const Module *cxx = m->cxx ? m : NULL;
  for (size_t i = 0; i < modules->count; ++i) {
    const Module *dep = find_module(g, modules->items[i]);
    if (dep->kind != MODULE_STATIC)
      continue;
    if (m->tools->msvc)
      compare_abi_option(m, dep, "runtimeLib", "dynamic");
    compare_abi_option(m, dep, "sanitizers", "");
    if (!dep->cxx)
      continue;
    if (!cxx)
      cxx = dep;
    else {
      compare_abi_option(cxx, dep, "cxxStd", "");
      compare_abi_option(cxx, dep, "rtti", NULL);
      compare_abi_option(cxx, dep, "exceptions", NULL);
    }
  }
}

static void link_module(NinjaEdges *edges, const Graph *g, const Module *m,
                        const BuildTemplate *templates) {
  const Toolchain *t = m->tools;
  Strings inputs = {0}, args = {0}, program = {0}, implicit = {0};
  add_items(&inputs, &m->objects);
  if (m->cuda_compiler && boolean(m, "cudaDeviceLink", false)) {
    if (!boolean(m, "cudaRdc", false))
      failf("{}: cudaDeviceLink requires cudaRdc", m->name);
    const char *directory = m->tools == &g->project->host ? "host/obj" : "obj";
    const char *output = path(g->project->build_dir,
        format_string("{}/{}/cuda_device_link{}", directory, m->name,
                      t->artifacts.object_suffix));
    if (producer(g, output))
      failf("{} conflicts with a CUDA device-link output", output);
    Strings cuda_program = {0}, cuda_args = {0};
    push(&cuda_program, m->cuda_compiler);
    cuda_common_args(g, m, &cuda_args);
    push(&cuda_args, "--device-link");
    if (t->msvc) {
      const char *runtime = opt(m, "runtimeLib", "dynamic");
      push(&cuda_args, format_string("-Xcompiler=/{}{}",
           same(runtime, "dynamic") ? "MD" : "MT",
           same(g->project->config, "debug") ? "d" : ""));
    }
    add_items(&cuda_args, &m->objects);
    push(&cuda_args, "-o");
    push(&cuda_args, output);
    NinjaEdge *device = new_edge(edges, RULE_LINK, m);
    device->outputs = vstr_from_cstr(ninja_path(output));
    device->inputs = edge_files(&m->objects);
    response(device, &cuda_program, &cuda_args, output, t, "--options-file");
    push(&inputs, output);
  }
  const BuildTemplate *templ = &templates[link_template(m)];
  RuleKind rule = RULE_ARCHIVE;
  if (m->kind == MODULE_STATIC) {
    push(&program, executable_path());
    push(&program, "_archive");
    push(&program, m->artifact);
    push(&program, t->ar);
    render_link_args(templ, m, NULL, &(Strings){0}, &args);
    add_items(&args, &inputs);
  } else {
    rule = RULE_LINK;
    push(&program, t->ld);
    const char *pdb = module_pdb(m);
    if (pdb)
      push(&implicit, pdb);
    Strings modules = {0}, dirs = {0};
    link_modules(g, m, &modules);
    check_link_abi(g, m, &modules);
    collect_lib_dirs(&dirs, &m->public_usage);
    collect_lib_dirs(&dirs, &m->private_usage);
    for (size_t i = 0; i < modules.count; ++i) {
      const Module *dep = find_module(g, modules.items[i]);
      collect_lib_dirs(&dirs, &dep->public_usage);
      if (link_private(dep))
        collect_lib_dirs(&dirs, &dep->private_usage);
    }
    render_link_args(templ, m, pdb, &dirs, &args);
    add_items(&args, &inputs);
    library_flags(g, m, &m->public_usage, &dirs, &args, &inputs);
    library_flags(g, m, &m->private_usage, &dirs, &args, &inputs);
    for (size_t i = 0; i < modules.count; ++i) {
      const Module *dep = find_module(g, modules.items[i]);
      const char *file = dep->import_lib ? dep->import_lib : dep->artifact;
      if (file) {
        unique(&inputs, file);
        push(&args, file);
      }
      library_flags(g, m, &dep->public_usage, &dirs, &args, &inputs);
      if (link_private(dep))
        library_flags(g, m, &dep->private_usage, &dirs, &args, &inputs);
    }
  }
  if (m->import_lib)
    push(&implicit, m->import_lib);
  NinjaEdge *edge = new_edge(edges, rule, m);
  edge->outputs = vstr_from_cstr(ninja_path(m->artifact));
  edge->implicit_outputs = edge_files(&implicit);
  edge->inputs = edge_files(&inputs);
  response(edge, &program, &args, m->artifact, t, NULL);
  mkdirs(parent(m->artifact));
  if (m->import_lib)
    mkdirs(parent(m->import_lib));
}

static void task_edges(NinjaEdges *edges, const Graph *g) {
  for (size_t i = 0; i < g->task_count; ++i) {
    const Task *task = g->tasks[i];
    if (!task->selected)
      continue;
    Strings args = {0}, inputs = {0};
    add_items(&inputs, &task->inputs);
    if (task->tool) {
      const Module *tool = find_module(g, task->tool);
      push(&args, tool->artifact);
      unique(&inputs, tool->artifact);
      add_items(&args, &task->args);
    } else if (task->program) {
      push(&args, task->program);
      unique(&inputs, task->program);
      add_items(&args, &task->args);
    } else {
      const char *self = executable_path();
      push(&args, self);
      const bool copy_task = same(task->engine, "copy");
      push(&args, copy_task ? "_copy" : "_render");
      push(&args, copy_task ? task->inputs.items[0] : task->template);
      push(&args, task->outputs.items[0]);
      if (task->data)
        push(&args, task->data);
      unique(&inputs, self);
    }
    for (size_t j = 0; j < task->outputs.count; ++j)
      mkdirs(parent(task->outputs.items[j]));
    invoke(edges, task->owner, &task->outputs, &inputs, &args);
  }
}

static void runtime_closure(const Graph *g, const Module *m, Strings *visited,
                            Strings *runtime) {
  if (contains(visited, m->name))
    return;
  push(visited, m->name);
  if (m->artifact && m->kind == MODULE_SHARED)
    unique(runtime, m->artifact);
  for (size_t i = 0; i < m->runtime_bins.count; ++i)
    unique(runtime, m->runtime_bins.items[i]);
  const Strings *edges[] = {&m->public_deps, &m->private_deps,
                            &m->runtime_deps};
  for (size_t j = 0; j < 3; ++j)
    for (size_t i = 0; i < edges[j]->count; ++i)
      runtime_closure(g, find_module(g, edges[j]->items[i]), visited, runtime);
}

const char *module_alias(const Module *module) {
  return format_string("module/{}", module->name);
}

static void emit_aliases(NinjaEdges *edges, const Graph *g) {
  const Project *p = g->project;
  Strings copied = {0}, origins = {0}, defaults = {0};
  for (size_t i = 0; i < g->count; ++i) {
    const Module *m = g->modules[i];
    if (!m->selected)
      continue;
    Strings outputs = {0};
    if (m->artifact)
      push(&outputs, m->artifact);
    if (m->kind == MODULE_HEADER_ONLY && m->test_name)
      for (size_t j = 0; j < m->runtime_deps.count; ++j)
        unique(&outputs, module_alias(find_module(g, m->runtime_deps.items[j])));
    for (size_t j = 0; j < g->task_count; ++j) {
      const Task *task = g->tasks[j];
      if (task->owner == m && task->selected)
        for (size_t k = 0; k < task->outputs.count; ++k)
          unique(&outputs, task->outputs.items[k]);
    }
    Strings runtime = {0}, visited = {0};
    if (m->kind == MODULE_EXECUTABLE || m->kind == MODULE_SHARED)
      runtime_closure(g, m, &visited, &runtime);
    for (size_t j = 0; j < runtime.count; ++j) {
      const char *source = runtime.items[j];
      if (same(source, m->artifact))
        continue;
      const char *dir =
          starts(parent(source), p->build_dir) && !same(m->tools->os, "windows")
              ? parent(source)
              : parent(m->artifact);
      if (!inside(p->build_dir, source) && !same(m->tools->os, "windows"))
        dir = path(p->build_dir, "lib");
      const char *destination = path(dir, filename(source));
      if (!same(destination, source)) {
        if (producer(g, destination))
          failf("runtime staging conflicts with generated output '{}'",
                destination);
        for (size_t k = 0; k < g->count; ++k)
          if (same(g->modules[k]->artifact, destination) ||
              same(g->modules[k]->import_lib, destination))
            failf("runtime staging conflicts with build output '{}'",
                  destination);
        size_t k = 0;
        for (; k < copied.count; ++k)
          if (same(copied.items[k], destination))
            break;
        if (k < copied.count && !same(origins.items[k], source))
          failf("runtime staging collision at '{}'", destination);
        if (k == copied.count) {
          push(&copied, destination);
          push(&origins, source);
          Strings args = {0}, one_out = {0}, one_in = {0};
          push(&args, executable_path());
          push(&args, "_copy");
          push(&args, source);
          push(&args, destination);
          push(&one_out, destination);
          push(&one_in, source);
          invoke(edges, NULL, &one_out, &one_in, &args);
        }
      }
      unique(&outputs, destination);
    }
    const char *alias = module_alias(m);
    NinjaEdge *edge = new_edge(edges, RULE_PHONY, NULL);
    edge->outputs = vstr_from_cstr(ninja_path(alias));
    edge->inputs = edge_files(&outputs);
    if (p->request->targets.count ? contains(&p->request->targets, m->name)
                                  : m->tools != &p->host)
      push(&defaults, alias);
  }
  NinjaEdge *all = new_edge(edges, RULE_PHONY, NULL);
  all->outputs = vstr_from_cstr("all");
  all->inputs = edge_files(&defaults);
}

static BuildTemplate read_template(const Project *p, TemplateId id) {
  return read_build_template(p, template_names[id]);
}

static void install_edges(NinjaEdges *edges, const Graph *g, Strings *metadata,
                          const BuildTemplate *templates) {
  Strings inputs[INSTALL_COMPONENT_COUNT] = {0};
  write_install_plan(g, inputs, metadata, &templates[SDK_JSON], &templates[SDK_PC]);
  for (size_t i = 0; i < g->count; ++i) {
    const Module *m = g->modules[i];
    if (!m->selected || m->tools != &g->project->target || m->test_only)
      continue;
    const char *format = symbol_format(m);
    if (!format)
      continue;
    const char *output = symbol_output(g, m);
    Strings outputs = {0};
    if (same(format, "macho")) {
      push(&outputs, dsym_dwarf(output, m->artifact));
      push(&outputs, path(output, "Contents/Info.plist"));
    } else {
      push(&outputs, output);
    }
    for (size_t j = 0; j < outputs.count; ++j)
      if (producer(g, outputs.items[j]))
        failf("{}: debug symbols conflict with '{}'", m->name, outputs.items[j]);
    const char *source[] = {m->artifact};
    const char *args[] = {executable_path(), "_symbols", format, m->artifact,
                          output};
    Strings prerequisites = {.items = source, .count = 1};
    Strings tool_args = {.items = args, .count = sizeof(args) / sizeof(*args)};
    invoke(edges, NULL, &outputs, &prerequisites, &tool_args);
  }
  Strings prerequisites = {0};
  push(&prerequisites, INSTALL_PLAN);
  for (unsigned i = 0; i < INSTALL_COMPONENT_COUNT; ++i) {
    const char *target = install_target((InstallComponent)i);
    NinjaEdge *alias = new_edge(edges, RULE_PHONY, NULL);
    alias->outputs = vstr_from_cstr(target);
    alias->inputs = edge_files(&inputs[i]);
    push(&prerequisites, target);
  }
  NinjaEdge *always = new_edge(edges, RULE_PHONY, NULL);
  always->outputs = vstr_from_cstr("install/always");
  push(&prerequisites, "install/always");
  const char *output[] = {"_install.action"};
  const char *args[] = {executable_path(), "_install", INSTALL_PLAN,
                       install_prefix(g->project, NULL)};
  Strings outputs = {.items = output, .count = 1};
  Strings command_args = {.items = args, .count = 4};
  invoke(edges, NULL, &outputs, &prerequisites, &command_args);
  NinjaEdge *install = new_edge(edges, RULE_PHONY, NULL);
  install->outputs = vstr_from_cstr("install");
  install->inputs = vstr_from_cstr("_install.action");
}

static void load_templates(const Graph *g, BuildTemplate *templates) {
  templates[NINJA_MANIFEST] = read_template(g->project, NINJA_MANIFEST);
  templates[NINJA_RULES] = read_template(g->project, NINJA_RULES);
  templates[ARTIFACT_NAMES] = *g->artifact_template;
  for (size_t i = 0; i < g->count; ++i) {
    const Module *m = g->modules[i];
    if (m->selected && m->export_target && !templates[SDK_JSON].file) {
      templates[SDK_JSON] = read_template(g->project, SDK_JSON);
      templates[SDK_PC] = read_template(g->project, SDK_PC);
    }
    if (!m->selected || !m->artifact)
      continue;
    const TemplateId compiler = compile_template(m->tools);
    if (!templates[compiler].file)
      templates[compiler] = read_template(g->project, compiler);
    if (m->cuda_compiler && !templates[COMPILE_CUDA].file)
      templates[COMPILE_CUDA] = read_template(g->project, COMPILE_CUDA);
    const TemplateId id = link_template(m);
    if (!templates[id].file)
      templates[id] = read_template(g->project, id);
  }
}

static void write_stamp(const Project *p, const BuildTemplate *templates) {
  Text values = {0};
  for (size_t i = 0; i < p->used_environment.count; ++i) {
    Setting item = p->used_environment.items[i];
    const char *value = string(item.value, item.name);
    append_format(&values, "{}:{}:{}\n", item.name, strlen(value), value);
  }
  for (size_t i = 0; i < TEMPLATE_COUNT; ++i) {
    const BuildTemplate *templ = &templates[i];
    if (!templ->file)
      continue;
    append_format(&values, "{}:{}:", templ->file, templ->source.len);
    append_n(&values, templ->source.data, templ->source.len);
  }
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < values.length; ++i) {
    hash ^= (unsigned char)values.data[i];
    hash *= UINT64_C(1099511628211);
  }
  const char *stamp = format_string("{:016x}\n", hash);
  write_file(path(p->build_dir, "config-env.stamp"), stamp, strlen(stamp));
}

static void check_identity(const Project *p, bool publish);

void write_environment_stamp(const Graph *g) {
  check_identity(g->project, false);
  BuildTemplate templates[TEMPLATE_COUNT] = {0};
  load_templates(g, templates);
  write_stamp(g->project, templates);
}

static void regeneration(NinjaEdges *edges, const Project *p,
                          const Strings *metadata) {
  Strings args = {0};
  push(&args, executable_path());
  push(&args, "gen");
  push(&args, "--root");
  push(&args, p->root);
  push(&args, "--preset");
  push(&args, p->name);
  push(&args, "--platform");
  push(&args, p->target.platform);
  push(&args, "--build-dir");
  push(&args, p->build_dir);
  push(&args, "--locked-target");
  push(&args, p->target.id);
  push(&args, "--locked-host");
  push(&args, p->host.id);
  if (p->request->tests)
    push(&args, "--tests");
  for (size_t i = 0; i < p->request->targets.count; ++i) {
    push(&args, "--target");
    push(&args, p->request->targets.items[i]);
  }
  Strings inputs = {0};
  push(&inputs, "config-env.stamp");
  add_items(&inputs, &p->inputs);
  push(&inputs, args.items[0]);
  NinjaEdge *regen = new_edge(edges, RULE_REGENERATE, NULL);
  regen->outputs = vstr_from_cstr("build.ninja");
  const char *generated[] = {"compile_commands.json", "toolchains.json",
                            INSTALL_PLAN};
  Strings outputs = {0};
  for (size_t i = 0; i < sizeof(generated) / sizeof(*generated); ++i)
    push(&outputs, generated[i]);
  add_items(&outputs, metadata);
  regen->implicit_outputs = edge_files(&outputs);
  regen->inputs = edge_files(&inputs);
  regen->run = edge_value(command(&args));
  args.items[1] = "_env";
  NinjaEdge *always = new_edge(edges, RULE_PHONY, NULL);
  always->outputs = vstr_from_cstr("always_check_environment");
  NinjaEdge *check = new_edge(edges, RULE_TASK, NULL);
  check->outputs = vstr_from_cstr("config-env.stamp");
  check->inputs = vstr_from_cstr("always_check_environment");
  check->run = edge_value(command(&args));
}

static void check_identity(const Project *p, bool publish) {
  const char *file = path(p->build_dir, "toolchains.json");
  Json *identity = own(json_create_object());
  const Toolchain *roles[] = {&p->host, &p->target};
  const char *names[] = {"host", "target"};
  for (size_t i = 0; i < 2; ++i) {
    const Toolchain *t = roles[i];
    Json *role = json_create_object();
    json_set(identity, names[i], role);
    json_set(role, "id", json_create_string(t->id));
    json_set(role, "platform", json_create_string(t->platform));
    const char *tools[] = {t->cc, t->cxx, t->ar, t->ld};
    const char *keys[] = {"cc", "cxx", "ar", "ld"};
    for (size_t j = 0; j < 4; ++j)
      json_set(role, keys[j],
               json_create_string(
                   format_string("{}@{}", tools[j], path_stamp(tools[j]))));
  }
  if (is_file(file)) {
    const Json *previous = load_json(file);
    for (size_t i = 0; i < 2; ++i) {
      const Json *before = member(previous, names[i]),
                 *after = member(identity, names[i]);
      for (size_t j = 0; j < json_object_size(after); ++j) {
        const char *key = json_object_key(after, j);
        if (!same(json_get_string(before, key), json_get_string(after, key)))
          failf("{}: {} toolchain identity changed; use a fresh --build-dir",
                p->build_dir, names[i]);
      }
    }
  } else if (publish) {
    write_json(file, identity);
  }
}

static JINJA_CMETA_ENV *compile_templates(BuildTemplate *templates) {
  JINJA_CMETA_ENV *env = create_template_env();
  for (size_t i = 0; i < TEMPLATE_COUNT; ++i) {
    BuildTemplate *templ = &templates[i];
    if (i == NINJA_RULES || i == ARTIFACT_NAMES || !templ->file)
      continue;
    compile_build_template(env, templ);
  }
  return env;
}

static void render_argument_array(const BuildTemplate *templ,
                                  const cmeta_data_desc *descriptor,
                                  const void *data,
                                  const JINJA_CMETA_RENDER_OPTIONS *limits,
                                  const char *label, Strings *args) {
  Text rendered = render_template(templ, descriptor, data, limits);
  Json *argv = json_parse(contents(&rendered), rendered.length);
  const char *where = format_string("{} ({})", templ->file, label);
  if (!argv)
    failf("{}: expected a JSON argument array: {}", where, json_get_error());
  array(argv, where);
  for (size_t i = 0; i < json_array_size(argv); ++i)
    push(args, copy(string(json_array_get(argv, i), where)));
  json_free(argv);
}

static void render_compile_args(const BuildTemplate *templ, const Module *m,
                                const CompileOptions *options, Strings *args) {
  static const cmeta_type_identity policy_identity =
      CMETA_TYPE_ID_ATOM_INIT("slate.compile.policy");
  static const cmeta_type_desc policy_storage = {.name = "CompilerPolicy",
                                                 .size = sizeof(CompilerPolicy),
                                                 .align =
                                                     _Alignof(CompilerPolicy),
                                                 .kind = CMETA_T_OBJECT,
                                                 .identity = &policy_identity};
  static const cmeta_type_identity identity =
      CMETA_TYPE_ID_ATOM_INIT("slate.compile.options");
  static const cmeta_type_desc storage = {.name = "CompileOptions",
                                          .size = sizeof(CompileOptions),
                                          .align = _Alignof(CompileOptions),
                                          .kind = CMETA_T_OBJECT,
                                          .identity = &identity};
  const cmeta_data_desc *str = jinja_cmeta_vstr_data();
#define POLICY_FIELD(field, descriptor)                                        \
  {                                                                            \
    "slate.compile.policy." #field, #field,                              \
        offsetof(CompilerPolicy, field), descriptor                            \
  }
  const cmeta_data_field_desc policy_fields[] = {
      POLICY_FIELD(optimization, str),
      POLICY_FIELD(debug_info, str),
      POLICY_FIELD(warnings, str),
      POLICY_FIELD(runtime_lib, str),
      POLICY_FIELD(rtti, str),
      POLICY_FIELD(exceptions, str),
      POLICY_FIELD(lto, str),
      POLICY_FIELD(sanitizers, str),
      POLICY_FIELD(symbol_visibility, str),
      POLICY_FIELD(visibility_inlines_hidden, &cmeta_data_bool),
      POLICY_FIELD(debug_crt, &cmeta_data_bool),
      POLICY_FIELD(werror, &cmeta_data_bool)};
#undef POLICY_FIELD
  const cmeta_data_struct_shape policy_shape = {
      StructMeta(CompilerPolicy), policy_fields,
      sizeof(policy_fields) / sizeof(*policy_fields)};
  const cmeta_data_desc policy = {.struct_size = sizeof(policy),
                                  .abi_version = CMETA_DATA_DESC_ABI_VERSION,
                                  .stable_id =
                                      "slate.compile.policy.data",
                                  .display_name = "CompilerPolicy",
                                  .kind = CMETA_DATA_STRUCT,
                                  .storage_type = &policy_storage,
                                  .shape = &policy_shape};
#define COMPILE_FIELD(field, descriptor)                                       \
  {                                                                            \
    "slate.compile.options." #field, #field,                             \
        offsetof(CompileOptions, field), descriptor                            \
  }
  const cmeta_data_field_desc fields[] = {
      COMPILE_FIELD(policy, &policy),
      COMPILE_FIELD(standard, str),
      COMPILE_FIELD(triple, str),
      COMPILE_FIELD(darwin_arch, str),
      COMPILE_FIELD(sysroot, str),
      COMPILE_FIELD(source, str),
      COMPILE_FIELD(output, str),
      COMPILE_FIELD(depfile, str),
      COMPILE_FIELD(cxx, &cmeta_data_bool),
      COMPILE_FIELD(objc, &cmeta_data_bool),
      COMPILE_FIELD(pic, &cmeta_data_bool),
      COMPILE_FIELD(includes, &cmeta_data_sequence_view),
      COMPILE_FIELD(defines, &cmeta_data_sequence_view),
      COMPILE_FIELD(flags, &cmeta_data_sequence_view)};
#undef COMPILE_FIELD
  const cmeta_data_struct_shape shape = {StructMeta(CompileOptions), fields,
                                         sizeof(fields) / sizeof(*fields)};
  const cmeta_data_desc descriptor = {
      .struct_size = sizeof(descriptor),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "slate.compile.options.data",
      .display_name = "CompileOptions",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &storage,
      .shape = &shape};
  const size_t items =
      options->includes.count + options->defines.count + options->flags.count;
  const size_t nodes_per_item = 8;
  if (items > (SIZE_MAX - JINJA_CMETA_DEFAULT_MAX_NODES) / nodes_per_item)
    failf("{}: too many compiler options to render", m->name);
  JINJA_CMETA_RENDER_OPTIONS limits = JINJA_CMETA_RENDER_OPTIONS_INIT;
  limits.max_nodes = JINJA_CMETA_DEFAULT_MAX_NODES + items * nodes_per_item;
  render_argument_array(templ, &descriptor, options, &limits, m->name, args);
}

Struct(LinkOptions, (vstr, os), (vstr, machine), (vstr, triple),
       (vstr, darwin_arch), (vstr, sysroot), (vstr, output),
       (vstr, output_name), (vstr, import_lib), (vstr, pdb), (vstr, lto),
       (vstr, sanitizers), (bool, shared),
       (cmeta_data_collection_view, lib_dirs));

static void render_link_args(const BuildTemplate *templ, const Module *m,
                             const char *pdb, const Strings *lib_dirs,
                             Strings *args) {
  static const cmeta_type_identity identity =
      CMETA_TYPE_ID_ATOM_INIT("slate.link.options");
  static const cmeta_type_desc storage = {.name = "LinkOptions",
                                          .size = sizeof(LinkOptions),
                                          .align = _Alignof(LinkOptions),
                                          .kind = CMETA_T_OBJECT,
                                          .identity = &identity};
  const cmeta_data_desc *str = jinja_cmeta_vstr_data();
#define LINK_FIELD(field)                                                      \
  {                                                                            \
    "slate.link.options." #field, #field, offsetof(LinkOptions, field),  \
        str                                                                    \
  }
  const cmeta_data_field_desc fields[] = {
      LINK_FIELD(os),
      LINK_FIELD(machine),
      LINK_FIELD(triple),
      LINK_FIELD(darwin_arch),
      LINK_FIELD(sysroot),
      LINK_FIELD(output),
      LINK_FIELD(output_name),
      LINK_FIELD(import_lib),
      LINK_FIELD(pdb),
      LINK_FIELD(lto),
      LINK_FIELD(sanitizers),
      {"slate.link.options.lib_dirs", "lib_dirs",
       offsetof(LinkOptions, lib_dirs), &cmeta_data_sequence_view},
      {"slate.link.options.shared", "shared",
       offsetof(LinkOptions, shared), &cmeta_data_bool}};
#undef LINK_FIELD
  const cmeta_data_struct_shape shape = {StructMeta(LinkOptions), fields,
                                         sizeof(fields) / sizeof(*fields)};
  const cmeta_data_desc descriptor = {
      .struct_size = sizeof(descriptor),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "slate.link.options.data",
      .display_name = "LinkOptions",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &storage,
      .shape = &shape};
  const Toolchain *t = m->tools;
  const LinkOptions data = {
      .os = vstr_from_cstr(t->os),
      .machine = vstr_from_cstr(t->arch),
      .triple = vstr_from_cstr(compiler_target(t)),
      .darwin_arch = vstr_from_cstr(darwin_arch(t)),
      .sysroot = vstr_from_cstr(t->sysroot),
      .output = vstr_from_cstr(m->artifact),
      .output_name = vstr_from_cstr(filename(m->artifact)),
      .import_lib = vstr_from_cstr(m->import_lib),
      .pdb = vstr_from_cstr(pdb),
      .lto = vstr_from_cstr(opt(m, "lto", "none")),
      .sanitizers = vstr_from_cstr(opt(m, "sanitizers", "")),
      .shared = m->kind == MODULE_SHARED,
      .lib_dirs = string_view(lib_dirs)};
  render_argument_array(templ, &descriptor, &data, NULL, m->name, args);
}

static Text render_manifest(const BuildTemplate *templ, const NinjaEdges *edges,
                            int depth) {
  static const cmeta_type_identity identity =
      CMETA_TYPE_ID_ATOM_INIT("slate.ninja.edge");
  static const cmeta_type_desc storage = {.name = "NinjaEdge",
                                          .size = sizeof(NinjaEdge),
                                          .align = _Alignof(NinjaEdge),
                                          .kind = CMETA_T_OBJECT,
                                          .identity = &identity};
  const cmeta_data_desc *string_data = jinja_cmeta_vstr_data();
#define NINJA_EDGE_FIELD(field)                                                \
  {                                                                            \
    "slate.ninja.edge." #field, #field, offsetof(NinjaEdge, field),      \
        string_data                                                            \
  }
  const cmeta_data_field_desc fields[] = {
      NINJA_EDGE_FIELD(rule),
      NINJA_EDGE_FIELD(outputs),
      NINJA_EDGE_FIELD(implicit_outputs),
      NINJA_EDGE_FIELD(inputs),
      NINJA_EDGE_FIELD(implicit_inputs),
      NINJA_EDGE_FIELD(order_only),
      NINJA_EDGE_FIELD(run),
      NINJA_EDGE_FIELD(response),
      NINJA_EDGE_FIELD(argv),
      NINJA_EDGE_FIELD(dep),
  };
#undef NINJA_EDGE_FIELD
  const cmeta_data_struct_shape shape = {StructMeta(NinjaEdge), fields,
                                         sizeof(fields) / sizeof(*fields)};
  const cmeta_data_desc edge = {.struct_size = sizeof(edge),
                                .abi_version = CMETA_DATA_DESC_ABI_VERSION,
                                .stable_id = "slate.ninja.edge.data",
                                .display_name = "NinjaEdge",
                                .kind = CMETA_DATA_STRUCT,
                                .storage_type = &storage,
                                .shape = &shape};
  static const cmeta_type_identity rule_identity =
      CMETA_TYPE_ID_ATOM_INIT("slate.ninja.rule");
  static const cmeta_type_desc rule_storage = {.name = "NinjaRule",
                                               .size = sizeof(NinjaRule),
                                               .align = _Alignof(NinjaRule),
                                               .kind = CMETA_T_OBJECT,
                                               .identity = &rule_identity};
  const cmeta_data_field_desc rule_fields[] = {
      {"slate.ninja.rule.name", "name", offsetof(NinjaRule, name),
       string_data},
      {"slate.ninja.rule.kind", "kind", offsetof(NinjaRule, kind),
       string_data}};
  const cmeta_data_struct_shape rule_shape = {
      StructMeta(NinjaRule), rule_fields,
      sizeof(rule_fields) / sizeof(*rule_fields)};
  const cmeta_data_desc rule = {.struct_size = sizeof(rule),
                                .abi_version = CMETA_DATA_DESC_ABI_VERSION,
                                .stable_id = "slate.ninja.rule.data",
                                .display_name = "NinjaRule",
                                .kind = CMETA_DATA_STRUCT,
                                .storage_type = &rule_storage,
                                .shape = &rule_shape};
  const NinjaTemplateData data = {
      .link_pool_depth = depth,
      .compile_pool_depth = edges->compile_depth,
      .edges = {edges->items, edges->count, sizeof(*edges->items), &edge},
      .rules = {edges->rules, edges->rule_count, sizeof(*edges->rules), &rule}};
  JINJA_CMETA_RENDER_OPTIONS limits = JINJA_CMETA_RENDER_OPTIONS_INIT;
  /* A reflected edge and its field reads stay in the Jinja node workspace. */
  const size_t nodes_per_edge = 1 + 2 * shape.field_count;
  if (edges->count >
      (SIZE_MAX - JINJA_CMETA_DEFAULT_MAX_NODES) / nodes_per_edge)
    fail("too many build edges to render");
  limits.max_nodes =
      JINJA_CMETA_DEFAULT_MAX_NODES + edges->count * nodes_per_edge;
  const size_t nodes_per_rule = 64;
  if (edges->rule_count > (SIZE_MAX - limits.max_nodes) / nodes_per_rule)
    fail("too many Ninja rules to render");
  limits.max_nodes += edges->rule_count * nodes_per_rule;
  return render_template(templ, cmeta_reflected_data(NinjaTemplateData), &data,
                         &limits);
}

void emit_ninja(Graph *g) {
  const Project *p = g->project;
  NinjaEdges edges = {0};
  const Json *compile_depth = get(&p->fields, "compilePoolDepth");
  if (compile_depth) {
    double value = json_type(compile_depth) == JSON_NUMBER
                       ? json_number(compile_depth) : 0;
    if (!(value >= 1 && value <= 1024) || value != (int)value)
      fail("invalid compilePoolDepth");
    edges.compile_depth = (int)value;
  }
  Json *database = own(json_create_array());
  const Json *setting = get(&p->fields, "linkPoolDepth");
  double depth =
      setting && json_type(setting) == JSON_NUMBER ? json_number(setting) : 4;
  if (depth < 1 || depth > 1024 || depth != (int)depth)
    fail("invalid linkPoolDepth");
  BuildTemplate templates[TEMPLATE_COUNT] = {0};
  load_templates(g, templates);
  JINJA_CMETA_ENV *env = compile_templates(templates);
  check_identity(p, true);
  init_rules(&edges, &templates[NINJA_RULES]);
  task_edges(&edges, g);
  for (size_t i = 0; i < g->count; ++i) {
    Module *m = g->modules[i];
    if (m->selected && m->artifact) {
      compile_module(&edges, database, g, m, templates);
      link_module(&edges, g, m, templates);
    }
  }
  emit_aliases(&edges, g);
  Strings metadata = {0};
  install_edges(&edges, g, &metadata, templates);
  regeneration(&edges, p, &metadata);
  Text manifest =
      render_manifest(&templates[NINJA_MANIFEST], &edges, (int)depth);
  for (size_t i = 0; i < TEMPLATE_COUNT; ++i)
    if (templates[i].compiled)
      jinja_cmeta_release(templates[i].compiled);
  jinja_cmeta_env_destroy(env);
  write_stamp(p, templates);
  write_json(path(p->build_dir, "compile_commands.json"), database);
  write_file(path(p->build_dir, "build.ninja"), contents(&manifest),
             manifest.length);
}
