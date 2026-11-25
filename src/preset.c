#include "build.h"

#include <core/vstr.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char project_manifest[] = "slate.json";

static bool env_key(vstr name) {
  if (!name.len ||
      !(isalpha((unsigned char)name.data[0]) || name.data[0] == '_'))
    return false;
  for (size_t i = 1; i < name.len; ++i)
    if (!(isalnum((unsigned char)name.data[i]) || name.data[i] == '_'))
      return false;
  return true;
}

const char *find_project_root(const Request *request) {
  if (request->root)
    return path(cwd(), request->root);
  for (const char *dir = cwd();; dir = parent(dir)) {
    if (is_file(path(dir, project_manifest)))
      return dir;
    if (same(dir, parent(dir)))
      return NULL;
  }
}

static void load_fragment(Project *p, const char *file, Strings *stack) {
  if (contains(stack, file))
    failf("{}: preset include cycle", file);
  if (contains(&p->inputs, file))
    return;
  if (!inside(p->root, file))
    failf("{}: preset include escapes repository", file);
  push(stack, file);
  const Json *fragment = load_json(file);
  check_fields(fragment, file,
               " $schema include defaultPreset platforms toolchains presets "
               "styles env modules moduleFiles libraries tools ");
  Strings includes = {0};
  read_strings(&includes, member(fragment, "include"), file);
  for (size_t i = 0; i < includes.count; ++i)
    load_fragment(p, path(parent(file), includes.items[i]), stack);
  --stack->count;
  push(&p->inputs, file);
  for (size_t i = 0; i < json_object_size(fragment); ++i) {
    const char *key = json_object_key(fragment, i);
    const Json *value = json_object_value(fragment, i);
    if (same(key, "include") || same(key, "$schema"))
      continue;
    if (same(key, "libraries")) {
      const char *where = format_string("{}.libraries", file);
      const char *directory = parent(file);
      Strings libraries = {0};
      read_strings(&libraries, value, where);
      for (size_t j = 0; j < libraries.count; ++j) {
        if (!*libraries.items[j])
          failf("{}: expected a nonempty library manifest path", where);
        unique(&p->libraries, path(directory, libraries.items[j]));
      }
      continue;
    }
    if (same(key, "env")) {
      const char *where = format_string("{}.env", file);
      Strings names = {0};
      read_strings(&names, value, where);
      for (size_t j = 0; j < names.count; ++j) {
        const char *name = names.items[j];
        if (!env_key(vstr_from_cstr(name)) || contains(&p->env_keys, name))
          failf("{}: invalid or duplicate environment variable '{}'", where,
                name);
        push(&p->env_keys, name);
      }
      continue;
    }
    if (same(key, "defaultPreset") || same(key, "modules") ||
        same(key, "moduleFiles")) {
      if (member(p->manifest, key))
        failf("{}: duplicate {}", file, key);
      json_set(p->manifest, key, json_clone(value));
      continue;
    }
    object(value, format_string("{}.{}", file, key));
    Json *group = json_object_get(p->manifest, key);
    if (!group) {
      group = json_create_object();
      json_set(p->manifest, key, group);
    }
    for (size_t j = 0; j < json_object_size(value); ++j) {
      const char *name = json_object_key(value, j);
      if (!identifier(name) || member(group, name))
        failf("{}: invalid or duplicate {}.{}", file, key, name);
      json_set(group, name, json_clone(json_object_value(value, j)));
    }
  }
}

