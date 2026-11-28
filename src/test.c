#include "build.h"

#include <stdlib.h>
#include <string.h>

static const char *const *test_vector(const Strings *values) {
  if (!values->count)
    return NULL;
  const char **items = take((values->count + 1) * sizeof(*items));
  memcpy(items, values->items, values->count * sizeof(*items));
  items[values->count] = NULL;
  return items;
}

static bool run_test(const Project *project, const Module *module) {
  Strings args = module->test_args;
  if (project->request->test_args) {
    args = (Strings){0};
    for (size_t i = 0; i < module->test_args.count; ++i)
      push(&args, module->test_args.items[i]);
    for (const char *const *arg = project->request->test_args; *arg; ++arg)
      push(&args, *arg);
  }
  process_options_t options;
  process_options_init(&options);
  options.program =
      module->test_program ? module->test_program : module->artifact;
  options.cwd =
      module->test_directory ? module->test_directory : project->build_dir;
  options.args = test_vector(&args);
  options.env = test_vector(&module->test_environment);
  options.timeout_ms = (uint64_t)module->test_timeout * 1000;
  options.flags = 0;

  print_line("{}  {}", console_mark("RUN", "36"), module->test_name);
  fflush(stdout);
  process_t *child;
  int rc = process_spawn(&options, &child);
  if (rc) {
    print_line("{} {} (cannot start '{}' in '{}' [{}])",
               console_mark("FAIL", "31"), module->test_name, options.program,
               options.cwd, rc);
    return false;
  }
  process_result_t result;
  rc = wait_child(child, &result);
  process_destroy(child);
  if (command_termination_status())
    return false;
  const char *failure = console_mark("FAIL", "31");
  if (rc)
    print_line("{} {} (wait error {})", failure, module->test_name, rc);
  else if (result.state == SALTS_PROCESS_TIMED_OUT)
    print_line("{} {} (timed out after {} s)", failure, module->test_name,
               module->test_timeout);
  else if (result.state == SALTS_PROCESS_SIGNALED)
    print_line("{} {} (signal {})", failure, module->test_name,
               result.term_signal);
  else if (result.state != SALTS_PROCESS_EXITED)
    print_line("{} {} (process state {}, error {})", failure, module->test_name,
               (int)result.state, result.error_code);
  else if (result.exit_code && result.exit_code != project->request->test_accept_exit)
    print_line("{} {} (exit {})", failure, module->test_name, result.exit_code);
  else {
    print_line("{} {}", console_mark("PASS", "32"), module->test_name);
    return true;
  }
  return false;
}

int test_project(Graph *graph, const char *ninja) {
  const Project *project = graph->project;
  const Request *request = project->request;
  Strings names = {0}, targets = {0};
  const Module **tests = NULL;
  size_t count = 0, capacity = 0;
  for (size_t i = 0; i < graph->count; ++i) {
    const Module *module = graph->modules[i];
    if (!module->selected || !module->test_name ||
        (!project->native && module->tools != &project->host))
      continue;
    if (contains(&names, module->test_name))
      failf("duplicate test name '{}'", module->test_name);
    push(&names, module->test_name);
    if ((request->test_filter &&
         !strstr(module->test_name, request->test_filter)) ||
        (request->test_label &&
         !contains(&module->test_labels, request->test_label)))
      continue;
    tests = reserve(tests, count + 1, &capacity, sizeof(*tests));
    tests[count++] = module;
  }
  if (request->list_tests) {
    for (size_t i = 0; i < count; ++i)
      puts(tests[i]->test_name);
    return 0;
  }
  if (!count)
    fail("no matching host-runnable tests");
  if (count != 1 && (request->test_args || request->test_accept_exit))
    fail("test arguments and --accept-exit require exactly one matching test");
  for (size_t i = 0; i < count; ++i)
    push(&targets, module_alias(tests[i]));
  push(&targets, NULL);
  emit_ninja(graph);
  print_project(project);
  fflush(stdout);
  int rc = run_child("Ninja", ninja, targets.items, project->build_dir);
  if (rc)
    return rc;
  size_t passed = 0;
  for (size_t i = 0; i < count && !command_termination_status(); ++i)
    passed += run_test(project, tests[i]);
  rc = command_termination_status();
  if (rc)
    return rc;
  print_line("{} passed, {} failed", passed, count - passed);
  return passed == count ? EXIT_SUCCESS : EXIT_FAILURE;
}
