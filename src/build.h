#ifndef SLATE_H
#define SLATE_H

#include <core/fmt.h>
#include <core/process.h>
#include <json_parser.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef json_value_t Json;

typedef struct {
  const char **items;
  size_t count;
  size_t capacity;
} Strings;

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} Text;

typedef struct {
  const char *name;
  const Json *value;
  const char *origin;
} Setting;

typedef struct {
  Setting *items;
  size_t count;
  size_t capacity;
} Settings;

typedef struct {
  const char *command;
  const char *root;
  const char *preset;
  const char *platform;
  const char *driver;
  const char *explain;
  const char *locked_target;
  const char *locked_host;
  const char *build_dir;
  const char *ninja_tool;
  const char *install_prefix;
  const char *test_filter;
  const char *test_label;
  unsigned install_components;
  Strings targets;
  bool tests;
  bool list_tests;
  bool graph_json;
} Request;

typedef struct {
  const char *executable_suffix;
  const char *static_prefix;
  const char *static_suffix;
  const char *shared_prefix;
  const char *shared_suffix;
  const char *import_prefix;
  const char *import_suffix;
  const char *object_suffix;
  const char *response_suffix;
  const char *depfile_suffix;
  const char *pdb_suffix;
  const char *symbols_suffix;
} ArtifactNames;

typedef struct {
  const char *id;
  const char *driver;
  const char *platform;
  const char *os;
  const char *arch;
  const char *cc;
  const char *cxx;
  const char *ar;
  const char *ld;
  const char *launcher;
  const char *triplet;
  const char *vcpkg_root;
  const char *triple;
  const char *sysroot;
  const Json *runtime_bins;
  ArtifactNames artifacts;
  bool msvc;
} Toolchain;

typedef struct {
  const Request *request;
  const char *root;
  const char *name;
  const char *build_dir;
  const char *config;
  const char *dot_env;
  Json *manifest;
  const Json *styles;
  Settings fields;
  Settings definitions;
  Settings options;
  Settings environment;
  Settings used_environment;
  Strings env_keys;
  Strings preset_styles;
  Strings inputs;
  Toolchain target;
  Toolchain host;
  bool native;
  bool tests;
  bool verbose;
} Project;

typedef enum {
  INSTALL_RUNTIME,
  INSTALL_DEV,
  INSTALL_SYMBOLS,
  INSTALL_COMPONENT_COUNT
} InstallComponent;

#define INSTALL_ALL ((1u << INSTALL_COMPONENT_COUNT) - 1u)
#define INSTALL_PLAN "install-plan.json"

typedef struct {
  const char *from;
  const char *to;
  InstallComponent component;
} InstallMapping;

typedef enum {
  MODULE_EXECUTABLE,
  MODULE_STATIC,
  MODULE_SHARED,
  MODULE_HEADER_ONLY,
  MODULE_PREBUILT
} ModuleKind;

typedef struct {
  Strings includes;
  Strings lib_dirs;
  Strings libs;
  Strings link_flags;
  Strings flags;
  Settings defines;
} Usage;

typedef struct Module Module;
typedef struct Task Task;
struct Task {
  const char *name;
  const char *engine;
  const char *template;
  const char *data;
  const char *tool;
  const char *program;
  const Json *input_paths;
  Strings args;
  Strings inputs;
  Strings outputs;
  Module *owner;
  unsigned state;
  bool selected;
};

struct Module {
  const char *name;
  const char *directory;
  const char *manifest;
  const char *artifact;
  const char *import_lib;
  const char *test_name;
  const char *test_program;
  const char *test_directory;
  const Json *data;
  const Json *package;
  const Toolchain *tools;
  ModuleKind kind;
  Strings sources;
  Strings objc_sources;
  Strings declared_public_includes;
  Strings public_deps;
  Strings private_deps;
  Strings runtime_deps;
  Strings runtime_bins;
  Strings debug_symbols;
  Strings objects;
  Strings test_args;
  Strings test_environment;
  Strings test_labels;
  InstallMapping *install;
  size_t install_count;
  size_t install_capacity;
  Usage public_usage;
  Usage private_usage;
  Usage compile_usage;
  Settings options;
  Settings ninja_rules;
  unsigned state;
  unsigned test_timeout;
  bool selected;
  bool cxx;
  bool test_only;
  bool export_target;
  const char *export_version;
};

typedef struct BuildTemplate BuildTemplate;

typedef struct {
  Project *project;
  BuildTemplate *artifact_template;
  Module **modules;
  size_t count;
  size_t capacity;
  Task **tasks;
  size_t task_count;
  size_t task_capacity;
} Graph;

_Noreturn void fail(const char *message);
_Noreturn void fail_formatted(tstr message);
#define failf(...) fail_formatted(tstr_format(__VA_ARGS__))
void print_formatted_line(tstr line);
#define print_line(...) print_formatted_line(tstr_format(__VA_ARGS__))
void init_console_output(void);
const char *console_mark(const char *text, const char *color);
void console_start(const char *action, const Request *request);
int console_finish(const char *action, int status);
void *take(size_t size);
void *reserve(void *previous, size_t count, size_t *capacity,
              size_t element_size);