static void load_environment(Project *p) {
  if (!is_file(p->dot_env))
    return;
  size_t size;
  const char *bytes = read_file(p->dot_env, &size);
  if (size >= 3 && !memcmp(bytes, "\xEF\xBB\xBF", 3)) {
    bytes += 3;
    size -= 3;
  }
  vstr rest = vstr_from_buf(bytes, size);
  if (!vstr_utf8_valid(rest) || memchr(bytes, 0, size))
    failf("{}: expected UTF-8 without NUL bytes", p->dot_env);
  for (size_t line_no = 1; rest.len; ++line_no) {
    vstr line =
        vstr_trim(vstr_split_next(&rest, vstr_from_cstr("\n")), " \t\r");
    if (!line.len || line.data[0] == '#')
      continue;
    vstr key, equal, value;
    if (!vstr_partition(line, vstr_from_cstr("="), &key, &equal, &value))
      failf("{}:{}: expected KEY=value", p->dot_env, line_no);
    key = vstr_trim(key, " \t");
    if (!env_key(key))
      failf("{}:{}: invalid variable name", p->dot_env, line_no);
    const char *name = slice(key.data, key.len);
    if (!contains(&p->env_keys, name))
      continue;
    if (setting(&p->environment, name))
      failf("{}:{}: duplicate variable {}", p->dot_env, line_no, name);
    value = vstr_trim(value, " \t");
    if (value.len && (value.data[0] == '"' || value.data[0] == '\'')) {
      if (value.len < 2 || value.data[0] != value.data[value.len - 1])
        failf("{}:{}: unterminated quoted value for {}", p->dot_env, line_no,
              name);
      value = vstr_sub(value, 1, value.len - 2);
    }
    put(&p->environment, name, own(json_create_string_n(value.data, value.len)),
        format_string("{}:{}", p->dot_env, line_no));
  }
}

static const char *expand_host(const char *text, const char *where) {
  Text out = {0};
  const char *start = text, *ref;
  while ((ref = strstr(start, "${host:"))) {
    append_n(&out, start, (size_t)(ref - start));
    if (starts(ref, "${host:exe}")) {
      append(&out, same(host_os(), "windows") ? ".exe" : "");
      start = ref + 11;
    } else if (starts(ref, "${host:rid}")) {
      append_format(&out, "{}-{}", host_os(), host_arch());
      start = ref + 11;
    } else {
      failf("{}: unknown host substitution in '{}'", where, text);
    }
  }
  if (start == text)
    return text;
  append(&out, start);
  return contents(&out);
}

static const char *environment_value(Project *p, const char *name,
                                     const char *where, const char **problem,
                                     bool track) {
  if (!contains(&p->env_keys, name)) {
    *problem =
        format_string("{}: undeclared environment variable {}", where, name);
    return NULL;
  }
  const char *value = environment_variable(name);
  const char *origin = "process environment";
  if (!value) {
    const Setting *local = setting(&p->environment, name);
    if (!local) {
      *problem = format_string(
          "{}: {} is required in .env or the process environment", where, name);
      return NULL;
    }
    value = string(local->value, name);
    origin = local->origin;
  }
  if (!*value) {
    *problem =
        format_string("{}: {} has an empty value ({})", where, name, origin);
    return NULL;
  }
  if (track && !setting(&p->used_environment, name))
    put(&p->used_environment, name, own(json_create_string(value)), origin);
  return value;
}

const char *expand_env(Project *p, const char *text, const char *where,
                       const char **problem, bool track) {
  text = expand_host(text, where);
  Text out = {0};
  const char *start = text;
  for (;;) {
    const char *raw_ref = strstr(start, "${env:");
    const char *path_ref = strstr(start, "${envPath:");
    const bool as_path = path_ref && (!raw_ref || path_ref < raw_ref);
    const char *ref = as_path ? path_ref : raw_ref;
    if (!ref)
      break;
    append_n(&out, start, (size_t)(ref - start));
    const char *key =
        ref + (as_path ? sizeof("${envPath:") - 1 : sizeof("${env:") - 1);
    const char *end = strchr(key, '}');
    if (!end || !env_key(vstr_from_buf(key, (size_t)(end - key)))) {
      *problem = format_string("{}: invalid environment reference", where);
      return NULL;
    }
    const char *name = slice(key, (size_t)(end - key));
    const char *value = environment_value(p, name, where, problem, track);
    if (!value)
      return NULL;
    append(&out, as_path ? path(p->root, value) : value);
    start = end + 1;
  }
  if (start == text)
    return text;
  append(&out, start);
  return contents(&out);
}

