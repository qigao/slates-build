#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "build.h"

#include <cmd_arger.h>
#include <dotenv.h>
#include <mustache_json.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#endif

static const char app_version[] = SLATE_NAME " " SLATE_VERSION;

#ifndef _WIN32
static bool watch_termination;
static volatile sig_atomic_t termination_signal;

static void request_termination(int signal_number) {
  termination_signal = signal_number;
}

static void watch_sigterm(void) {
  struct sigaction action = {.sa_handler = request_termination};
  sigemptyset(&action.sa_mask);
  watch_termination = sigaction(SIGTERM, &action, NULL) == 0;
}
#endif

/* SaltsBase 0.1.8's static CmdParser calls this unconditionally. The build
 * resolver loads the selected project's .env, using --root and its own schema.
 */
int dotenv_load_default(bool overwrite) {
  (void)overwrite;
  return -1;
}

static void usage(const char *action, const CmdArgerSubCommand *commands,
                  uint32_t command_count, const CmdArgerDesc *options,
                  uint32_t option_count) {
  puts(app_version);
  if (same(action, "tool")) {
    puts(
        "Usage: " SLATE_NAME " tool NAME [--root DIR] [--preset NAME] "
        "[--platform NAME] [--compiler DRIVER] [--build-dir DIR] [-- ARGS...]");
    puts("Use '" SLATE_NAME " tool list' to list available Ninja tools.");
    return;
  }
  if (same(action, "run")) {
    puts("Usage: " SLATE_NAME " run --target NAME [OPTIONS] [-- ARGS...]");
  } else if (action) {
    print_line("Usage: " SLATE_NAME " {} [OPTIONS]", action);
  } else {
    puts("Usage: " SLATE_NAME " <command> [OPTIONS]");
    for (uint32_t i = 0; i < command_count; ++i)
      print_line("  {}  {}", commands[i].name, commands[i].info);
  }
  puts("\nOptions:");
  for (uint32_t i = 0; i < option_count; ++i)
    if (!options[i].group)
      print_line("  --{}  {}", options[i].name, options[i].info);
  puts("  --help     Show help\n  --version  Show the version");
}

static int normalize_options(char **out, int count, char **args,
                             const CmdArgerDesc *options, uint32_t option_count,
                             bool *help) {
  int size = 2;
  out[0] = args[0];
  out[1] = args[1];
  int i = 2;
  if (same(args[1], "tool") && i < count && !starts(args[i], "--"))
    out[size++] = args[i++];
  for (; i < count; ++i) {
    const char *arg = args[i];
    if (same(arg, "--help")) {
      *help = true;
      continue;
    }
    if (!starts(arg, "--"))
      failf("unknown option '{}'", arg);
    size_t name_length = strcspn(arg + 2, "=");
    const CmdArgerDesc *option = NULL;
    for (uint32_t k = 0; k < option_count; ++k)
      if (strlen(options[k].name) == name_length &&
          !strncmp(arg + 2, options[k].name, name_length)) {
        option = &options[k];
        break;
      }
    if (!option)
      failf("unknown option '{}'", arg);
    if (arg[2 + name_length] == '=' || option->kind == CmdArgerDescKind_flag) {
      out[size++] = args[i];
      continue;
    }
    if (i + 1 == count || starts(args[i + 1], "--"))
      failf("{} requires a value", arg);
    // CmdParser takes one value per occurrence. Keep a variadic --targets
    // group and literal @-prefixed option values in the existing CLI.
    do {
      out[size++] = format_string("{}={}", arg, args[++i]);
    } while (same(option->name, "targets") && i + 1 < count &&
             !starts(args[i + 1], "--"));
  }
  return size;
}