char *copy(const char *str);
char *slice(const char *str, size_t length);
char *copy_formatted(tstr value);
#define format_string(...) copy_formatted(tstr_format(__VA_ARGS__))
bool same(const char *a, const char *b);
bool starts(const char *str, const char *prefix);
bool ends(const char *str, const char *suffix);
bool filename_matches(const char *pattern, const char *name);
bool identifier(const char *str);
void push(Strings *list, const char *str);
bool contains(const Strings *list, const char *str);
void unique(Strings *list, const char *str);
void sort_strings(Strings *list);
void append(Text *text, const char *str);
void append_n(Text *text, const char *str, size_t length);
void append_formatted(Text *text, tstr value);
#define append_format(text, ...)                                               \
  append_formatted((text), tstr_format(__VA_ARGS__))
const char *contents(const Text *text);
void put(Settings *map, const char *key, const Json *value, const char *origin);
const Setting *setting(const Settings *map, const char *key);
const Json *get(const Settings *map, const char *key);
void overlay(Settings *map, const Json *object, const char *origin);
Json *own(Json *value);
Json *load_json(const char *path);
const Json *member(const Json *json, const char *key);
const char *string(const Json *value, const char *where);
const char *optional_string(const Json *value, const char *where);
void object(const Json *value, const char *where);
void array(const Json *value, const char *where);
void check_fields(const Json *value, const char *where, const char *fields);
void read_strings(Strings *out, const Json *value, const char *where);
void json_set(Json *object, const char *key, Json *value);
void json_add(Json *array, Json *value);

const char *cwd(void);
void utf8_arguments(int *count, char ***args);
const char *native_tool_text(const char *value);
const char *environment_variable(const char *name);
#ifdef _WIN32
void initialize_vsdevcmd(const char *root, const char *script,
                         const char *arch);
int write_vsdevcmd_environment(void);
#endif
const char *executable_path(void);
const char *path(const char *base, const char *relative);
const char *parent(const char *path);
const char *filename(const char *path);
bool is_file(const char *path);
bool is_dir(const char *path);
bool is_link(const char *path);
bool inside(const char *directory, const char *file);
void list_dir(const char *directory, Strings *entries);
const char *find_program(const char *root, const char *name);
const char *probe_program(const char *program, const char *const *args,
                          const char *directory, bool allow_nonzero,
                          const char **reported);
const char *capture_program(const char *program, const char *const *args,
                            const char *directory, const char *const *env,
                            bool allow_nonzero, const char **out,
                            const char **err);
const char *read_file(const char *path, size_t *length);
void mkdirs(const char *path);
void write_file(const char *path, const char *data, size_t length);
int remove_file(const char *path);
void write_json(const char *path, const Json *json);
void copy_file(const char *source, const char *destination);
void copy_install_file(const char *source, const char *destination);
uint64_t path_stamp(const char *path);
const char *host_os(void);
const char *host_arch(void);
const char *argument(const char *str);
const char *command(const Strings *args);
const char *ninja_path(const char *str);
const char *ninja_value(const char *str);

Project load_project(const Request *request);
const char *find_project_root(const Request *request);
const char *compiler_target(const Toolchain *tools);
const char *darwin_arch(const Toolchain *tools);
const char *expand_env(Project *project, const char *text, const char *where,
                       const char **problem, bool track);
void apply_style(const Project *project, const char *name, Settings *options,
                 Usage *usage, Strings *stack);
void print_project(const Project *project);
Graph load_graph(Project *project);
const char *vcpkg_tree(Project *project, const Module *module, bool config);
void resolve_package(Project *project, Module *module);
const char *package_abi(const Toolchain *tools);
Module *find_module(const Graph *graph, const char *name);
Task *producer(const Graph *graph, const char *output);
void link_modules(const Graph *graph, const Module *module, Strings *order);
void print_graph(const Graph *graph);
void print_graph_json(const Graph *graph);
bool module_runnable(const Project *project, const Module *module);
void explain_module(const Graph *graph, const char *name);
void emit_ninja(Graph *graph);
const char *module_alias(const Module *module);
int command_termination_status(void);
int wait_child(process_t *child, process_result_t *result);
int run_child(const char *role, const char *program, const char *const *args,
              const char *directory);
int test_project(Graph *graph, const char *ninja);
void write_environment_stamp(const Graph *graph);
const char *module_pdb(const Module *module);
const char *symbol_format(const Module *module);
const char *symbol_output(const Graph *graph, const Module *module);
const char *dsym_dwarf(const char *bundle, const char *artifact);
const char *install_target(InstallComponent component);
unsigned parse_install_components(const char *names);
const char *install_prefix(const Project *project, const char *override);
void add_install_mapping(Module *module, InstallComponent component,
                         const char *from, const char *to, const char *where);
void write_install_plan(const Graph *graph, Strings *inputs, Strings *metadata,
                        const BuildTemplate *sdk_json,
                        const BuildTemplate *sdk_pc);
const char *artifact_install_dir(const Graph *graph, const char *artifact);
void run_install(const char *plan, const char *prefix, unsigned components);

#endif