static void apply_preset(Project *p, const Json *presets, const char *name,
                         Strings *stack) {
  if (contains(stack, name))
    failf("preset inheritance cycle at {}", name);
  if (stack->count >= 128)
    fail("preset inheritance is too deep");
  const Json *preset = member(presets, name);
  if (!preset)
    failf("unknown preset '{}'", name);
  const char *origin = format_string("preset:{}", name);
  check_fields(
      preset, origin,
      " inherits config targetPlatform toolchain hostToolchain vsDevCmd "
      "buildDir cStd cxxStd templateDir linkPoolDepth options defines styles "
      "packages flags tests ");
  const char *base = optional_string(member(preset, "inherits"), origin);
  if (base) {
    push(stack, name);
    apply_preset(p, presets, base, stack);
    --stack->count;
  }
  for (size_t i = 0; i < json_object_size(preset); ++i) {
    const char *key = json_object_key(preset, i);
    const Json *value = json_object_value(preset, i);
    put(&p->fields, key, value, origin);
  }
  overlay(&p->options, member(preset, "options"), origin);
  overlay(&p->definitions, member(preset, "defines"), origin);
  read_strings(&p->preset_styles, member(preset, "styles"), origin);
}

void apply_style(const Project *p, const char *name, Settings *options,
                 Usage *usage, Strings *stack) {
  if (contains(stack, name))
    failf("style inheritance cycle at {}", name);
  if (stack->count >= 128)
    fail("style inheritance is too deep");
  const Json *style = member(p->styles, name);
  if (!style)
    failf("unknown style '{}'", name);
  const char *origin = format_string("style:{}", name);
  check_fields(style, origin, " extends options defines flags ");
  Strings bases = {0};
  read_strings(&bases, member(style, "extends"), origin);
  push(stack, name);
  for (size_t i = 0; i < bases.count; ++i)
    apply_style(p, bases.items[i], options, usage, stack);
  --stack->count;
  overlay(options, member(style, "options"), origin);
  overlay(&usage->defines, member(style, "defines"), origin);
  read_strings(&usage->flags, member(style, "flags"), origin);
}

static const char *native_platform(const Json *platforms) {
  const char *match = NULL;
  for (size_t i = 0; i < json_object_size(platforms); ++i) {
    const Json *platform = json_object_value(platforms, i);
    if (!same(optional_string(member(platform, "os"), "platform.os"),
              host_os()) ||
        !same(optional_string(member(platform, "arch"), "platform.arch"),
              host_arch()))
      continue;
    if (match)
      failf("more than one platform matches {}/{}; set targetPlatform",
            host_os(), host_arch());
    match = json_object_key(platforms, i);
  }
  if (!match)
    failf("no platform matches host {}/{}", host_os(), host_arch());
  return match;
}

static const char *resolve_tool(Project *p, const Json *manifest,
                                const char *id, const char *field,
                                const char **problem, bool track) {
  const char *where = format_string("toolchains.{}.{}", id, field);
  const char *tool = string(member(manifest, field), where);
  tool = expand_env(p, tool, where, problem, track);
  if (!tool)
    return NULL;
  const char *resolved = find_program(p->root, tool);
  if (!resolved)
    *problem = format_string("{}: tool '{}' was not found", where, tool);
  return resolved;
}

