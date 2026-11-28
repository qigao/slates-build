#include "template.h"

#include <ctype.h>
#include <string.h>

Struct(SdkContext, (vstr, name), (vstr, version), (vstr, os), (vstr, arch),
       (vstr, abi), (vstr, config), (vstr, usage), (vstr, runtime),
       (vstr, cflags), (vstr, libs));

static const char *join_relative(const char *dir, const char *name) {
  return *name ? format_string("{}/{}", dir, name) : dir;
}

static const char *mapped_path(const Graph *g, const Module *m,
                               const char *source, bool directory) {
  for (size_t i = 0; i < g->count; ++i) {
    const Module *dep = g->modules[i];
    if (!dep->selected || dep->tools != m->tools || dep->test_only)
      continue;
    if (!directory &&
        (same(dep->artifact, source) || same(dep->import_lib, source)))
      return join_relative(artifact_install_dir(g, source), filename(source));
    for (size_t j = 0; j < dep->install_count; ++j) {
      const InstallMapping *map = &dep->install[j];
      if (map->component != INSTALL_DEV)
        continue;
      const char *from = map->from;
      const bool wildcard = strchr(from, '*') != NULL;
      const bool file = is_file(from) || producer(g, from);
      if (wildcard || file) {
        if (!same(parent(from), directory ? source : parent(source)))
          continue;
        if (directory)
          return map->to;
        if (same(from, source) ||
            (wildcard && filename_matches(filename(from), filename(source))))
          return join_relative(map->to, filename(source));
      } else if (inside(from, source)) {
        const char *tail = source + strlen(from);
        return join_relative(map->to, *tail == '/' ? tail + 1 : tail);
      }
    }
  }
  failf("{}: SDK export has no install mapping for '{}'", m->name, source);
}

static void pc_arg(Text *text, const char *name, const char *relative,
                   const Module *m) {
  if (strpbrk(name, "\r\n") || (relative && strpbrk(relative, "\r\n")))
    failf("{}: newline in SDK export argument", m->name);
  if (text->length)
    append(text, " ");
  if (!relative)
    append(text, "\"");
  const char *parts[] = {name, relative ? "${prefix}/" : "",
                         relative ? relative : ""};
  for (size_t i = 0; i < sizeof(parts) / sizeof(*parts); ++i)
    for (const char *c = parts[i]; *c; ++c) {
      if (*c == '\\' || *c == '"' ||
          (relative && (isspace((unsigned char)*c) || *c == '\'' || *c == '#')))
        append(text, "\\");
      append_n(text, c, 1);
    }
  if (!relative)
    append(text, "\"");
}

static const char *encode_json(const Json *json) {
  size_t size;
  char *encoded = json_serialize(json, &size);
  if (!encoded)
    fail("cannot serialize SDK metadata");
  const char *value = slice(encoded, size);
  json_serialize_free(encoded);
  return value;
}

static void pkg_path(Json *items, const char *relative) {
  json_add(items, json_create_string(format_string("../../../{}", relative)));
}

static void cmake_contents(Text *text, const char *value, bool path_value) {
  if (strpbrk(value, "\r\n"))
    fail("newline in CMake SDK export");
  if (path_value)
    append(text, "${CMAKE_CURRENT_LIST_DIR}/");
  for (const char *c = value; *c; ++c) {
    if (strchr("\\\";$", *c))
      append(text, "\\");
    append_n(text, c, 1);
  }
}

static void cmake_value(Text *text, const char *value, bool path_value) {
  append(text, "\"");
  cmake_contents(text, value, path_value);
  append(text, "\"");
}

static void cmake_list(Text *text, const char *property, const Json *values,
                       size_t skip) {
  if (json_array_size(values) <= skip)
    return;
  append(text, property);
  append(text, " \"");
  for (size_t i = skip; i < json_array_size(values); ++i) {
    const char *value = string(json_array_get(values, i), property);
    if (i != skip) append(text, ";");
    cmake_contents(text, value, starts(value, "../../../"));
  }
  append(text, "\"\n");
}