static Request arguments(int count, char **args) {
  char *root = NULL, *preset = NULL, *platform = NULL, *driver = NULL;
  char *dir = NULL, *explain = NULL, *locked_target = NULL, *locked_host = NULL;
  char *format = NULL;
  char *ninja_tool = NULL, *prefix = NULL, *components = NULL;
  char *test_filter = NULL, *test_label = NULL;
  char **targets = take((size_t)count * sizeof(*targets));
  uint32_t target_count = 0;
  CmdArgerBool tests = cmd_arger_false, list_tests = cmd_arger_false;
  CmdArgerDesc options[] = {
      cmd_arger_desc_string(&root, "root", "Project directory"),
      cmd_arger_desc_string(&preset, "preset", "Preset name"),
      cmd_arger_desc_string(&platform, "platform", "Target platform"),
      cmd_arger_desc_string(&driver, "compiler", "Compiler driver"),
      cmd_arger_desc_string(&dir, "build-dir", "Build output directory"),
      cmd_arger_desc_string_list(targets, &target_count, (uint32_t)count,
                                 "target", "Select one target name"),
      cmd_arger_desc_string_list(targets, &target_count, (uint32_t)count,
                                 "targets", "Select one or more target names"),
      cmd_arger_desc_flag(&tests, "tests", "Include test targets"),
      cmd_arger_desc_flag(&list_tests, "list", "List matching tests (test)"),
      cmd_arger_desc_string(&test_filter, "filter",
                            "Test name substring (test)"),
      cmd_arger_desc_string(&test_label, "label", "Exact test label (test)"),
      cmd_arger_desc_string(&explain, "explain",
                            "Explain a target (gen or build)"),
      cmd_arger_desc_string(&format, "format", "Graph format (dot or json)"),
      cmd_arger_desc_string(&prefix, "prefix", "Installation directory"),
      cmd_arger_desc_string(&components, "components",
                            "Install runtime,dev,symbols (comma-separated)"),
      cmd_arger_with_group(cmd_arger_desc_string(&locked_target,
                                                 "locked-target",
                                                 "Resolved target toolchain"),
                           "Internal options"),
      cmd_arger_with_group(cmd_arger_desc_string(&locked_host, "locked-host",
                                                 "Resolved host toolchain"),
                           "Internal options"),
  };
  CmdArgerDesc tool_name =
      cmd_arger_desc_string(&ninja_tool, "name", "Ninja subtool name");
  CmdArgerSubCommand commands[] = {
      {.name = "gen", .info = "Generate a Ninja build"},
      {.name = "build", .info = "Generate and build targets with Ninja"},
      {.name = "run", .info = "Build and run an executable target"},
      {.name = "test", .info = "Build and run declared tests"},
      {.name = "install", .info = "Build and install selected components"},
      {.name = "tool",
       .info = "Run a Ninja subtool",
       .required_args = &tool_name,
       .required_args_count = 1},
      {.name = "doctor", .info = "Check the project and toolchain"},
      {.name = "graph", .info = "Print the dependency graph"},
      {.name = "_env", .info = "Refresh Ninja's environment stamp"},
  };
  const uint32_t option_count = sizeof(options) / sizeof(*options);
  const uint32_t command_count = sizeof(commands) / sizeof(*commands);
  const uint32_t public_commands = command_count - 1;
  if (count < 2 || same(args[1], "--help") || same(args[1], "help")) {
    usage(NULL, commands, public_commands, options, option_count);
    exit(count < 2 ? EXIT_FAILURE : EXIT_SUCCESS);
  }
  uint32_t action = 0;
  while (action < command_count && !same(args[1], commands[action].name))
    ++action;
  if (action == command_count)
    failf("unknown command '{}'; see --help", args[1]);
  char **normalized = take(((size_t)count + 1) * sizeof(*normalized));
  bool help = false;
  int normalized_count =
      normalize_options(normalized, count, args, options, option_count, &help);
  if (help) {
    usage(commands[action].name, commands, public_commands, options,
          option_count);
    exit(EXIT_SUCCESS);
  }
  if (same(args[1], "tool") &&
      (count < 3 || starts(args[2], "-") || !identifier(args[2])))
    fail("tool requires a Ninja subtool name; see tool --help");
  int selected = -1;
  cmd_arger_parse_subcommand(
      options, option_count, commands,
      action < public_commands ? public_commands : command_count, &selected,
      normalized_count, normalized, app_version, cmd_arger_false);
  const bool test = same(commands[selected].name, "test");
  if (!test && (list_tests || test_filter || test_label))
    fail("--list, --filter and --label require test");
  Request req = {.command = commands[selected].name,
                 .root = root,
                 .preset = preset,
                 .platform = platform,
                 .driver = driver,
                 .build_dir = dir,
                 .ninja_tool = ninja_tool,
                 .explain = explain,
                 .tests = test || tests != 0,
                 .list_tests = list_tests != 0,
                 .test_filter = test_filter,
                 .test_label = test_label,
                 .locked_target = locked_target,
                 .locked_host = locked_host};
  if (same(req.command, "install")) {
    if (prefix && !*prefix)
      fail("--prefix requires a nonempty directory");
    req.install_prefix = prefix;
    req.install_components = parse_install_components(components);
  } else if (prefix || components) {
    fail("--prefix and --components require install");
  }
  if (format && (!same(req.command, "graph") ||
                 (!same(format, "dot") && !same(format, "json"))))
    fail("--format requires graph and accepts dot or json");
  req.graph_json = same(format, "json");
  if (explain && !same(req.command, "gen") && !same(req.command, "build"))
    fail("--explain requires gen or build");
  for (uint32_t i = 0; i < target_count; ++i)
    unique(&req.targets, targets[i]);
  if (same(req.command, "run") && req.targets.count != 1)
    fail("run requires exactly one --target NAME");
  if (ninja_tool && (req.targets.count || req.tests))
    fail("tool takes Ninja targets and options after --");
  return req;
}