static bool toolchain(Project *p, const Json *manifest, Toolchain *t,
                      const char **problem, bool track) {
  const char *restrict_os = optional_string(member(manifest, "hostOs"), t->id);
  const char *restrict_arch =
      optional_string(member(manifest, "hostArch"), t->id);
  if ((restrict_os && !same(restrict_os, host_os())) ||
      (restrict_arch && !same(restrict_arch, host_arch())) ||
      (same(t->driver, "msvc") && !same(host_os(), "windows"))) {
    *problem = "toolchain cannot run on this host";
    return false;
  }
  t->msvc = same(t->driver, "msvc");
  if (t->msvc) {
    if (!same(t->os, "windows")) {
      *problem = "MSVC requires a Windows target";
      return false;
    }
    const char *active_arch = environment_variable("VSCMD_ARG_TGT_ARCH");
    if (active_arch && *active_arch && !same(active_arch, t->arch)) {
      *problem = format_string(
          "Visual Studio environment targets {}; initialize it for {}",
          active_arch, t->arch);
      return false;
    }
  }
  t->cc = resolve_tool(p, manifest, t->id, "cc", problem, track);
  if (!t->cc)
    return false;
  t->cxx = resolve_tool(p, manifest, t->id, "cxx", problem, track);
  if (!t->cxx)
    return false;
  t->ar = resolve_tool(p, manifest, t->id, "ar", problem, track);
  if (!t->ar)
    return false;
  t->ld = resolve_tool(p, manifest, t->id, "ld", problem, track);
  if (!t->ld)
    return false;
  if (member(manifest, "launcher")) {
    t->launcher = resolve_tool(p, manifest, t->id, "launcher", problem, track);
    if (!t->launcher)
      return false;
  }
  t->triple = optional_string(member(manifest, "triple"), t->id);
  t->runtime_bins = member(manifest, "runtimeBins");
  const char *sysroot = optional_string(member(manifest, "sysroot"), t->id);
  if (sysroot) {
    const char *value =
        expand_env(p, sysroot, format_string("toolchains.{}.sysroot", t->id),
                   problem, track);
    if (!value)
      return false;
    t->sysroot = path(p->root, value);
    if (!is_dir(t->sysroot)) {
      *problem = "sysroot directory was not found";
      return false;
    }
  }
  return true;
}

const char *compiler_target(const Toolchain *tools) {
  return same(tools->driver, "clang") || same(tools->driver, "llvm-mingw")
             ? tools->triple
             : NULL;
}

const char *darwin_arch(const Toolchain *tools) {
  if (!same(tools->driver, "clang") ||
      (!same(tools->os, "macos") && !same(tools->os, "ios")))
    return NULL;
  return same(tools->arch, "x64") ? "x86_64" : tools->arch;
}

static bool triple_matches(const Toolchain *tools, const char *triple) {
  const char *dash = strchr(triple, '-');
  if (!dash)
    return false;
  const char *arch = slice(triple, (size_t)(dash - triple));
  if (same(arch, "x86_64") || same(arch, "amd64"))
    arch = "x64";
  else if (same(arch, "aarch64"))
    arch = "arm64";
  else if (same(arch, "i386") || same(arch, "i486") || same(arch, "i586") ||
           same(arch, "i686"))
    arch = "x86";
  if (!same(arch, tools->arch))
    return false;
  if (strstr(dash, "-android"))
    return same(tools->os, "android");
  if (strstr(dash, "-linux"))
    return same(tools->os, "linux");
  if (strstr(dash, "-windows") || strstr(dash, "-mingw"))
    return same(tools->os, "windows");
  if (strstr(dash, "-darwin") || strstr(dash, "-macos"))
    return same(tools->os, "macos");
  if (strstr(dash, "-ios"))
    return same(tools->os, "ios");
  return false;
}

static const char *probe_toolchain(const Project *p, const Toolchain *t,
                                   const char **version) {
  const char *version_args[] = {t->msvc ? "/Bv" : "--version", NULL};
  const char *problem =
      probe_program(t->cc, version_args, p->root, t->msvc, version);
  if (problem)
    return problem;
  if (t->msvc) {
    const char *machine = strrchr(*version, ' ');
    const char *expected = same(t->arch, "arm64") ? "ARM64" : t->arch;
    return machine &&
                   (same(machine + 1, expected) || same(machine + 1, t->arch))
               ? NULL
               : format_string("compiler reports '{}' for target {}", *version,
                               t->arch);
  }

  const char *args[5];
  size_t count = 0;
  const char *triple = compiler_target(t);
  const char *arch = darwin_arch(t);
  if (triple)
    args[count++] = format_string("--target={}", triple);
  if (arch) {
    args[count++] = "-arch";
    args[count++] = arch;
  }
  args[count++] = "-dumpmachine";
  args[count] = NULL;
  const char *programs[] = {t->cc, t->cxx, t->ld};
  for (size_t i = 0; i < sizeof(programs) / sizeof(*programs); ++i) {
    const char *program = programs[i];
    if (i && (same(program, t->cc) || (i == 2 && same(program, t->cxx))))
      continue;
    const char *machine;
    problem = probe_program(program, args, p->root, false, &machine);
    if (problem)
      return problem;
    if (!triple_matches(t, machine))
      return format_string("{} reports target '{}' for {}/{}", program, machine,
                           t->os, t->arch);
  }
  return NULL;
}