static void emit_cmake_target(const Graph *g, const Module *m, const Json *usage,
                               const char *manifest) {
  if (!m->cmake_target)
    return;
  Text text = {0};
  append(&text, format_string("if(TARGET {})\n  return()\nendif()\n", m->cmake_target));
  const char *kind = m->kind == MODULE_STATIC ? "STATIC"
                     : m->kind == MODULE_SHARED ? "SHARED" : "INTERFACE";
  append(&text, format_string("add_library({} {} IMPORTED)\nset_target_properties({} PROPERTIES\n",
                               m->cmake_target, kind, m->cmake_target));
  if (m->artifact) {
    append(&text, "IMPORTED_LOCATION ");
    cmake_value(&text, format_string("../../../{}", mapped_path(g, m, m->artifact, false)), true);
    append(&text, "\n");
  }
  if (m->import_lib) {
    append(&text, "IMPORTED_IMPLIB ");
    cmake_value(&text, format_string("../../../{}", mapped_path(g, m, m->import_lib, false)), true);
    append(&text, "\n");
  }
  cmake_list(&text, "INTERFACE_INCLUDE_DIRECTORIES", member(usage, "includeDirs"), 0);
  cmake_list(&text, "INTERFACE_COMPILE_OPTIONS", member(usage, "flags"), 0);
  cmake_list(&text, "INTERFACE_LINK_DIRECTORIES", member(usage, "libDirs"), 0);
  cmake_list(&text, "INTERFACE_LINK_LIBRARIES", member(usage, "libs"), m->artifact ? 1 : 0);
  cmake_list(&text, "INTERFACE_LINK_OPTIONS", member(usage, "linkFlags"), 0);
  const Json *defines = member(usage, "defines");
  if (json_object_size(defines)) {
    Json *values = own(json_create_array());
    for (size_t i = 0; i < json_object_size(defines); ++i) {
      const char *key = json_object_key(defines, i);
      const Json *value = json_object_value(defines, i);
      json_add(values, json_create_string(json_type(value) == JSON_NULL ? key
          : format_string("{}={}", key, string(value, m->name))));
    }
    cmake_list(&text, "INTERFACE_COMPILE_DEFINITIONS", values, 0);
  }
  Json *features = own(json_create_array());
  const char *standards[] = {"cStd", "cxxStd"};
  for (size_t i = 0; i < 2; ++i) {
    const char *standard = optional_string(get(&m->options, standards[i]), m->name);
    const char *prefix = i ? "c++" : "c";
    if (standard && starts(standard, prefix)) {
      const char *suffix = standard + strlen(prefix);
      if (*suffix && !suffix[strspn(suffix, "0123456789")])
        json_add(features, json_create_string(format_string("{}_std_{}", i ? "cxx" : "c", suffix)));
    }
  }
  cmake_list(&text, "INTERFACE_COMPILE_FEATURES", features, 0);
  append(&text, ")\n");
  const char *output = path(parent(manifest), format_string("{}-target.cmake", m->name));
  write_file(output, contents(&text), text.length);
}

static void sdk_link_usage(const Graph *g, const Module *m, const Usage *usage,
                           Json *dirs, Json *libs, Json *flags, Text *pc_dirs,
                           Text *pc_libs) {
  for (size_t i = 0; i < usage->lib_dirs.count; ++i) {
    const char *dir = mapped_path(g, m, usage->lib_dirs.items[i], true);
    pkg_path(dirs, dir);
    pc_arg(pc_dirs, "-L", dir, m);
  }
  for (size_t i = 0; i < usage->libs.count; ++i) {
    const char *lib = usage->libs.items[i];
    if (strchr(lib, '/') || strchr(lib, '\\')) {
      const char *file = mapped_path(g, m, lib, false);
      pkg_path(libs, file);
      pc_arg(pc_libs, "", file, m);
    } else {
      json_add(libs, json_create_string(lib));
      pc_arg(pc_libs,
             starts(lib, "-") || ends(lib, ".lib") ? lib
                                                   : format_string("-l{}", lib),
             NULL, m);
    }
  }
  for (size_t i = 0; i < usage->link_flags.count; ++i) {
    const char *flag = usage->link_flags.items[i];
    if (strstr(flag, g->project->root))
      failf("{}: SDK link flag includes a project path: '{}'", m->name, flag);
    json_add(flags, json_create_string(flag));
    pc_arg(pc_libs, flag, NULL, m);
  }
}

static void sdk_runtime(const Graph *g, const Module *m, Strings *visited,
                        Strings *files) {
  if (contains(visited, m->name) || m->test_only)
    return;
  push(visited, m->name);
  if (m->kind == MODULE_SHARED)
    unique(files, join_relative(artifact_install_dir(g, m->artifact),
                                filename(m->artifact)));
  if (m->kind == MODULE_EXECUTABLE)
    return;
  const char *directory = same(m->tools->os, "windows") ? "bin" : "lib";
  for (size_t i = 0; i < m->runtime_bins.count; ++i)
    unique(files, join_relative(directory, filename(m->runtime_bins.items[i])));
  const Strings *deps[] = {&m->public_deps, &m->private_deps, &m->runtime_deps};
  for (size_t j = 0; j < sizeof(deps) / sizeof(*deps); ++j)
    for (size_t i = 0; i < deps[j]->count; ++i)
      sdk_runtime(g, find_module(g, deps[j]->items[i]), visited, files);
}