static const char *find_ninja(const char *root) {
  const char *ninja = find_program(root, "ninja");
  if (!ninja)
    fail("Ninja not found on PATH");
  return ninja;
}

int command_termination_status(void) {
#ifndef _WIN32
  if (termination_signal)
    return 128 + termination_signal;
#endif
  return 0;
}

int wait_child(process_t *child, process_result_t *result) {
#ifndef _WIN32
  if (watch_termination) {
    // Salts owns a separate process group. Return to the caller to destroy it
    // before Slate exits.
    while (!termination_signal) {
      int rc = process_wait_for(child, 100, result);
      if (rc != SALTS_ETIMEDOUT)
        return rc;
    }
    return SALTS_ECANCELED;
  }
#endif
  return process_wait(child, result);
}

int run_child(const char *role, const char *program, const char *const *args,
              const char *directory) {
  int status = command_termination_status();
  if (status)
    return status;
  process_options_t options;
  process_options_init(&options);
  options.program = program;
  options.args = args;
  options.cwd = directory;
  options.flags = 0;
  process_t *child;
  int rc = process_spawn(&options, &child);
  if (rc)
    failf("cannot start {} '{}' ({})", role, program, rc);
  process_result_t result;
  rc = wait_child(child, &result);
  process_destroy(child);
  status = command_termination_status();
  if (status)
    return status;
  if (rc || result.state != SALTS_PROCESS_EXITED)
    failf("{} did not exit normally", role);
  return result.exit_code;
}

static int run_ninja_tool(const Request *request, char **tool_args) {
  const char *root;
  const char *directory;
  if (request->build_dir || same(request->ninja_tool, "list")) {
    root = find_project_root(request);
    if (!root)
      root = cwd();
    directory = request->build_dir ? path(root, request->build_dir) : root;
  } else {
    Project project = load_project(request);
    root = project.root;
    directory = project.build_dir;
  }
  Strings args = {0};
  push(&args, "-t");
  push(&args, request->ninja_tool);
  for (; tool_args && *tool_args; ++tool_args)
    push(&args, *tool_args);
  push(&args, NULL);
  return run_child("Ninja", find_ninja(root), args.items, directory);
}

static int install_project(const Project *project, const char *ninja) {
  const Request *req = project->request;
  Strings targets = {0};
  for (unsigned i = 0; i < INSTALL_COMPONENT_COUNT; ++i)
    if (req->install_components & (1u << i))
      push(&targets, install_target((InstallComponent)i));
  push(&targets, NULL);
  int rc = run_child("Ninja", ninja, targets.items, project->build_dir);
  if (rc)
    return rc;
  run_install(path(project->build_dir, INSTALL_PLAN),
              install_prefix(project, req->install_prefix),
              req->install_components);
  return 0;
}

static int archive(const char *output, const char *tool, char **args) {
  if (is_file(output)) {
    int rc = remove_file(output);
    if (rc)
      failf("cannot replace archive '{}' ({})", output, rc);
  }
  return run_child("archiver", tool, (const char *const *)args, NULL);
}

static int extract_symbols(const char *format, const char *source,
                           const char *destination) {
  const bool macho = same(format, "macho");
  if (!macho && !same(format, "elf"))
    failf("unknown debug symbol format '{}'", format);
  const char *name = macho ? "dsymutil" : "llvm-objcopy";
  const char *tool = find_program(cwd(), name);
  if (!tool && !macho)
    tool = find_program(cwd(), "objcopy");
  if (!tool)
    failf("{} is required to install debug symbols for '{}'", name, source);
  mkdirs(parent(destination));
  const char *const elf[] = {"--only-keep-debug", source, destination, NULL};
  const char *const apple[] = {source, "-o", destination, NULL};
  return run_child("Debug symbol tool", tool, macho ? apple : elf, NULL);
}