static Toolchain select_toolchain(Project *p, const char *platform_id,
                                  const char *pinned, const char *driver,
                                  const char *devcmd) {
  const Json *platform = member(member(p->manifest, "platforms"), platform_id);
  if (!platform)
    failf("unknown platform '{}'", platform_id);
  check_fields(platform, platform_id, " os arch toolchains ");
  const char *os = string(member(platform, "os"), platform_id);
  const char *arch = string(member(platform, "arch"), platform_id);
  if (devcmd) {
    if (!same(os, "windows") || !same(host_os(), "windows") ||
        (driver && !same(driver, "msvc")))
      fail("vsDevCmd requires a Windows MSVC target on a Windows host");
    driver = "msvc";
  }
  const Json *candidates = member(platform, "toolchains");
  array(candidates, platform_id);
  Text rejected = {0};
  for (size_t i = 0; i < json_array_size(candidates); ++i) {
    const Json *choice = json_array_get(candidates, i);
    check_fields(choice, platform_id, " id vcpkgTriplet vcpkgRoot ");
    const char *id = string(member(choice, "id"), platform_id);
    if (pinned && !same(pinned, id))
      continue;
    const Json *manifest = member(member(p->manifest, "toolchains"), id);
    if (!manifest)
      failf("{} references unknown toolchain '{}'", platform_id, id);
    check_fields(manifest, id,
                 " driver cc cxx ar ld launcher hostOs hostArch triple sysroot "
                 "versionContains runtimeBins ");
    const char *family = string(member(manifest, "driver"), id);
    if (driver && !same(driver, family))
      continue;
    if (!same(family, "msvc") && !same(family, "llvm-mingw") &&
        !same(family, "clang") && !same(family, "gcc"))
      failf("{}: unsupported driver '{}'", id, family);
    const char *problem = NULL;
#ifdef _WIN32
    if (devcmd) {
      const char *include = environment_variable("INCLUDE");
      const char *lib = environment_variable("LIB");
      const bool ready =
          same(environment_variable("VSCMD_ARG_TGT_ARCH"), arch) && include &&
          *include && lib && *lib;
      const char *where = format_string("presets.{}.vsDevCmd", p->name);
      const char *value = expand_env(p, devcmd, where, &problem, true);
      if (!value && !ready)
        fail(problem);
      if (value) {
        const char *script = path(p->root, value);
        const char *ext = strrchr(script, '.');
        if (!ext || (_stricmp(ext, ".bat") && _stricmp(ext, ".cmd")) ||
            !is_file(script))
          failf("{}: expected an existing .bat or .cmd file at '{}'", where,
                script);
        if (!ready)
          initialize_vsdevcmd(p->root, script, arch);
        unique(&p->inputs, script);
      }
      problem = NULL;
      devcmd = NULL;
    }
#endif
    Toolchain t = {
        .id = id,
        .driver = family,
        .platform = platform_id,
        .os = os,
        .arch = arch,
        .triplet = optional_string(member(choice, "vcpkgTriplet"), id),
        .vcpkg_root = optional_string(member(choice, "vcpkgRoot"), id)};
    if (toolchain(p, manifest, &t, &problem, false)) {
      const char *version = NULL;
      if (!same(p->request->command, "_env"))
        problem = probe_toolchain(p, &t, &version);
      const char *requires =
          optional_string(member(manifest, "versionContains"), id);
      if (!problem && requires && version && !strstr(version, requires))
        problem =
            format_string("compiler version does not contain '{}'", requires);
      if (!problem) {
        if (!toolchain(p, manifest, &t, &problem, true))
          fail(problem);
        if (p->verbose)
          print_line("  selected {} for {} ({})", id, platform_id, version);
        return t;
      }
    }
    if (p->verbose)
      print_line("  skipped {} for {} ({})", id, platform_id, problem);
    append_format(&rejected, "\n  {}: {}", id, problem);
  }
  failf("no usable toolchain for {}{}{}{}{}{}", platform_id,
        pinned ? " pinned to " : "", pinned ? pinned : "",
        driver ? " with driver " : "", driver ? driver : "",
        contents(&rejected));
}