void emit_sdk(const Graph *g, const Module *m, const BuildTemplate *json,
              const BuildTemplate *pc, const char *manifest,
              const char *pkgconfig) {
  Json *usage = own(json_create_object());
  Json *includes = json_create_array(), *defs = json_create_object();
  Json *cflags = json_create_array(), *dirs = json_create_array();
  Json *libs = json_create_array(), *link_flags = json_create_array();
  Json *runtime = own(json_create_array());
  json_set(usage, "includeDirs", includes);
  json_set(usage, "defines", defs);
  json_set(usage, "flags", cflags);
  json_set(usage, "libDirs", dirs);
  json_set(usage, "libs", libs);
  json_set(usage, "linkFlags", link_flags);
  Text pc_cflags = {0}, pc_dirs = {0}, pc_libs = {0};
  const Usage *u = &m->public_usage;
  for (size_t i = 0; i < u->includes.count; ++i) {
    const char *dir = mapped_path(g, m, u->includes.items[i], true);
    pkg_path(includes, dir);
    pc_arg(&pc_cflags, "-I", dir, m);
  }
  for (size_t i = 0; i < u->defines.count; ++i) {
    const Setting *d = &u->defines.items[i];
    const char *value =
        json_type(d->value) == JSON_NULL ? NULL : string(d->value, d->origin);
    json_set(defs, d->name,
             value ? json_create_string(value) : json_create_null());
    pc_arg(&pc_cflags,
           value ? format_string("-D{}={}", d->name, value)
                 : format_string("-D{}", d->name),
           NULL, m);
  }
  for (size_t i = 0; i < u->flags.count; ++i) {
    const char *flag = u->flags.items[i];
    if (strstr(flag, g->project->root))
      failf("{}: SDK compile flag includes a project path: '{}'", m->name,
            flag);
    json_add(cflags, json_create_string(flag));
    pc_arg(&pc_cflags, flag, NULL, m);
  }
  Strings closure = {0}, visited = {0}, files = {0};
  push(&closure, m->name);
  if (m->kind != MODULE_SHARED)
    link_modules(g, m, &closure);
  for (size_t i = 0; i < closure.count; ++i) {
    const Module *dep = find_module(g, closure.items[i]);
    const char *artifact = dep->import_lib ? dep->import_lib : dep->artifact;
    if (artifact) {
      const char *file = mapped_path(g, m, artifact, false);
      pkg_path(libs, file);
      pc_arg(&pc_libs, "", file, m);
    }
    sdk_link_usage(g, m, &dep->public_usage, dirs, libs, link_flags, &pc_dirs,
                   &pc_libs);
    if (dep->kind == MODULE_STATIC || dep->kind == MODULE_PREBUILT)
      sdk_link_usage(g, m, &dep->private_usage, dirs, libs, link_flags,
                     &pc_dirs, &pc_libs);
  }
  sdk_runtime(g, m, &visited, &files);
  for (size_t i = 0; i < files.count; ++i)
    pkg_path(runtime, files.items[i]);
  const SdkContext context = {
      .name = vstr_from_cstr(m->name),
      .version =
          vstr_from_cstr(m->export_version ? m->export_version : "0.0.0"),
      .os = vstr_from_cstr(m->tools->os),
      .arch = vstr_from_cstr(m->tools->arch),
      .abi = vstr_from_cstr(package_abi(m->tools)),
      .config = vstr_from_cstr(g->project->config),
      .usage = vstr_from_cstr(encode_json(usage)),
      .runtime = vstr_from_cstr(encode_json(runtime)),
      .cflags = vstr_from_cstr(contents(&pc_cflags)),
      .libs = vstr_from_cstr(format_string(
          "{}{}{}", contents(&pc_dirs),
          pc_dirs.length && pc_libs.length ? " " : "", contents(&pc_libs))),
  };
  static const cmeta_type_identity identity =
      CMETA_TYPE_ID_ATOM_INIT("slate.sdk.context");
  static const cmeta_type_desc storage = {.name = "SdkContext",
                                          .size = sizeof(SdkContext),
                                          .align = _Alignof(SdkContext),
                                          .kind = CMETA_T_OBJECT,
                                          .identity = &identity};
  const cmeta_data_desc *str = jinja_cmeta_vstr_data();
#define SDK_FIELD(field)                                                       \
  {                                                                            \
    "slate.sdk.context." #field, #field, offsetof(SdkContext, field),    \
        str                                                                    \
  }
  const cmeta_data_field_desc fields[] = {SDK_FIELD(name),   SDK_FIELD(version),
                                          SDK_FIELD(os),     SDK_FIELD(arch),
                                          SDK_FIELD(abi),    SDK_FIELD(config),
                                          SDK_FIELD(usage),  SDK_FIELD(runtime),
                                          SDK_FIELD(cflags), SDK_FIELD(libs)};
#undef SDK_FIELD
  const cmeta_data_struct_shape shape = {StructMeta(SdkContext), fields,
                                         sizeof(fields) / sizeof(*fields)};
  const cmeta_data_desc descriptor = {
      .struct_size = sizeof(descriptor),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "slate.sdk.context.data",
      .display_name = "SdkContext",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &storage,
      .shape = &shape};
  const Text pkg = render_template(json, &descriptor, &context, NULL);
  const Text metadata = render_template(pc, &descriptor, &context, NULL);
  write_file(manifest, contents(&pkg), pkg.length);
  write_file(pkgconfig, contents(&metadata), metadata.length);
  emit_cmake_target(g, m, usage, manifest);
}