static void render(const char *template, const char *output,
                   const char *context) {
  size_t length;
  const char *source = read_file(template, &length);
  MUSTACHE_TEMPLATE *compiled = mustache_compile(source, length, NULL, NULL, 0);
  if (!compiled)
    failf("invalid Mustache template '{}'", template);
  Json *data = context ? load_json(context) : own(json_create_object());
  MUSTACHE_STRING_RENDERER writer;
  if (mustache_string_renderer_init(&writer)) {
    mustache_release(compiled);
    fail("cannot initialize Mustache renderer");
  }
  int rc =
      mustache_render_json(compiled, data, &writer.base, &writer, NULL, NULL);
  char *result = rc ? NULL : mustache_string_renderer_get(&writer);
  mustache_string_renderer_free(&writer);
  mustache_release(compiled);
  if (!result)
    failf("cannot render '{}'", template);
  write_file(output, result, strlen(result));
  free(result);
}

int main(int argc, char **argv) {
  init_console_output();
  utf8_arguments(&argc, &argv);
  const char *action = argc > 1 ? argv[1] : "";
  if (same(action, "--version")) {
    puts(app_version);
    return 0;
  }
#ifdef _WIN32
  if (same(action, "_vsenv") && argc == 2)
    return write_vsdevcmd_environment();
#endif
  if (same(action, "_archive") && argc >= 5)
    return archive(argv[2], argv[3], argv + 4);
  if (same(action, "_copy") && argc == 4) {
    copy_file(argv[2], argv[3]);
    return 0;
  }
  if (same(action, "_install") && argc == 4) {
    run_install(argv[2], argv[3], INSTALL_ALL);
    return 0;
  }
  if (same(action, "_symbols") && argc == 5)
    return extract_symbols(argv[2], argv[3], argv[4]);
  if (same(action, "_render") && (argc == 4 || argc == 5)) {
    render(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    return 0;
  }
  int option_count = argc;
  if (same(action, "tool") || same(action, "run")) {
    for (int i = 2; i < argc; ++i)
      if (same(argv[i], "--")) {
        option_count = i;
        break;
      }
  }
  Request req = arguments(option_count, argv);
  char **extra_args = option_count < argc ? argv + option_count + 1 : NULL;
  if (req.ninja_tool)
    return run_ninja_tool(&req, extra_args);
  action = req.command;
#ifndef _WIN32
  if (same(action, "build") || same(action, "test") || same(action, "run"))
    watch_sigterm();
#endif
  if (req.explain && !req.targets.count)
    push(&req.targets, req.explain);
  Project project = load_project(&req);
  Graph graph = load_graph(&project);
  if (same(action, "test")) {
    if (req.list_tests)
      return test_project(&graph, NULL);
    console_start("Test", &req);
    return console_finish("Test",
                          test_project(&graph, find_ninja(project.root)));
  }
  const bool build = same(action, "build");
  const bool run = same(action, "run");
  const bool install = same(action, "install");
  if (build || run || install || same(action, "gen")) {
    if (!graph.count)
      failf("no module targets found under {}. Check modules, moduleFiles and "
            "include",
            project.root);
    const Module *program = NULL;
    if (run) {
      program = find_module(&graph, req.targets.items[0]);
      if (!program || !module_runnable(&project, program))
        failf("target '{}' is not a host-runnable executable",
              req.targets.items[0]);
    }
    const char *ninja =
        build || run || install ? find_ninja(project.root) : NULL;
    if (req.explain)
      explain_module(&graph, req.explain);
    if (build || run)
      console_start(build ? "Build" : "Run", &req);
    emit_ninja(&graph);
    print_project(&project);
    if (install) {
      fflush(stdout);
      return install_project(&project, ninja);
    }
    if (build || run) {
      fflush(stdout);
      int rc = run_child("Ninja", ninja, NULL, project.build_dir);
      if (build)
        return console_finish("Build", rc);
      if (!rc)
        rc = run_child(program->name, program->artifact,
                       (const char *const *)extra_args, project.build_dir);
      return console_finish("Run", rc);
    }
  } else if (same(action, "doctor")) {
    const char *ninja = find_ninja(project.root);
    const char *version,
        *problem =
            probe_program(ninja, (const char *const[]){"--version", NULL},
                          project.root, false, &version);
    if (problem)
      fail(problem);
    print_project(&project);
    print_line("Ninja: {} ({})", version, ninja);
  } else if (same(action, "graph")) {
    if (req.graph_json)
      print_graph_json(&graph);
    else
      print_graph(&graph);
  } else {
    write_environment_stamp(&graph);
  }
  return 0;
}