Project load_project(const Request *request) {
  Project p = {.request = request,
               .root = find_project_root(request),
               .verbose = same(request->command, "doctor")};
  if (!p.root)
    failf("no {} found; pass --root", project_manifest);
  p.manifest = own(json_create_object());
  Strings stack = {0};
  load_fragment(&p, path(p.root, project_manifest), &stack);
  p.dot_env = path(p.root, ".env");
  load_environment(&p);
  p.name = request->preset
               ? request->preset
               : optional_string(member(p.manifest, "defaultPreset"),
                                 "defaultPreset");
  if (!p.name)
    fail("choose a preset with --preset or set defaultPreset");
  const Json *presets = member(p.manifest, "presets");
  object(presets, "presets");
  apply_preset(&p, presets, p.name, &stack);
  p.config = string(get(&p.fields, "config"),
                    format_string("presets.{}.config", p.name));
  if (!identifier(p.config))
    failf("invalid config '{}'", p.config);
  p.styles = member(p.manifest, "styles");
  const Json *platforms = member(p.manifest, "platforms");
  object(platforms, "platforms");
  const char *target =
      request->platform
          ? request->platform
          : optional_string(get(&p.fields, "targetPlatform"), "targetPlatform");
  if (!target)
    target = native_platform(platforms);
  const char *pinned =
      request->locked_target
          ? request->locked_target
          : optional_string(get(&p.fields, "toolchain"), "toolchain");
  const char *devcmd = optional_string(get(&p.fields, "vsDevCmd"), "vsDevCmd");
  p.target = select_toolchain(&p, target, pinned, request->driver, devcmd);
  p.native = same(p.target.os, host_os()) && same(p.target.arch, host_arch());
  const Json *tests = get(&p.fields, "tests");
  if (tests && json_type(tests) != JSON_BOOL)
    failf("presets.{}.tests must be a boolean", p.name);
  p.tests = request->tests || (tests ? json_bool(tests) : p.native);
  const char *host =
      request->locked_host
          ? request->locked_host
          : optional_string(get(&p.fields, "hostToolchain"), "hostToolchain");
  const char *native = p.native ? target : native_platform(platforms);
  p.host = same(native, target) && (!host || same(host, p.target.id))
               ? p.target
               : select_toolchain(&p, native, host, NULL, NULL);
  const Json *packages = get(&p.fields, "packages");
  if (packages) {
    object(packages, "packages");
    const Json *vcpkg = member(packages, "vcpkg");
    if (vcpkg) {
      check_fields(vcpkg, "packages.vcpkg", " root triplet config ");
      const char *override =
          optional_string(member(vcpkg, "triplet"), "packages.vcpkg.triplet");
      if (override && p.target.triplet && !same(override, p.target.triplet))
        failf("vcpkg triplet '{}' does not match selected ABI triplet '{}'",
              override, p.target.triplet);
      if (override)
        p.target.triplet = override;
    }
  }
  const char *dir =
      request->build_dir
          ? request->build_dir
          : optional_string(get(&p.fields, "buildDir"), "buildDir");
  if (!dir)
    dir = format_string("out/{}/{}/{}", p.name, target, p.target.id);
  p.build_dir = path(p.root, dir);
  if (same(p.build_dir, p.root))
    fail("buildDir must differ from the repository root");
  return p;
}

void print_project(const Project *p) {
  print_line("preset: {} ({})", p->name, p->config);
  print_line("target: {} / {} ({})", p->target.platform, p->target.id,
             p->target.cc);
  if (!same(p->target.id, p->host.id) ||
      !same(p->target.platform, p->host.platform))
    print_line("host: {} / {} ({})", p->host.platform, p->host.id, p->host.cc);
  if (p->target.triplet)
    print_line("vcpkg triplet: {}", p->target.triplet);
  for (size_t i = 0; i < p->used_environment.count; ++i)
    print_line("environment: {} ({})", p->used_environment.items[i].name,
               p->used_environment.items[i].origin);
  print_line("build directory: {}", p->build_dir);
}
